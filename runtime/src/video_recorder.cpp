/*
 * video_recorder.cpp - composed-present + SPU-mix recorder (see
 * video_recorder.h for the design, timing model and threading).
 *
 * Process spawning is the only OS-specific part:
 *   _WIN32                      CreateProcessW, CREATE_NO_WINDOW, explicit
 *                               handle list (only the pipe + log inherit)
 *   POSIX (Linux, macOS, BSD)   posix_spawnp
 *   anything else               popen()/pclose() — the portable fallback
 */
#include "video_recorder.h"
#include "audio_trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#  define VREC_SPAWN_WIN32 1
#elif defined(__unix__) || defined(__APPLE__)
#  include <fcntl.h>
#  include <signal.h>
#  include <spawn.h>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  include <pthread.h>
#  define VREC_SPAWN_POSIX 1
extern char **environ;
#else
#  define VREC_SPAWN_POPEN 1
#endif

namespace {

/* ===================================================================== */
/* Child process                                                          */
/* ===================================================================== */

struct Proc {
#if defined(VREC_SPAWN_WIN32)
    HANDLE proc = nullptr;
    HANDLE in_w = nullptr;
#elif defined(VREC_SPAWN_POSIX)
    pid_t pid = -1;
    int   in_fd = -1;
#else
    FILE *pipe = nullptr;
#endif
    bool running = false;
};

#if defined(VREC_SPAWN_WIN32)
std::wstring widen(const std::string &s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

/* CommandLineToArgvW quoting rules. */
void win_quote(std::wstring &cmd, const std::wstring &arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++bs; }
        if (i == arg.size()) { cmd.append(bs * 2, L'\\'); break; }
        if (arg[i] == L'"') { cmd.append(bs * 2 + 1, L'\\'); cmd += L'"'; }
        else { cmd.append(bs, L'\\'); cmd += arg[i]; }
    }
    cmd += L'"';
}
#endif

#if defined(VREC_SPAWN_POPEN)
std::string sh_quote(const std::string &a)
{
    std::string q = "'";
    for (char c : a) {
        if (c == '\'') q += "'\\''";
        else q += c;
    }
    q += "'";
    return q;
}
#endif

/* Spawn argv with stdin = a pipe we write (want_stdin) or the null device,
 * stdout+stderr appended to log_path. Never opens a console window. */
bool proc_spawn(const std::vector<std::string> &argv, bool want_stdin,
                const std::string &log_path, Proc *p, std::string *err)
{
    *p = Proc();
#if defined(VREC_SPAWN_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(widen(log_path).c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        *err = "cannot open ffmpeg log " + log_path;
        return false;
    }
    HANDLE in_r = nullptr, in_w = nullptr;
    if (want_stdin) {
        /* 8 MiB pipe: one 1080p BGRA frame fits, so a frame write rarely
         * waits on the encoder's read loop. */
        if (!CreatePipe(&in_r, &in_w, &sa, 8u << 20)) {
            CloseHandle(log);
            *err = "CreatePipe failed";
            return false;
        }
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    } else {
        in_r = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, 0, nullptr);
        if (in_r == INVALID_HANDLE_VALUE) in_r = nullptr;
    }
    std::wstring cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += L' ';
        win_quote(cmd, widen(argv[i]));
    }
    /* Inherit ONLY these handles: the runtime may hold other inheritable
     * handles, and a leaked pipe end would keep ffmpeg's stdin open. */
    HANDLE inherit[2];
    DWORD n_inherit = 0;
    if (in_r) inherit[n_inherit++] = in_r;
    inherit[n_inherit++] = log;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    bool attrs_ok = InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) &&
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherit, n_inherit * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_r;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    si.lpAttributeList = attrs_ok ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');
    const DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                        (attrs_ok ? EXTENDED_STARTUPINFO_PRESENT : 0);
    BOOL ok = CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                             flags, nullptr, nullptr, &si.StartupInfo, &pi);
    DWORD gle = ok ? 0 : GetLastError();
    if (attrs_ok) DeleteProcThreadAttributeList(attrs);
    if (in_r) CloseHandle(in_r);
    CloseHandle(log);
    if (!ok) {
        if (in_w) CloseHandle(in_w);
        char msg[160];
        std::snprintf(msg, sizeof msg, "cannot start ffmpeg (CreateProcess error %lu)",
                      (unsigned long)gle);
        *err = msg;
        return false;
    }
    CloseHandle(pi.hThread);
    p->proc = pi.hProcess;
    p->in_w = in_w;
    p->running = true;
    return true;
