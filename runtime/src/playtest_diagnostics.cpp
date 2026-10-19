/* Shared playtest diagnostics: deterministic input recording and verified
 * replay at the engine boundary. No title function or guest layout is hooked.
 * At each guest VBlank (psx_mod_register_vblank_plugin) the pad state
 * already latched into SIO is, by construction, the state the game reads for
 * that frame: the host samples pads into SIO once per frame boundary, after
 * the VBlank. Each row stores that state plus a cheap guest-RAM digest.
 *
 * Replay installs offline controller sources that return the row the NEXT
 * VBlank will verify, so the recorded SIO state reaches the game at the same
 * boundary it did when recorded. Every VBlank then checks pads and digest and
 * stops at the first divergence. Host text/files are presentation only. */
#include "mod_plugins.h"
#include "mod_netplay.h"
#include "mod_runtime.h"
#include "execution_profile.h"
#include "host_osd.h"
#include "memcard.h"
#include "savestate.h"
#include "sio.h"
#include "playtest_diagnostic_paths.h"
#include "playtest_diagnostic_writer.h"
#include "playtest_diagnostic_telemetry.h"
#include "cdrom.h"
#include "render_pass.h"
#include "gpu.h"
#include "video_telemetry.h"
#include "psx_sha256_file.h"
#include <array>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#ifndef PSX_DIAGNOSTIC_BUILD_ID
#define PSX_DIAGNOSTIC_BUILD_ID "diagnostic-test-build"
#endif
#ifndef PSX_DIAGNOSTIC_GAME_ID
#define PSX_DIAGNOSTIC_GAME_ID "fixture-game"
#endif
#ifndef PSX_DIAGNOSTIC_MOD_ID
#define PSX_DIAGNOSTIC_MOD_ID "psxrecomp.diagnostics"
#endif
#define PSX_DIAGNOSTIC_INPUT_FORMAT "psxrecomp-input-v2"