#elif defined(VREC_SPAWN_POSIX)
    int fds[2] = { -1, -1 };
    if (want_stdin && pipe(fds) != 0) { *err = "pipe() failed"; return false; }
    if (want_stdin) fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (want_stdin) {
        posix_spawn_file_actions_adddup2(&fa, fds[0], 0);
        posix_spawn_file_actions_addclose(&fa, fds[0]);
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_addopen(&fa, 1, log_path.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    std::vector<char *> cargv;
    for (const auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (want_stdin) close(fds[0]);
    if (rc != 0) {
        if (want_stdin) close(fds[1]);
        *err = std::string("cannot start ffmpeg: ") + std::strerror(rc);
        return false;
    }
    p->pid = pid;
    p->in_fd = want_stdin ? fds[1] : -1;
    p->running = true;
    return true;
#else
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += ' ';
        cmd += sh_quote(argv[i]);
    }
    cmd += " >>" + sh_quote(log_path) + " 2>&1";
    if (!want_stdin) cmd += " </dev/null";
    p->pipe = popen(cmd.c_str(), want_stdin ? "w" : "r");
    if (!p->pipe) { *err = "popen failed"; return false; }
    p->running = true;
    return true;
#endif
}

bool proc_write(Proc *p, const void *data, size_t n)
{
    const unsigned char *b = static_cast<const unsigned char *>(data);
#if defined(VREC_SPAWN_WIN32)
    while (n > 0) {
        DWORD chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n, wrote = 0;
        if (!WriteFile(p->in_w, b, chunk, &wrote, nullptr) || wrote == 0) return false;
        b += wrote; n -= wrote;
    }
    return true;
#elif defined(VREC_SPAWN_POSIX)
    while (n > 0) {
        ssize_t w = write(p->in_fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        b += w; n -= (size_t)w;
    }
    return true;
#else
    return std::fwrite(b, 1, n, p->pipe) == n;
#endif
}

/* Close our end of stdin (EOF to ffmpeg) and wait. Returns the exit code. */
int proc_finish(Proc *p)
{
    if (!p->running) return -1;
    p->running = false;
#if defined(VREC_SPAWN_WIN32)
    if (p->in_w) { CloseHandle(p->in_w); p->in_w = nullptr; }
    WaitForSingleObject(p->proc, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p->proc, &code);
    CloseHandle(p->proc);
    p->proc = nullptr;
    return (int)code;
#elif defined(VREC_SPAWN_POSIX)
    if (p->in_fd >= 0) { close(p->in_fd); p->in_fd = -1; }
    int status = 0;
    while (waitpid(p->pid, &status, 0) < 0 && errno == EINTR) {}
    p->pid = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#else
    if (!p->pipe) return -1;
    char sink[256];
    /* A read-mode popen (mux) must be drained before pclose. */
    while (std::fread(sink, 1, sizeof sink, p->pipe) > 0) {}
    int rc = pclose(p->pipe);
    p->pipe = nullptr;
    return rc;
#endif
}

std::string log_tail(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    long from = size > 300 ? size - 300 : 0;
    std::fseek(f, from, SEEK_SET);
    std::string s((size_t)(size - from), '\0');
    size_t got = std::fread(&s[0], 1, s.size(), f);
    std::fclose(f);
    s.resize(got);
    for (char &c : s) if (c == '\r' || c == '\n' || c == '"' || c == '\\') c = ' ';
    return s;
}

/* ===================================================================== */
/* Recorder state                                                         */
/* ===================================================================== */

struct Frame {
    std::vector<uint8_t> px;
};

constexpr double kGuestHz = PSX_GUEST_CPU_HZ;
constexpr uint64_t kCyclesPerSample = 768;   /* 33868800 / 44100 */
constexpr int kAudioRate = 44100;

struct Rec {
    std::mutex mu;
    std::condition_variable cv_writer;   /* frames ready / stop / flush */
    std::condition_variable cv_space;    /* pool buffer freed */
    std::condition_variable cv_state;    /* finished (done/error) */

    std::atomic<int>      state{VREC_IDLE};
    std::atomic<uint32_t> session{0};
    std::atomic<int>      backend{VREC_BACKEND_NONE};
    double (*rate_hint)(void) = nullptr;
    std::atomic<VrecBackend (*)(void)> backend_fn{nullptr};

    VrecOptions opt{};
    VrecStatus  st{};
    std::string ffmpeg, part_video, part_audio, log_path;

    std::vector<std::unique_ptr<Frame>> storage;
    std::vector<Frame *> free_list;
    std::deque<Frame *> ready;
    int  converting = 0;            /* buffers taken but not yet queued */
    bool have_first = false;
    std::vector<uint64_t> cycles;   /* guest stamp per captured frame, submit order */
    uint64_t submitted = 0;         /* frames queued to the writer */
    uint64_t lost = 0;
    bool flush_done = false;
    std::chrono::steady_clock::time_point stop_at{};
    int64_t nominal_num = 60, nominal_den = 1;

    /* writer-thread-owned */
    std::thread writer;
    Proc enc;
    bool enc_started = false;
    FILE *wav = nullptr;
    std::atomic<bool> audio_armed{false};   /* first frame located audio_start */
    int64_t audio_start = 0;        /* absolute SPU_OUT frame of the first present */
    int64_t audio_next = 0;         /* next absolute frame to append */
    uint64_t audio_written = 0;     /* frames in the WAV (incl. padding) */
    std::vector<int16_t> abuf;
};

Rec g;

bool active_state(int s)
{
    return s == VREC_RECORDING || s == VREC_STOPPING || s == VREC_FINALIZING;
}

uint64_t file_size_or0(const std::string &p)
{
    std::error_code ec;
    auto n = std::filesystem::file_size(std::filesystem::u8path(p), ec);
    return ec ? 0 : (uint64_t)n;
}

void remove_quiet(const std::string &p)
{
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(p), ec);
}

void wav_header(FILE *f, uint64_t frames)
{
    uint32_t data = (uint32_t)std::min<uint64_t>(frames * 4u, 0xFFFFFFFFull - 36u);
    uint32_t riff = 36u + data;
    uint32_t rate = (uint32_t)kAudioRate, byte_rate = rate * 4u;
    unsigned char h[44];
    std::memcpy(h, "RIFF", 4);
    for (int i = 0; i < 4; i++) h[4 + i] = (unsigned char)(riff >> (8 * i));
    std::memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0;
    h[20] = 1; h[21] = 0; h[22] = 2; h[23] = 0;
    for (int i = 0; i < 4; i++) h[24 + i] = (unsigned char)(rate >> (8 * i));
    for (int i = 0; i < 4; i++) h[28 + i] = (unsigned char)(byte_rate >> (8 * i));
    h[32] = 4; h[33] = 0; h[34] = 16; h[35] = 0;
    std::memcpy(h + 36, "data", 4);
    for (int i = 0; i < 4; i++) h[40 + i] = (unsigned char)(data >> (8 * i));
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, f);
}

bool truncate_file(FILE *f, uint64_t size)
{
    std::fflush(f);
#if defined(_WIN32)
    return _chsize_s(_fileno(f), (long long)size) == 0;
#elif defined(VREC_SPAWN_POSIX)
    return ftruncate(fileno(f), (off_t)size) == 0;
#else
    (void)f; (void)size;
    return false;
#endif
}

/* Writer thread: append SPU_OUT frames [audio_next, limit) that exist. */
void drain_audio(int64_t limit)
{
    if (!g.wav || !g.audio_armed.load(std::memory_order_acquire)) return;
    const int64_t head = (int64_t)audio_trace_tap_total(AUDIO_TAP_SPU_OUT);
    int64_t upto = std::min(head, limit);
    if (g.abuf.size() < 16384u * 2u) g.abuf.resize(16384u * 2u);
    while (g.audio_next < upto) {
        int64_t chunk = std::min<int64_t>(upto - g.audio_next, 16384);
        int64_t got;
        if (g.audio_next < 0) {
            /* The first present predates every rendered sample: silence. */
            chunk = std::min<int64_t>(chunk, -g.audio_next);
            std::fill(g.abuf.begin(), g.abuf.begin() + chunk * 2, (int16_t)0);
            got = chunk;
            std::lock_guard<std::mutex> lk(g.mu);
            g.st.audio_pad_frames += (uint64_t)chunk;
        } else {
            got = audio_trace_read_pcm(AUDIO_TAP_SPU_OUT, (uint64_t)g.audio_next,
                                       (uint64_t)chunk, g.abuf.data());
            if (got < 0) {
                /* Evicted (the ring holds ~95 s; only reachable if the writer
                 * stalled that long): keep the timeline, count the gap. */
                std::fill(g.abuf.begin(), g.abuf.begin() + chunk * 2, (int16_t)0);
                got = chunk;
                std::lock_guard<std::mutex> lk(g.mu);
                g.st.audio_pad_frames += (uint64_t)chunk;
            }
            if (got == 0) break;
        }
        std::fwrite(g.abuf.data(), 4, (size_t)got, g.wav);
        g.audio_next += got;
        g.audio_written += (uint64_t)got;
    }
    std::lock_guard<std::mutex> lk(g.mu);
    g.st.audio_frames = g.audio_written;
}

std::string rational(int64_t num, int64_t den)
{
    char b[64];
    std::snprintf(b, sizeof b, "%lld/%lld", (long long)num, (long long)den);
    return b;
}

std::vector<std::string> encoder_argv(const VrecOptions &o, int w, int h)
{
    char size[32];
    std::snprintf(size, sizeof size, "%dx%d", w, h);
    std::vector<std::string> a = {
        g.ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "bgra", "-s", size,
        "-framerate", rational(g.nominal_num, g.nominal_den),
        "-i", "-", "-an",
        "-vf", "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
        "-c:v", o.encoder,
    };
    /* -nostdin only stops ffmpeg reading the console for keys; the rawvideo
     * input still reads the pipe on fd 0 via "-i -". */
    const std::string enc = o.encoder;
    const bool nvenc = enc.find("_nvenc") != std::string::npos;
    if (o.preset[0]) { a.push_back("-preset"); a.push_back(o.preset); }
    else if (enc == "libx264" || enc == "libx265") { a.push_back("-preset"); a.push_back("veryfast"); }
    char q[16];
    std::snprintf(q, sizeof q, "%d", o.crf);
    if (enc == "libx264" || enc == "libx265") { a.push_back("-crf"); a.push_back(q); }
    else if (nvenc) {
        a.insert(a.end(), { "-rc", "vbr", "-cq", q, "-b:v", "0" });
    }
    char extra[16][128];
    int n = vrec_split_args(o.encoder_args, extra, 16);
    for (int i = 0; i < n; i++) a.push_back(extra[i]);
    a.insert(a.end(), { "-color_primaries", "bt709", "-color_trc", "bt709",
                        "-colorspace", "bt709", "-f", "nut", g.part_video });
    return a;
}

void set_error_locked(const std::string &msg)
{
    std::snprintf(g.st.error, sizeof g.st.error, "%s", msg.c_str());
    g.st.state = VREC_ERROR;
    g.state.store(VREC_ERROR);
    g.cv_space.notify_all();
    g.cv_state.notify_all();
}

void release_frame_locked(Frame *f)
{
    g.free_list.push_back(f);
    g.cv_space.notify_one();
}