namespace {
using psx_playtest_diagnostic::Row;
using psx_playtest_diagnostic::RowColumns;
constexpr uint32_t RamBase = 0x80000000u, RamWords = 0x200000u / 4u;
// A rotating phase covers every main-RAM word every StrideWords VBlanks while
// keeping the per-frame read bounded (16 KiB, never the whole 2 MiB).
constexpr uint32_t StrideWords = 128, DigestWords = RamWords / StrideWords;
constexpr unsigned ColButtons = 1, ColPad = 3, ColPadStride = 5, ColDigest = 13;
constexpr unsigned ColConnected = 14;

psx_playtest_diagnostic::Writer<> recording;
psx_playtest_diagnostic::Writer<4096, psx_playtest_diagnostic::TelemetryRow> telemetry;
std::chrono::steady_clock::time_point telemetry_start, telemetry_last;
unsigned incident;
bool incident_held, telemetry_failed;
std::filesystem::path recording_folder;
std::ifstream replay;
Row pending{};               // replay row the next VBlank verifies
bool pending_valid;
uint32_t sequence;
uint32_t osd_sequence;
bool started, replaying, replay_failed, recording_failed, owns_sources, osd_enabled;
bool baseline_loaded;
bool input_only, legacy_route;
unsigned legacy_connected;
unsigned last_generation;
std::string replay_path, baseline_path;
std::vector<uint8_t> expected_baseline, pending_baseline;
bool pending_baseline_unavailable;
struct Stats { uint64_t ns = 0, max = 0, count = 0, start = 0, slow = 0; } stats;

std::string build_identity() {
    // The executable hash qualifies archive builds, dirty trees, compiler
    // changes and signed release binaries. Compute once, outside steady state.
    static const std::string identity=[] {
        uint8_t digest[32]{};
        if(psx_sha256_file(psx_playtest_diagnostic_executable(),digest)!=PsxFileHashResult::Ok)
            throw std::runtime_error("cannot hash diagnostic executable identity");
        std::string value=PSX_DIAGNOSTIC_BUILD_ID "/sha256-";
        constexpr char hex[]="0123456789abcdef";
        for(uint8_t byte:digest) {value+=hex[byte>>4];value+=hex[byte&15];}
        return value;
    }();
    return identity;
}

#ifndef PSX_DIAGNOSTIC_TEST
std::filesystem::path application_directory() {
    // Same directory the engine anchors settings.toml, mods and saves to.
    const auto& root = PSXRecompV4::mod_runtime_root();
    if (!root.empty() && root.has_parent_path()) return root.parent_path();
    return psx_playtest_diagnostic_application_directory();
}
#endif

void recording_location(const std::string& message) {
    /* Independent of configured memory-card paths, mods paths and cwd. */
    try {
        auto root = application_directory();
        if (root.empty()) return;
        std::ofstream out(root / "diagnostics-location.txt", std::ios::trunc);
        out << message << "\n";
    } catch (...) { /* Recording must not depend on this convenience pointer. */ }
}

void report_stats() {
    if (!stats.count && !stats.start) return;
    // The session-start tick (folder, cards, settings) is reported separately.
    fprintf(stderr, "psxrecomp.diagnostics: %llu VBlank rows, game-thread cost mean %.1f us max %.1f us "
            "(%llu over 1 ms); session start %.1f us\n",
            (unsigned long long)stats.count, stats.count ? stats.ns / 1000.0 / stats.count : 0.0,
            stats.max / 1000.0, (unsigned long long)stats.slow, stats.start / 1000.0);
    fflush(stderr);
    stats = {};
}

void close_recording() {
    recording.close();
    telemetry.close();
    if (!telemetry && !telemetry_failed) {
        telemetry_failed = true;
        fprintf(stderr, "psxrecomp.diagnostics: telemetry close failed: %s\n", telemetry.error_text());
    }
    report_stats();
    if (!recording && !recording_failed) {
        recording_failed = true;
        recording_location(std::string("Recording FAILED: ") + recording.error_text());
        fprintf(stderr, "psxrecomp.diagnostics: %s\n", recording.error_text());
        fflush(stderr);
    }
}

std::string plan_identity() {
    auto portable = PSXRecompV4::mod_runtime_plan_fingerprint_portable();
    return portable.empty() ? PSXRecompV4::mod_runtime_fingerprint() : portable;
}

uint32_t state_digest(uint32_t seq) {
    uint32_t h = 2166136261u;
    const uint32_t phase = seq % StrideWords;
    for (uint32_t i = 0; i < DigestWords; ++i) {
        h ^= psx_mod_read_word(RamBase + ((i * StrideWords + phase) << 2));
        h *= 16777619u;
        h ^= h >> 15;
    }
    return h;
}

/* The pad state the game reads for the frame this VBlank starts. */
Row sample(uint32_t seq) {
    Row row{};
    row[0] = seq;
    for (unsigned seat = 0; seat < 2; ++seat) {
        uint8_t sticks[4] = {0x80, 0x80, 0x80, 0x80};
        sio_get_pad_sticks((int)seat, sticks);
        row[ColButtons + seat] = sio_get_pad_buttons_slot((int)seat);
        row[ColConnected + seat] = sio_get_pad_connected((int)seat) ? 1 : 0;
        row[ColPad + seat * ColPadStride] = sio_get_pad_analog((int)seat) ? 1 : 0;
        for (unsigned i = 0; i < 4; ++i) row[ColPad + seat * ColPadStride + 1 + i] = sticks[i];
    }
    row[ColDigest] = state_digest(seq);
    return row;
}

bool parse_row(const std::string& line, Row& row) {
    if (line.compare(0, 2, "N ")) return false;
    std::string fields = line.substr(2);
    for (char& c : fields) if (c == ',') c = ' ';
    std::istringstream in(fields);
    const unsigned columns = legacy_route ? ColConnected : RowColumns;
    row.fill(0);
    for (unsigned column = 0; column < columns; ++column) if (!(in >> row[column])) return false;
    if (legacy_route) {
        row[ColConnected] = legacy_connected & 1u;
        row[ColConnected + 1] = (legacy_connected >> 1) & 1u;
    }
    in >> std::ws;
    if (!in.eof() || row[0] < 0 || row[0] > UINT32_MAX || row[ColDigest] < 0 || row[ColDigest] > UINT32_MAX) return false;
    for (unsigned i = ColButtons; i < ColButtons + 2; ++i) if (row[i] < 0 || row[i] > 65535) return false;
    for (unsigned seat = 0; seat < 2; ++seat) {
        if (row[ColConnected + seat] < 0 || row[ColConnected + seat] > 1) return false;
        const unsigned base = ColPad + seat * ColPadStride;
        if (row[base] < 0 || row[base] > 1) return false;
        for (unsigned i = 1; i <= 4; ++i) if (row[base + i] < 0 || row[base + i] > 255) return false;
    }
    return true;
}

int source(unsigned seat, PSXModControllerState *state) {
    if (!replaying || !pending_valid) return 0; // declined: neutral until the baseline/first row
    psx_mod_set_controller_source_connected(seat, (int)pending[ColConnected + seat]);
    const unsigned base = ColPad + seat * ColPadStride;
    state->buttons = (uint32_t)pending[ColButtons + seat];
    state->analog = (uint32_t)pending[base];
    state->lx = (uint32_t)pending[base + 1]; state->ly = (uint32_t)pending[base + 2];
    state->rx = (uint32_t)pending[base + 3]; state->ry = (uint32_t)pending[base + 4];
    return 1;
}
int first_source(PSXModControllerState *state) { return source(0, state); }
int second_source(PSXModControllerState *state) { return source(1, state); }
void release_sources() {
    if (!owns_sources) return;
    psx_mod_set_controller_source(0, nullptr); psx_mod_set_controller_source(1, nullptr);
    owns_sources = false;
}

void fail(const char *reason) {
    fprintf(stderr, "psxrecomp.diagnostics: replay stopped at sequence %u: %s\n", sequence, reason);
    fflush(stderr);
    psx_mod_counter_add("psxrecomp.diagnostics.replay-failed", 1);
    replay.close(); replaying = false; replay_failed = true; pending_valid = false;
    release_sources();
}

/* Load the next recorded row. False at end of recording (including an
 * interrupted recording's torn last line). */
bool next_row() {
    std::string line;
    if (!std::getline(replay, line)) return false;
    if (replay.eof()) return false; // no terminating newline: a torn final row
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!parse_row(line, pending)) {
        fail("malformed input row");
        return true;
    }
    pending_valid = true;
    return true;
}