/* Writer thread: finish encoding, cut the audio slice, mux. */
void finalize()
{
    std::vector<uint64_t> cycles;
    VrecOptions o;
    uint64_t presents, submitted, lost;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.state.store(VREC_FINALIZING);
        g.st.state = VREC_FINALIZING;
        cycles = g.cycles;
        o = g.opt;
        presents = g.st.presents;
        submitted = g.submitted;
        lost = g.lost;
        g.st.unflushed = presents > submitted + lost ? presents - submitted - lost : 0;
    }
    int enc_exit = g.enc_started ? proc_finish(&g.enc) : -1;
    const uint64_t n = cycles.size();
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.st.ffmpeg_exit = enc_exit;
    }
    if (n == 0 || !g.enc_started) {
        if (g.wav) { std::fclose(g.wav); g.wav = nullptr; }
        remove_quiet(g.part_audio);
        remove_quiet(g.part_video);
        std::lock_guard<std::mutex> lk(g.mu);
        set_error_locked("no frames were presented while recording "
                         "(is the window presenting? headless has no present)");
        return;
    }
    if (enc_exit != 0) {
        if (g.wav) { std::fclose(g.wav); g.wav = nullptr; }
        std::lock_guard<std::mutex> lk(g.mu);
        set_error_locked("ffmpeg encoder exited with code " + std::to_string(enc_exit) +
                         ": " + log_tail(g.log_path));
        return;
    }

    /* Output rate. */
    int64_t num = g.nominal_num, den = g.nominal_den;
    const double span_s = n >= 2 ? (double)(cycles[n - 1] - cycles[0]) / kGuestHz : 0.0;
    if (o.fps > 0.0) vrec_rationalize_fps(o.fps, &num, &den);
    else if (n >= 2 && span_s > 0.0) vrec_rationalize_fps((double)(n - 1) / span_s, &num, &den);
    const double fps = (double)num / (double)den;
    const double video_s = (double)n / fps;

    /* Cadence diagnostics. */
    double max_skew = 0.0, min_iv = 0.0, max_iv = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        double guest_t = (double)(cycles[i] - cycles[0]) / kGuestHz;
        double out_t = (double)i / fps;
        max_skew = std::max(max_skew, std::fabs(out_t - guest_t));
        if (i) {
            double iv = (double)(cycles[i] - cycles[i - 1]) * 1000.0 / kGuestHz;
            if (i == 1 || iv < min_iv) min_iv = iv;
            if (i == 1 || iv > max_iv) max_iv = iv;
        }
    }

    /* Audio slice: exactly the video duration from the first present. */
    uint64_t pad = 0, trim = 0;
    int64_t slip = 0;
    if (o.audio && g.wav) {
        const uint64_t need = (uint64_t)std::llround(video_s * kAudioRate);
        const int64_t target = g.audio_start + (int64_t)need;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        for (;;) {
            drain_audio(target);
            if (g.audio_next >= target) break;
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (g.audio_written < need) {
            /* The emulation stopped rendering (paused / exiting) before the
             * slice was complete: pad the tail so the track spans the video. */
            pad = need - g.audio_written;
            std::vector<int16_t> zero(8192 * 2, 0);
            uint64_t left = pad;
            while (left) {
                size_t c = (size_t)std::min<uint64_t>(left, 8192);
                std::fwrite(zero.data(), 4, c, g.wav);
                left -= c;
            }
            g.audio_written = need;
        } else if (g.audio_written > need) {
            trim = g.audio_written - need;
            truncate_file(g.wav, 44u + need * 4u);
            g.audio_written = need;
        }
        uint64_t h1 = 0, c1 = 0;
        if (audio_trace_spu_clock(&h1, &c1)) {
            const int64_t at_last = (int64_t)h1 +
                ((int64_t)cycles[n - 1] - (int64_t)c1) / (int64_t)kCyclesPerSample;
            const int64_t expect = g.audio_start +
                (int64_t)((cycles[n - 1] - cycles[0]) / kCyclesPerSample);
            slip = at_last - expect;
        }
        wav_header(g.wav, g.audio_written);
        std::fclose(g.wav);
        g.wav = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.st.fps = fps;
        g.st.video_seconds = video_s;
        g.st.guest_seconds = n >= 2 ? span_s + 1.0 / fps : 1.0 / fps;
        g.st.max_av_skew_ms = max_skew * 1000.0;
        g.st.min_present_ms = min_iv;
        g.st.max_present_ms = max_iv;
        g.st.audio_pad_frames += pad;
        g.st.audio_trim_frames = trim;
        g.st.audio_frames = g.audio_written;
        g.st.audio_slip_frames = slip;
    }

    /* Mux. The live encode stamped frame k at k / nominal seconds. A measured
     * rate that differs is applied losslessly to the stream copy by scaling
     * every timestamp with the setts bitstream filter (an input -r does NOT
     * retime a stream copy from a timestamped container — measured, see
     * video_recorder_e2e_test). */
    std::vector<std::string> a = { g.ffmpeg, "-hide_banner", "-nostdin",
                                   "-loglevel", "error", "-y" };
    const bool retime = num != g.nominal_num || den != g.nominal_den;
    a.insert(a.end(), { "-i", g.part_video });
    if (o.audio) a.insert(a.end(), { "-i", g.part_audio });
    a.insert(a.end(), { "-map", "0:v:0" });
    if (o.audio) a.insert(a.end(), { "-map", "1:a:0" });
    a.insert(a.end(), { "-c:v", "copy" });
    if (retime) {
        /* K = nominal / measured; 1/90000 output time base keeps every frame
         * within 6 us of k / fps whatever time base the encoder chose. */
        char k[64];
        std::snprintf(k, sizeof k, "%.15g",
                      ((double)g.nominal_num / (double)g.nominal_den) / fps);
        const std::string f = std::string("*TB*") + k + "/TB_OUT";
        a.push_back("-bsf:v");
        a.push_back("setts=time_base=1/90000:pts=PTS" + f + ":dts=DTS" + f +
                    ":duration=DURATION" + f);
    }
    if (o.audio) {
        char br[16];
        std::snprintf(br, sizeof br, "%dk", o.audio_bitrate_k);
        a.insert(a.end(), { "-c:a", "aac", "-b:a", br });
    }
    a.insert(a.end(), { "-movflags", "+faststart", o.path });
    Proc mux;
    std::string err;
    int mux_exit = -1;
    if (proc_spawn(a, false, g.log_path, &mux, &err))
        mux_exit = proc_finish(&mux);
    std::lock_guard<std::mutex> lk(g.mu);
    g.st.mux_exit = mux_exit;
    if (mux_exit != 0) {
        set_error_locked(err.empty()
            ? "mux failed with code " + std::to_string(mux_exit) + " (intermediates kept): " +
              log_tail(g.log_path)
            : err);
        return;
    }
    if (!o.keep_intermediates) {
        remove_quiet(g.part_video);
        remove_quiet(g.part_audio);
    }
    g.st.bytes_out = file_size_or0(o.path);
    g.st.state = VREC_DONE;
    g.state.store(VREC_DONE);
    g.cv_state.notify_all();
}

void writer_main(uint32_t session)
{
#if defined(VREC_SPAWN_POSIX)
    /* A dead encoder must surface as EPIPE on this thread, not kill the game. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, nullptr);
#endif
    for (;;) {
        Frame *f = nullptr;
        int w = 0, h = 0;
        bool finalize_now = false;
        {
            std::unique_lock<std::mutex> lk(g.mu);
            for (;;) {
                if (g.session.load() != session || g.state.load() == VREC_ERROR) return;
                if (!g.ready.empty()) { f = g.ready.front(); g.ready.pop_front(); break; }
                if (g.state.load() == VREC_STOPPING && g.converting == 0) {
                    const bool timed_out = std::chrono::steady_clock::now() - g.stop_at >
                                           std::chrono::seconds(3);
                    if (g.flush_done || timed_out) { finalize_now = true; break; }
                }
                g.cv_writer.wait_for(lk, std::chrono::milliseconds(20));
                if (g.state.load() == VREC_RECORDING && g.ready.empty()) {
                    lk.unlock();
                    drain_audio(INT64_MAX);   /* keep up even with no new frames */
                    lk.lock();
                }
            }
            w = g.st.width;
            h = g.st.height;
            g.st.queue_depth = (int)g.ready.size();
        }
        if (finalize_now) { finalize(); return; }

        bool ok = true;
        if (!g.enc_started) {
            std::string err;
            std::vector<std::string> argv = encoder_argv(g.opt, w, h);
            if (proc_spawn(argv, true, g.log_path, &g.enc, &err)) g.enc_started = true;
            else {
                std::lock_guard<std::mutex> lk(g.mu);
                release_frame_locked(f);
                set_error_locked(err);
                return;
            }
        }
        ok = proc_write(&g.enc, f->px.data(), f->px.size());
        drain_audio(INT64_MAX);
        std::lock_guard<std::mutex> lk(g.mu);
        release_frame_locked(f);
        if (!ok) {
            int code = proc_finish(&g.enc);
            g.st.ffmpeg_exit = code;
            set_error_locked("ffmpeg encoder stopped accepting frames (exit " +
                             std::to_string(code) + "): " + log_tail(g.log_path));
            if (g.wav) { std::fclose(g.wav); g.wav = nullptr; }
            return;
        }
        g.st.frames_written++;
        g.st.queue_depth = (int)g.ready.size();
        g.st.bytes_out = file_size_or0(g.part_video) + g.audio_written * 4u;
    }
}

/* BGRA src (any size, optional bottom-up) -> dst out_w x out_h top-down. */
void convert_frame(const uint8_t *src, int w, int h, int stride, int bottom_up,
                   uint8_t *dst, int ow, int oh)
{
    const size_t orow = (size_t)ow * 4u;
    if (w == ow && h == oh) {
        for (int y = 0; y < oh; y++) {
            const uint8_t *s = src + (size_t)(bottom_up ? h - 1 - y : y) * (size_t)stride;
            std::memcpy(dst + (size_t)y * orow, s, orow);
        }
        return;
    }
    int fx, fy, fw, fh;
    vrec_fit_rect(w, h, ow, oh, &fx, &fy, &fw, &fh);
    std::memset(dst, 0, orow * (size_t)oh);
    for (int y = 0; y < fh; y++) {
        int sy = (int)(((int64_t)y * h) / fh);
        if (bottom_up) sy = h - 1 - sy;
        const uint32_t *srow = reinterpret_cast<const uint32_t *>(src + (size_t)sy * (size_t)stride);
        uint32_t *drow = reinterpret_cast<uint32_t *>(dst + (size_t)(fy + y) * orow) + fx;
        for (int x = 0; x < fw; x++)
            drow[x] = srow[((int64_t)x * w) / fw];
    }
}

} /* namespace */

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