void begin_replay() {
    if (psx_mod_netplay_is_active()) { fail("offline replay only"); return; }
    replay.open(std::filesystem::u8path(replay_path), std::ios::binary);
    std::string format, execution, plan, build;
    auto line = [&](std::string& out) {
        if (!std::getline(replay, out)) return false;
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    };
    if (!line(format) || !line(execution) || !line(plan) || !line(build)) {
        fail("recording header is incomplete"); return;
    }
    const std::string want_format = "# " PSX_DIAGNOSTIC_INPUT_FORMAT,
        want_execution = std::string("# execution ") + PSX_EXECUTION_ID,
        want_plan = "# plan " + plan_identity(),
        want_build = "# build " + build_identity();
    if (input_only) {
        legacy_route = format == "# psxrecomp-input-v1";
        if (format != want_format && !legacy_route) { fail("unsupported input route format"); return; }
        if (legacy_route && legacy_connected > 3) { fail("v1 input route requires explicit legacy controller presence"); return; }
        fprintf(stderr,"psxrecomp.diagnostics: INPUT-ONLY ROUTE: build/mod/RAM verification unavailable; this is not a verified replay\n");
    } else if (format != want_format || execution != want_execution || plan != want_plan || build != want_build) {
        fprintf(stderr, "psxrecomp.diagnostics: replay header mismatches: format=%d execution=%d plan=%d build=%d\n",
                format != want_format, execution != want_execution, plan != want_plan, build != want_build);
        fprintf(stderr, "psxrecomp.diagnostics: active plan %s\n", plan_identity().c_str());
        fail("recording version/build/mod settings do not match"); return;
    }
    replaying = true;
    if (!next_row() || !pending_valid) { if (!replay_failed) fail("recording has no input rows"); return; }
    fprintf(stderr, "psxrecomp.diagnostics: offline input replay started\n");
    fflush(stderr);
}