extern "C" {

void vrec_options_default(VrecOptions *o)
{
    std::memset(o, 0, sizeof *o);
    std::snprintf(o->encoder, sizeof o->encoder, "libx264");
    o->crf = 18;
    o->audio = 1;
    o->audio_bitrate_k = 192;
    o->queue_frames = 8;
}

int vrec_options_validate(VrecOptions *o, char *err, int errcap)
{
    auto fail = [&](const char *m) { std::snprintf(err, (size_t)errcap, "%s", m); return 0; };
    if (!o->path[0]) return fail("path is required");
    const size_t pl = std::strlen(o->path);
    if (pl < 4 || (std::strcmp(o->path + pl - 4, ".mp4") != 0 &&
                   std::strcmp(o->path + pl - 4, ".MP4") != 0))
        return fail("path must end in .mp4");
    if (!o->encoder[0]) std::snprintf(o->encoder, sizeof o->encoder, "libx264");
    for (const char *c = o->encoder; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '_' || *c == '-'))
            return fail("encoder must be an ffmpeg encoder name (e.g. libx264, h264_nvenc)");
    for (const char *c = o->preset; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-'))
            return fail("preset must be a plain preset name");
    if (o->crf < 0 || o->crf > 51) return fail("crf must be 0..51");
    if (o->fps < 0.0 || o->fps > 1000.0 || std::isnan(o->fps))
        return fail("fps must be 0 (measured present rate) or 1..1000");
    if (o->fps > 0.0 && o->fps < 1.0) return fail("fps must be 0 (measured present rate) or 1..1000");
    if (o->width < 0 || o->height < 0 || o->width > 16384 || o->height > 16384)
        return fail("width/height must be 0 (first frame) or 2..16384");
    if ((o->width == 0) != (o->height == 0)) return fail("give both width and height, or neither");
    if (o->width) { o->width = (o->width + 1) & ~1; o->height = (o->height + 1) & ~1; }
    if (o->audio_bitrate_k <= 0) o->audio_bitrate_k = 192;
    if (o->audio_bitrate_k > 640) return fail("audio_bitrate_k must be <= 640");
    if (o->queue_frames == 0) o->queue_frames = 8;
    if (o->queue_frames < 2 || o->queue_frames > 64) return fail("queue_frames must be 2..64");
    char tmp[16][128];
    if (vrec_split_args(o->encoder_args, tmp, 16) < 0) return fail("encoder_args: too many or too long");
    o->audio = o->audio ? 1 : 0;
    return 1;
}

int vrec_split_args(const char *s, char out[][128], int max)
{
    int n = 0;
    if (!s) return 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (n >= max) return -1;
        int len = 0;
        while (*s && *s != ' ' && *s != '\t') {
            if (len >= 127) return -1;
            out[n][len++] = *s++;
        }
        out[n][len] = '\0';
        n++;
    }
    return n;
}

void vrec_fit_rect(int sw, int sh, int dw, int dh, int *x, int *y, int *w, int *h)
{
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) { *x = *y = *w = *h = 0; return; }
    /* Width-limited unless the source is relatively taller than the target. */
    int64_t fw = dw, fh = ((int64_t)dw * sh + sw / 2) / sw;
    if (fh > dh) { fh = dh; fw = ((int64_t)dh * sw + sh / 2) / sh; }
    if (fw < 1) fw = 1;
    if (fh < 1) fh = 1;
    *w = (int)fw; *h = (int)fh;
    *x = (int)((dw - fw) / 2);
    *y = (int)((dh - fh) / 2);
}

void vrec_rationalize_fps(double fps, int64_t *num, int64_t *den)
{
    if (!(fps > 0.0)) { *num = 60; *den = 1; return; }
    const double r = std::round(fps);
    if (std::fabs(fps - r) < 0.002) { *num = (int64_t)r; *den = 1; return; }
    const double n1001 = std::round(fps * 1.001);
    if (std::fabs(fps - n1001 / 1.001) < 0.002) { *num = (int64_t)n1001 * 1000; *den = 1001; return; }
    *num = std::llround(fps * 1000.0);
    *den = 1000;
}

const char *video_recorder_state_name(VrecState s)
{
    switch (s) {
    case VREC_IDLE: return "idle";
    case VREC_RECORDING: return "recording";
    case VREC_STOPPING: return "stopping";
    case VREC_FINALIZING: return "finalizing";
    case VREC_DONE: return "done";
    case VREC_ERROR: return "error";
    }
    return "unknown";
}

void video_recorder_set_backend(VrecBackend b) { g.backend.store((int)b); }
void video_recorder_set_backend_fn(VrecBackend (*fn)(void)) { g.backend_fn.store(fn); }
VrecBackend video_recorder_backend(void)
{
    VrecBackend (*fn)(void) = g.backend_fn.load();
    return fn ? fn() : (VrecBackend)g.backend.load();
}
void video_recorder_set_rate_hint_fn(double (*fn)(void))
{
    std::lock_guard<std::mutex> lk(g.mu);
    g.rate_hint = fn;
}

int video_recorder_find_ffmpeg(const char *explicit_path, char *out, int outcap)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    auto is_file = [&](const std::string &p) {
        return !p.empty() && fs::is_regular_file(fs::u8path(p), ec);
    };
    auto put = [&](const std::string &p) {
        std::snprintf(out, (size_t)outcap, "%s", p.c_str());
        return 1;
    };
    if (explicit_path && explicit_path[0])
        return is_file(explicit_path) ? put(explicit_path) : 0;
    if (const char *env = std::getenv("PSX_FFMPEG"))
        if (env[0]) return is_file(env) ? put(env) : 0;
    const char *path = std::getenv("PATH");
    if (!path) return 0;
#if defined(_WIN32)
    const char sep = ';';
    const char *names[] = { "ffmpeg.exe", "ffmpeg" };
#else
    const char sep = ':';
    const char *names[] = { "ffmpeg", nullptr };
#endif
    std::string all = path;
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t end = all.find(sep, pos);
        if (end == std::string::npos) end = all.size();
        std::string dir = all.substr(pos, end - pos);
        if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
            dir = dir.substr(1, dir.size() - 2);
        if (!dir.empty()) {
            for (const char *nm : names) {
                if (!nm) continue;
                std::string cand = dir + "/" + nm;
                if (is_file(cand)) return put(cand);
            }
        }
        pos = end + 1;
    }
    return 0;
}

int video_recorder_start(const VrecOptions *in, char *err, int errcap)
{
    auto fail = [&](const std::string &m) {
        std::snprintf(err, (size_t)errcap, "%s", m.c_str());
        return 0;
    };
    if (!in) return fail("no options");
    VrecOptions o = *in;
    if (!vrec_options_validate(&o, err, errcap)) return 0;

    std::unique_lock<std::mutex> lk(g.mu);
    const int cur = g.state.load();
    if (active_state(cur))
        return fail(std::string("already ") + video_recorder_state_name((VrecState)cur) +
                    " (stop it and wait for done/error first)");
    switch (video_recorder_backend()) {
    case VREC_BACKEND_NONE:
        return fail("no present surface to record: --headless has no window; launch with "
                    "--offscreen-window (alias --hidden-window) to record off the desktop");
    case VREC_BACKEND_VULKAN:
        return fail("the Vulkan backend has no present readback hook; record with "
                    "--renderer opengl or software");
    default: break;
    }
    char ff[VREC_PATH_MAX];
    if (!video_recorder_find_ffmpeg(o.ffmpeg, ff, (int)sizeof ff)) {
        if (o.ffmpeg[0]) return fail(std::string("ffmpeg not found at ") + o.ffmpeg);
        const char *env = std::getenv("PSX_FFMPEG");
        if (env && env[0]) return fail(std::string("ffmpeg not found at $PSX_FFMPEG=") + env);
        return fail("ffmpeg not found on PATH (install ffmpeg, set PSX_FFMPEG, or pass ffmpeg=<path>)");
    }

    /* Previous recording's writer has finished (state done/error/idle). */
    if (g.writer.joinable()) {
        lk.unlock();
        g.writer.join();
        lk.lock();
    }
    const std::string base = o.path;
    std::string part_video = base + ".part.video.nut";
    std::string part_audio = base + ".part.audio.wav";
    std::string log_path = base + ".ffmpeg.log";
    {
        /* Probe the destination directory with the log we will need anyway. */
        FILE *lf = std::fopen(log_path.c_str(), "wb");
        if (!lf) return fail("cannot write next to " + base + " (directory missing or read-only)");
        std::fclose(lf);
    }
    FILE *wav = nullptr;
    if (o.audio) {
        wav = std::fopen(part_audio.c_str(), "w+b");
        if (!wav) return fail("cannot create " + part_audio);
        wav_header(wav, 0);
    }

    g.opt = o;
    g.ffmpeg = ff;
    g.part_video = part_video;
    g.part_audio = part_audio;
    g.log_path = log_path;
    std::memset(&g.st, 0, sizeof g.st);
    std::snprintf(g.st.path, sizeof g.st.path, "%s", o.path);
    g.st.ffmpeg_exit = -1;
    g.st.mux_exit = -1;
    g.storage.clear();
    g.free_list.clear();
    g.ready.clear();
    g.converting = 0;
    g.have_first = false;
    g.cycles.clear();
    g.cycles.reserve(60 * 600);
    g.submitted = 0;
    g.lost = 0;
    g.flush_done = false;
    g.enc = Proc();
    g.enc_started = false;
    g.wav = wav;
    g.audio_armed.store(false);
    g.audio_start = g.audio_next = 0;
    g.audio_written = 0;
    const uint32_t session = g.session.load() + 1u;
    g.session.store(session);
    g.st.session = session;
    g.st.state = VREC_RECORDING;
    g.state.store(VREC_RECORDING);
    g.writer = std::thread(writer_main, session);
    return 1;
}

int video_recorder_stop(char *err, int errcap)
{
    std::lock_guard<std::mutex> lk(g.mu);
    const int cur = g.state.load();
    if (cur != VREC_RECORDING) {
        std::snprintf(err, (size_t)errcap, "not recording (state %s)",
                      video_recorder_state_name((VrecState)cur));
        return 0;
    }
    g.stop_at = std::chrono::steady_clock::now();
    g.state.store(VREC_STOPPING);
    g.st.state = VREC_STOPPING;
    /* Nothing captured means nothing pending in any backend. */
    if (g.st.presents == 0) g.flush_done = true;
    g.cv_writer.notify_all();
    g.cv_space.notify_all();
    return 1;
}

VrecState video_recorder_wait(int timeout_ms)
{
    std::unique_lock<std::mutex> lk(g.mu);
    auto pred = [] { return !active_state(g.state.load()); };
    if (timeout_ms < 0) g.cv_state.wait(lk, pred);
    else g.cv_state.wait_for(lk, std::chrono::milliseconds(timeout_ms), pred);
    VrecState s = (VrecState)g.state.load();
    if (!active_state(s) && g.writer.joinable()) {
        lk.unlock();
        g.writer.join();
    }
    return s;
}