/* A restore happens mid-frame, so a recorded segment's first row holds whatever
 * the live host sampled between the restore and the first VBlank. A replayed
 * load runs before the first frame boundary, so no source sample precedes
 * that VBlank: seed SIO with the recorded row instead. */
void seed_first_row() {
    if (!pending_valid) return;
    for (unsigned seat = 0; seat < 2; ++seat) {
        const unsigned base = ColPad + seat * ColPadStride;
        sio_set_pad_state_slot((int)seat, (uint16_t)pending[ColButtons + seat]);
        sio_set_pad_sticks((int)seat, (uint8_t)pending[base + 1], (uint8_t)pending[base + 2],
                           (uint8_t)pending[base + 3], (uint8_t)pending[base + 4]);
        sio_set_pad_connected((int)seat, (int)pending[ColConnected + seat]);
    }
}

std::string session_json(bool netplay) {
    std::ostringstream out;
    out << "{\n  \"format_version\": 1,\n  \"input_format\": \"" PSX_DIAGNOSTIC_INPUT_FORMAT "\",\n"
        << "  \"telemetry_format\": \"psxrecomp-telemetry-v2\",\n"
        << "  \"visual_verification\": \"unavailable: no independent pixel comparison\",\n"
        << "  \"game_id\": \"" PSX_DIAGNOSTIC_GAME_ID "\",\n"
        << "  \"boundary\": \"guest-vblank-sio-pad-state\",\n"
        << "  \"digest\": \"fnv-mix over " << DigestWords << " main-RAM words per VBlank, stride "
        << StrideWords << " words, rotating phase = sequence % " << StrideWords << "\",\n"
        << "  \"writer\": \"bounded-background-v1\",\n  \"flush_interval_ms\": 100,\n"
        << "  \"netplay\": " << (netplay ? "true" : "false")
        << ",\n  \"execution\": \"" << PSX_EXECUTION_ID
        << "\",\n  \"build\": \"" << build_identity() << "\"";
    return out.str();
}