void video_recorder_get_status(VrecStatus *out)
{
    std::lock_guard<std::mutex> lk(g.mu);
    *out = g.st;
    out->state = (VrecState)g.state.load();
    out->session = g.session.load();
    if (out->state == VREC_RECORDING || out->state == VREC_STOPPING) {
        const size_t n = g.cycles.size();
        if (n >= 2 && g.cycles[n - 1] > g.cycles[0]) {
            const double span = (double)(g.cycles[n - 1] - g.cycles[0]) / kGuestHz;
            out->fps = (double)(n - 1) / span;
            out->guest_seconds = span;
        } else {
            out->fps = (double)g.nominal_num / (double)g.nominal_den;
        }
        out->video_seconds = out->fps > 0 ? (double)out->frames_written / out->fps : 0.0;
        out->queue_depth = (int)g.ready.size();
    }
}

void video_recorder_shutdown(void)
{
    char err[128];
    if (g.state.load() == VREC_RECORDING) video_recorder_stop(err, (int)sizeof err);
    if (video_recorder_backend() != VREC_BACKEND_OPENGL) {
        /* Synchronous readback: nothing can be in flight. */
        uint32_t session = 0;
        if (video_recorder_flush_wanted(&session)) video_recorder_flush_done(session);
    }
    if (active_state(g.state.load())) video_recorder_wait(120000);
    if (g.writer.joinable() && !active_state(g.state.load())) g.writer.join();
}

int video_recorder_present_poll(uint32_t *session)
{
    const int s = g.state.load(std::memory_order_acquire);
    if (s != VREC_RECORDING && s != VREC_STOPPING) return VREC_PRESENT_IDLE;
    std::lock_guard<std::mutex> lk(g.mu);
    const int s2 = g.state.load();
    if (session) *session = g.session.load();
    if (s2 == VREC_RECORDING) {
        g.st.presents++;
        return VREC_PRESENT_CAPTURE;
    }
    if (s2 == VREC_STOPPING && !g.flush_done) return VREC_PRESENT_FLUSH;
    return VREC_PRESENT_IDLE;
}

void video_recorder_flush_done(uint32_t session)
{
    std::lock_guard<std::mutex> lk(g.mu);
    if (session != g.session.load()) return;
    g.flush_done = true;
    g.cv_writer.notify_all();
}

int video_recorder_flush_wanted(uint32_t *session)
{
    std::lock_guard<std::mutex> lk(g.mu);
    if (g.state.load() != VREC_STOPPING || g.flush_done) return 0;
    if (session) *session = g.session.load();
    return 1;
}

void video_recorder_note_lost_frame(uint32_t session)
{
    std::lock_guard<std::mutex> lk(g.mu);
    if (session != g.session.load()) return;
    g.lost++;
    g.st.dropped++;
}

void video_recorder_submit_frame(uint32_t session, const void *bgra, int w, int h,
                                 int stride, int bottom_up, uint64_t guest_cycle)
{
    if (!bgra || w <= 0 || h <= 0) { video_recorder_note_lost_frame(session); return; }
    Frame *f = nullptr;
    int ow = 0, oh = 0;
    {
        std::unique_lock<std::mutex> lk(g.mu);
        auto live = [&] {
            const int s = g.state.load();
            return g.session.load() == session && (s == VREC_RECORDING || s == VREC_STOPPING);
        };
        if (!live()) return;
        if (!g.have_first) {
            g.have_first = true;
            g.st.width = g.opt.width ? g.opt.width : ((w + 1) & ~1);
            g.st.height = g.opt.height ? g.opt.height : ((h + 1) & ~1);
            const size_t bytes = (size_t)g.st.width * (size_t)g.st.height * 4u;
            for (int i = 0; i < g.opt.queue_frames; i++) {
                g.storage.emplace_back(new Frame());
                g.storage.back()->px.resize(bytes);
                g.free_list.push_back(g.storage.back().get());
            }
            const double hint = g.rate_hint ? g.rate_hint() : 60.0;
            if (g.opt.fps > 0.0) vrec_rationalize_fps(g.opt.fps, &g.nominal_num, &g.nominal_den);
            else vrec_rationalize_fps(hint > 0.0 ? hint : 60.0, &g.nominal_num, &g.nominal_den);
            /* Audio anchor: the SPU frame rendered at this present's cycle. */
            uint64_t ah = 0, ac = 0;
            if (audio_trace_spu_clock(&ah, &ac)) {
                g.audio_start = (int64_t)ah +
                    ((int64_t)guest_cycle - (int64_t)ac) / (int64_t)kCyclesPerSample;
                g.st.audio_anchor = 1;
            } else {
                g.audio_start = (int64_t)audio_trace_tap_total(AUDIO_TAP_SPU_OUT);
                g.st.audio_anchor = 0;
            }
            g.audio_next = g.audio_start;
            g.audio_armed.store(true, std::memory_order_release);
        }
        ow = g.st.width;
        oh = g.st.height;
        if (g.free_list.empty()) {
            const auto t0 = std::chrono::steady_clock::now();
            g.st.backpressure_waits++;
            g.cv_space.wait(lk, [&] { return !live() || !g.free_list.empty(); });
            g.st.backpressure_ms += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            if (!live()) return;
        }
        f = g.free_list.back();
        g.free_list.pop_back();
        g.converting++;
        if (w != ow || h != oh) g.st.resized_frames++;
    }
    convert_frame(static_cast<const uint8_t *>(bgra), w, h, stride, bottom_up,
                  f->px.data(), ow, oh);
    std::lock_guard<std::mutex> lk(g.mu);
    g.converting--;
    if (g.session.load() != session || g.state.load() == VREC_ERROR) {
        release_frame_locked(f);
        return;
    }
    g.ready.push_back(f);
    g.cycles.push_back(guest_cycle);
    g.submitted++;
    g.cv_writer.notify_one();
}

} /* extern "C" */