void begin_recording() {
    started = true;
    try {
        // Register after startup logging is initialized, so accepted records
        // drain and final close errors are reported before that logger exits.
        // The writer destructor remains an idempotent early-exit fallback.
        static const bool cleanup_registered = std::atexit(close_recording) == 0;
        if (!cleanup_registered) throw std::runtime_error("cannot register diagnostic shutdown");
        auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto folder = psx_playtest_diagnostic_session_directory(
            application_directory(), PSX_DIAGNOSTIC_GAME_ID, "session-" + std::to_string(stamp));
        recording_folder = folder;
        recording.open(folder / "inputs.csv", std::string("# " PSX_DIAGNOSTIC_INPUT_FORMAT "\n# execution ") +
            PSX_EXECUTION_ID + "\n# plan " + plan_identity() + "\n# build " + build_identity() + "\n");
        if (!recording) throw std::runtime_error(recording.error_text());
        telemetry.open(folder / "telemetry.csv", psx_playtest_diagnostic::TelemetryHeader, "");
        telemetry_failed = !telemetry;
        if (telemetry_failed) fprintf(stderr, "psxrecomp.diagnostics: telemetry unavailable: %s; input capture continues\n", telemetry.error_text());
        telemetry_start = telemetry_last = std::chrono::steady_clock::now();
        incident = 0; incident_held = false;
        std::ofstream metadata(folder / "session.json");
        metadata << session_json(false);
        if (!pending_baseline.empty()) {
            std::ofstream baseline(folder / "baseline.pst", std::ios::binary);
            baseline.write((const char*)pending_baseline.data(), (std::streamsize)pending_baseline.size());
            baseline.close();
            if (!baseline) throw std::runtime_error("cannot capture restored-state baseline");
            metadata << ",\n  \"baseline\": \"baseline.pst\"";
        } else if (pending_baseline_unavailable) {
            // A rewind/netplay restore is not a slot file this build can copy.
            metadata << ",\n  \"baseline_unavailable\": true";
        }
        metadata << "\n}\n";
        metadata.close();
        if (!metadata) throw std::runtime_error("cannot capture session metadata");
        const auto settings = PSXRecompV4::mod_runtime_root().parent_path() / "settings.toml";
        if (std::filesystem::exists(settings)) std::filesystem::copy_file(settings, folder / "settings.toml");
        const auto state = PSXRecompV4::mod_runtime_root() / "state.toml";
        if (std::filesystem::exists(state))
            std::filesystem::copy_file(state, folder / "mods-state.toml");
        else {
            // An untouched mod catalog uses defaults without a state file.
            // Capture that choice explicitly so fresh installs can replay.
            std::ofstream defaults(folder / "mods-state.toml");
            defaults << "format_version = 2\n";
            if (!defaults) throw std::runtime_error("cannot capture default mod choices");
        }
        /* Copy initial cards, including explicit absence, before the title
         * can change either. Replay launchers bind copies of these files. */
        for (unsigned slot = 0; slot < 2; ++slot) {
            std::array<uint8_t, MEMCARD_SIZE> card{};
            if (memcard_is_present((int)slot)) {
                if (memcard_export_raw((int)slot, card.data())) throw std::runtime_error("cannot capture initial card");
                std::ofstream out(folder / ("card" + std::to_string(slot + 1) + ".mcd"), std::ios::binary);
                out.write((const char*)card.data(), (std::streamsize)card.size());
                if (!out) throw std::runtime_error("cannot write initial card");
            } else {
                std::ofstream out(folder / ("card" + std::to_string(slot + 1) + ".absent"));
                if (!out) throw std::runtime_error("cannot mark absent card");
            }
        }
        recording_location("Recording: " + psx_playtest_diagnostic_utf8(std::filesystem::absolute(folder).make_preferred()) +
            "\nAfter reproducing the bug, quit normally and zip this entire session folder."
            "\nPress both stick buttons together to mark an incident."
            "\nIt includes starting game saves; do not include the game disc or BIOS.");
        fprintf(stderr, "psxrecomp.diagnostics: recording %s\n", psx_playtest_diagnostic_utf8(folder).c_str());
        fflush(stderr);
        pending_baseline.clear(); pending_baseline_unavailable = false;
    } catch (const std::exception& error) {
        recording.close();
        telemetry.close();
        recording_failed = true;
        recording_location(std::string("Recording FAILED: ") + error.what());
        fprintf(stderr, "psxrecomp.diagnostics: recording unavailable: %s\n", error.what());
        psx_mod_counter_add("psxrecomp.diagnostics.recording-failed", 1);
    }
}

void capture_telemetry(const Row& input) {
    if (telemetry_failed || !telemetry.is_open()) return;
    const auto now = std::chrono::steady_clock::now();
    psx_playtest_diagnostic::TelemetryRow row{};
    row[0] = input[0];
    row[1] = std::chrono::duration_cast<std::chrono::microseconds>(now - telemetry_start).count();
    row[2] = sequence ? std::chrono::duration_cast<std::chrono::microseconds>(now - telemetry_last).count() : 0;
    telemetry_last = now;
    const bool held = (input[ColButtons] & 6) == 0;
    row[34] = -1; // No capture requested on this sample.
    if (held && !incident_held) {
        ++incident;
        const auto image = recording_folder / ("incident-" + std::to_string(incident) + ".png");
        row[34] = psx_video_telemetry_capture(psx_playtest_diagnostic_utf8(image).c_str()) ? 1 : 0;
        host_osd_push(("Playtest incident " + std::to_string(incident) + " marked").c_str(), 2000);
    }
    incident_held = held; row[3] = incident;
    CDROMDebugState cd{}; cdrom_debug_snapshot(&cd);
    row[4] = cd.reading; row[5] = cd.pending_pending;
    row[6] = (cd.read_min * 60 + cd.read_sec) * 75 + cd.read_sect - 150;
    row[7] = cdrom_data_read_active(); row[8] = cdrom_xa_stream_active();
    RenderPassStats render{}; render_pass_get_stats(&render);
    row[9] = render.passes; row[10] = render.aborted;
    row[11] = render.verify_checks; row[12] = render.verify_mismatch;
    uint64_t nop = 0, fill = 0, draw_commands = 0, environment = 0, copy = 0;
    gpu_get_gp0_stats(&nop, &fill, &draw_commands, &environment, &copy);
    PsxVideoTelemetry video{}; psx_video_telemetry_snapshot(&video);
    PsxVideoConfigTelemetry config{}; psx_video_config_telemetry(&config);
    row[13] = draw_commands; row[14] = video.presents; row[15] = video.present_errors;
    row[16] = video.valid; row[17] = config.renderer;
    row[18] = config.resolution_preset;
    row[19] = config.aspect_num; row[20] = config.aspect_den; row[21] = config.widescreen;
    row[22] = video.renderer; row[23] = video.internal_scale;
    row[24] = video.source_width; row[25] = video.source_height;
    row[26] = video.drawable_width; row[27] = video.drawable_height;
    row[28] = video.viewport_width; row[29] = video.viewport_height;
    row[30] = render_pass_verification_enabled(); row[31] = render.verify_checks != 0;
    row[32] = 0; row[33] = 0; // No independent visual oracle is wired here.
    row[35] = video.present_status_available;
    telemetry.native(row);
    if (!telemetry) {
        telemetry_failed = true;
        fprintf(stderr, "psxrecomp.diagnostics: telemetry stopped: %s; input capture continues\n", telemetry.error_text());
        psx_mod_counter_add("psxrecomp.diagnostics.telemetry-failed", 1);
    }
}

/* Parse savestate_status_json: did a slot-file load just complete? */
bool completed_slot_load(int& slot, unsigned& generation) {
    char json[256]{};
    savestate_status_json(json, sizeof json);
    unsigned gen = 0; int pending_flag = 0, ok = 0, last_slot = -1; char op[8]{};
    if (sscanf(json, "\"generation\":%u,\"pending\":%d,\"last_ok\":%d,\"last_op\":\"%7[a-z]\",\"last_slot\":%d",
               &gen, &pending_flag, &ok, op, &last_slot) != 5) return false;
    generation = gen; slot = last_slot;
    return !pending_flag && ok && !strcmp(op, "load") && last_slot >= 0 && gen != last_generation;
}

std::vector<uint8_t> read_slot(int slot) {
    uint8_t *data = nullptr; size_t size = 0;
    std::vector<uint8_t> image;
    if (savestate_read_slot(slot, &data, &size) && data) image.assign(data, data + size);
    free(data);
    return image;
}

/* A state restore starts a new baseline segment, as a manual jump cannot be
 * reconciled with earlier rows. Rewind restores land here too but have no slot
 * file; they are marked unavailable instead of copying a stale slot. */
void restored() {
    if (psx_mod_netplay_is_active()) return;
    int slot = -1; unsigned generation = 0;
    const bool from_slot = completed_slot_load(slot, generation);
    // Remember every status generation seen, so a later rewind never reuses a slot.
    last_generation = generation;
    std::vector<uint8_t> image = from_slot ? read_slot(slot) : std::vector<uint8_t>();
    if (!replay_path.empty()) {
        // PSX_LOAD_SLOT restores a segment's baseline before its first row.
        // A later manual jump cannot be reconciled with that segment's rows.
        if (started && !replay_failed && replaying) { fail("unexpected state load during diagnostic playback"); return; }
        if (baseline_path.empty() || expected_baseline.empty() || image != expected_baseline) {
            started = true; fail("restored image does not match required diagnostic baseline"); return;
        }
        baseline_loaded = true; started = true;
        fprintf(stderr, "psxrecomp.diagnostics: replay baseline restored\n");
        fflush(stderr);
        begin_replay();
        if (replaying) seed_first_row();
        return;
    }
    close_recording(); recording.clear();
    sequence = 0; started = false; recording_failed = false;
    pending_baseline = image;
    pending_baseline_unavailable = image.empty();
    psx_mod_counter_add("psxrecomp.diagnostics.state-segments", 1);
}

std::string input_line(unsigned seat, uint16_t buttons, uint32_t frame) {
    static const char *names[] = {"Se", "L3", "R3", "St", "U", "R", "D", "L", "L2", "R2", "L1", "R1", "T", "O", "X", "[]"};
    std::string text = "P" + std::to_string(seat + 1) + ":";
    bool held = false;
    for (unsigned bit = 0; bit < 16; ++bit) if (!(buttons & (1u << bit))) {
        text += ' '; text += names[bit]; held = true;
    }
    if (!held) text += " -";
    text += "  F" + std::to_string(frame);
    if (replay_failed || recording_failed) text += " ERROR";
    else if (replaying) text += input_only ? " ROUTE" : " PLAY";
    else if (recording.is_open()) text += " REC";
    return text;
}

void tick_osd() {
    if (!osd_enabled || psx_mod_netplay_is_active()) {
        host_osd_set_diagnostics(nullptr, nullptr);
        return;
    }
    // Input Display is independent of Recording. Only sample the accepted
    // button words; do not hash RAM, enqueue rows or create session files.
    auto first = input_line(0, sio_get_pad_buttons_slot(0), osd_sequence);
    auto second = input_line(1, sio_get_pad_buttons_slot(1), osd_sequence);
    host_osd_set_diagnostics(first.c_str(), second.c_str());
    ++osd_sequence;
}

const char *column_name(unsigned column) {
    static const char *names[RowColumns] = {"sequence", "P1 buttons", "P2 buttons",
        "P1 analog", "P1 lx", "P1 ly", "P1 rx", "P1 ry", "P2 analog", "P2 lx", "P2 ly", "P2 rx", "P2 ry", "RAM digest",
        "P1 connected", "P2 connected"};
    return names[column];
}

void verify(const Row& live) {
    if (!pending_valid) return;
    for (unsigned column = 0; column < RowColumns; ++column) {
        if (input_only && column == ColDigest) continue;
        if (pending[column] == live[column]) continue;
        fprintf(stderr, "psxrecomp.diagnostics: first divergence at sequence %u: %s expected %lld got %lld%s\n",
                sequence, column_name(column), (long long)pending[column], (long long)live[column],
                column == ColDigest ? " (guest state diverged; replay is not a successful reproduction)"
                                    : " (input boundary mismatch)");
        fail("first divergence");
        return;
    }
}

/* Runs at each guest VBlank, before the VBlank IRQ is delivered to the game. */
void tick() {
    if (psx_mod_netplay_is_active() || replay_failed) return;
    /* Never admit rows of a baseline segment from the normal boot while the
     * auto-load is still pending. */
    if (!replay_path.empty() && !baseline_path.empty() && !baseline_loaded) return;
    if (replay_path.empty() && recording_failed && !recording.is_open()) return;
    const auto t0 = std::chrono::steady_clock::now();
    const bool starting = !started;
    if (!started) {
        if (!replay_path.empty()) started = true;
        else begin_recording();
    }
    Row row = sample(sequence);
    if (replaying) {
        verify(row);
        if (replaying) {
            psx_mod_counter_add(input_only ? "psxrecomp.diagnostics.route-inputs" : "psxrecomp.diagnostics.replayed-inputs", 1);
            const uint32_t verified = sequence + 1;
            if (!next_row()) {
                replay.close(); replaying = false; pending_valid = false;
                psx_mod_counter_add(input_only ? "psxrecomp.diagnostics.route-complete" : "psxrecomp.diagnostics.replay-complete", 1);
                if (input_only) fprintf(stderr,"psxrecomp.diagnostics: input-only route complete (%u inputs applied; RAM was not verified)\n",verified);
                else fprintf(stderr, "psxrecomp.diagnostics: input replay complete (%u inputs verified)\n", verified);
                fflush(stderr);
                release_sources();
            }
        }
    } else if (recording.is_open() && !recording_failed) {
        recording.native(row);
        capture_telemetry(row);
        if (!recording) {
            psx_mod_counter_add("psxrecomp.diagnostics.recording-failed", 1);
            recording_failed = true;
            recording_location(std::string("Recording FAILED: ") + recording.error_text());
            fprintf(stderr, "psxrecomp.diagnostics: %s\n", recording.error_text());
        }
    }
    ++sequence;
    const uint64_t ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - t0).count();
    if (starting) { stats.start = ns; return; }
    stats.ns += ns; stats.max = std::max(stats.max, ns); ++stats.count;
    if (ns > 1000000) ++stats.slow;
}

void reset_session() {
    release_sources();
    close_recording(); replay.close(); recording.clear(); replay.clear();
    started = replaying = replay_failed = recording_failed = pending_valid = false;
    baseline_loaded = pending_baseline_unavailable = false;
    input_only = legacy_route = false;
    legacy_connected = 4;
    osd_enabled = false;
    sequence = osd_sequence = 0; last_generation = 0;
    pending_baseline.clear(); expected_baseline.clear();
    replay_path.clear(); baseline_path.clear();
    host_osd_set_diagnostics(nullptr, nullptr);
}

void activate() {
#ifndef PSX_NO_DEBUG_TOOLS
    const char* route = getenv("PSX_DIAGNOSTIC_INPUT_ONLY");
    input_only = route && !strcmp(route,"1");
    if (const char* presence = getenv("PSX_DIAGNOSTIC_LEGACY_CONNECTED")) {
        if (strlen(presence)==2 && (presence[0]=='0' || presence[0]=='1') &&
            (presence[1]=='0' || presence[1]=='1'))
            legacy_connected = (unsigned)(presence[0]-'0') | ((unsigned)(presence[1]-'0')<<1);
    }
#else
    const char* route = getenv("PSX_DIAGNOSTIC_INPUT_ONLY");
    if (route && !strcmp(route,"1")) {
        started = true; fail("input-only routes require a developer build"); return;
    }
#endif
    const char *path = getenv("PSX_DIAGNOSTIC_REPLAY");
    replay_path = path ? path : "";
    const char *baseline = getenv("PSX_DIAGNOSTIC_BASELINE");
    baseline_path = baseline ? baseline : "";
    try { (void)build_identity(); }
    catch(const std::exception& error) {
        started=true;
        if(!replay_path.empty())fail(error.what());
        else {
            recording_failed=true;
            recording_location(std::string("Recording FAILED: ")+error.what());
            fprintf(stderr,"psxrecomp.diagnostics: %s\n",error.what());
        }
        return;
    }
    recording_location(replay_path.empty() ? "Waiting for game frames. Playtest Diagnostics must be enabled."
        : "Offline diagnostic replay active; no new recording is created.");
    if (!replay_path.empty()) {
        if (psx_mod_netplay_is_active()) { started = true; fail("offline replay only"); return; }
        if (!baseline_path.empty()) {
            std::ifstream in(std::filesystem::u8path(baseline_path), std::ios::binary | std::ios::ate);
            auto size = in.tellg();
            if (!in || size <= 0 || size > 64 * 1024 * 1024) {
                started = true; fail("required diagnostic baseline is unavailable"); return;
            }
            expected_baseline.resize(static_cast<size_t>(size)); in.seekg(0);
            if (!in.read((char*)expected_baseline.data(), size)) {
                started = true; fail("cannot read required diagnostic baseline"); return;
            }
        }
        // Playback also works in a hidden run without physical controllers.
        psx_mod_set_controller_source(0, first_source);
        psx_mod_set_controller_source(1, second_source); owns_sources = true;
        if (baseline_path.empty()) begin_replay();
    }
    host_osd_set_diagnostics(nullptr, nullptr);
}

void activate_osd() {
    osd_enabled = true;
    osd_sequence = 0;
    host_osd_set_diagnostics(nullptr, nullptr);
}
}

PSX_MOD_CONSTRUCTOR(psx_register_playtest_diagnostics) {
    psx_mod_register_session_reset_plugin(PSX_DIAGNOSTIC_MOD_ID, reset_session);
    psx_mod_register_activation_plugin(PSX_DIAGNOSTIC_MOD_ID, activate);
    psx_mod_register_vblank_plugin(PSX_DIAGNOSTIC_MOD_ID, tick);
    psx_mod_register_savestate_plugin(PSX_DIAGNOSTIC_MOD_ID, restored);
    psx_mod_register_activation_plugin(PSX_DIAGNOSTIC_MOD_ID ".osd", activate_osd);
    psx_mod_register_vblank_plugin(PSX_DIAGNOSTIC_MOD_ID ".osd", tick_osd);
}
