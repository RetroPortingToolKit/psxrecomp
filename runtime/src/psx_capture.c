/*
 * psx_capture.c - unattended presented-frame / audio capture. See
 * psx_capture.h. No SDL dependency: the renderer hands over pixels, the audio
 * pump hands over PCM, the vblank handler drives the clock.
 */
#include "psx_capture.h"
#include "png_write.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define PSXCAP_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define PSXCAP_MKDIR(p) mkdir((p), 0777)
#endif

typedef struct { uint64_t frame; uint16_t word; } InputEv;

static char     s_out_dir[1024];
static char     s_wav_path[1024];
static char     s_script_path[1024];
static char     s_state_path[1024];
static int      s_req_w, s_req_h;
static uint64_t s_from = 0, s_to = UINT64_MAX, s_every = 1, s_exit_at = UINT64_MAX;
static int      s_ppm;
static int      s_requested, s_active, s_finished;

static uint8_t *s_rgb;           /* latest composed frame, top-down RGB */
static int      s_rgb_w, s_rgb_h;
static int      s_have_frame;
static uint64_t s_vb;            /* handler calls so far */
static uint64_t s_cur;           /* index of the frame the current handler presents */
static uint64_t s_frames_written;

static FILE    *s_wav;
static uint64_t s_wav_frames;
static int      s_wav_started, s_wav_stopped;

static InputEv *s_ev[2];
static size_t   s_ev_n[2], s_ev_cap[2], s_ev_pos[2];
static uint16_t s_hold[2] = { 0xFFFFu, 0xFFFFu };
static int      s_have_script;

/* ---- options -------------------------------------------------------- */

static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

int psx_capture_parse_arg(int argc, char **argv, int *i)
{
    const char *a = argv[*i];
    const char *v = (*i + 1 < argc) ? argv[*i + 1] : NULL;
    if (!starts_with(a, "--capture-") && strcmp(a, "--input-script") != 0 &&
        strcmp(a, "--load-state") != 0 && strcmp(a, "--exit-at-frame") != 0)
        return 0;
    if (!v) { fprintf(stderr, "psxrecomp capture: %s needs a value\n", a); return 0; }
    if (strcmp(a, "--capture-out") == 0) {
        snprintf(s_out_dir, sizeof s_out_dir, "%s", v);
    } else if (strcmp(a, "--capture-size") == 0) {
        int w = 0, h = 0;
        if (sscanf(v, "%dx%d", &w, &h) != 2 || w < 64 || h < 64 || w > 16384 || h > 16384) {
            fprintf(stderr, "psxrecomp capture: bad --capture-size '%s' (want WxH)\n", v);
            return 0;
        }
        s_req_w = w; s_req_h = h;
    } else if (strcmp(a, "--capture-from") == 0) {
        s_from = strtoull(v, NULL, 10);
    } else if (strcmp(a, "--capture-to") == 0) {
        s_to = strtoull(v, NULL, 10);
    } else if (strcmp(a, "--capture-every") == 0) {
        s_every = strtoull(v, NULL, 10);
        if (s_every < 1) s_every = 1;
    } else if (strcmp(a, "--capture-format") == 0) {
        s_ppm = (strcmp(v, "ppm") == 0);
    } else if (strcmp(a, "--capture-wav") == 0) {
        snprintf(s_wav_path, sizeof s_wav_path, "%s", v);
    } else if (strcmp(a, "--input-script") == 0) {
        snprintf(s_script_path, sizeof s_script_path, "%s", v);
    } else if (strcmp(a, "--load-state") == 0) {
        snprintf(s_state_path, sizeof s_state_path, "%s", v);
    } else if (strcmp(a, "--exit-at-frame") == 0) {
        s_exit_at = strtoull(v, NULL, 10);
    } else {
        fprintf(stderr, "psxrecomp capture: unknown option %s\n", a);
        return 0;
    }
    (*i)++;
    s_requested = 1;
    return 1;
}

int  psx_capture_requested(void) { return s_requested; }
int  psx_capture_active(void)    { return s_active; }
void psx_capture_size(int *w, int *h) { if (w) *w = s_req_w; if (h) *h = s_req_h; }
const char *psx_capture_load_state_path(void) { return s_state_path[0] ? s_state_path : NULL; }
uint64_t psx_capture_frame_index(void) { return s_cur; }

/* ---- input script --------------------------------------------------- */

static const struct { const char *name; uint16_t bit; } k_buttons[] = {
    { "SELECT", 1u << 0 }, { "L3", 1u << 1 }, { "R3", 1u << 2 }, { "START", 1u << 3 },
    { "UP", 1u << 4 }, { "RIGHT", 1u << 5 }, { "DOWN", 1u << 6 }, { "LEFT", 1u << 7 },
    { "L2", 1u << 8 }, { "R2", 1u << 9 }, { "L1", 1u << 10 }, { "R1", 1u << 11 },
    { "TRIANGLE", 1u << 12 }, { "TRI", 1u << 12 }, { "T", 1u << 12 },
    { "CIRCLE", 1u << 13 }, { "O", 1u << 13 },
    { "CROSS", 1u << 14 }, { "X", 1u << 14 },
    { "SQUARE", 1u << 15 }, { "SQ", 1u << 15 }, { "S", 1u << 15 },
};

static void ev_push(int p, uint64_t frame, uint16_t word)
{
    if (s_ev_n[p] == s_ev_cap[p]) {
        s_ev_cap[p] = s_ev_cap[p] ? s_ev_cap[p] * 2 : 256;
        s_ev[p] = (InputEv *)realloc(s_ev[p], s_ev_cap[p] * sizeof(InputEv));
        if (!s_ev[p]) { fprintf(stderr, "psxrecomp capture: out of memory\n"); exit(2); }
    }
    s_ev[p][s_ev_n[p]].frame = frame;
    s_ev[p][s_ev_n[p]].word = word;
    s_ev_n[p]++;
}

/* "<frame> [N:]BUTTONS" - BUTTONS joined by + , or space; '-' / NONE releases. */
static int load_script(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[512];
    int lineno = 0;
    if (!f) { fprintf(stderr, "psxrecomp capture: cannot open --input-script %s\n", path); return 0; }
    while (fgets(line, sizeof line, f)) {
        char *s = line, *h;
        uint64_t frame;
        int player = 0;
        uint16_t word = 0xFFFFu;
        lineno++;
        if ((h = strchr(s, '#')) != NULL) *h = 0;
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) continue;
        if (!isdigit((unsigned char)*s)) {
            fprintf(stderr, "psxrecomp capture: %s:%d: expected frame number\n", path, lineno);
            fclose(f); return 0;
        }
        frame = strtoull(s, &s, 10);
        while (*s && isspace((unsigned char)*s)) s++;
        if (isdigit((unsigned char)*s) && s[1] == ':') {
            player = *s - '1';
            if (player < 0 || player > 1) player = 0;
            s += 2;
        }
        while (*s) {
            char tok[16];
            size_t n = 0;
            while (*s && (isspace((unsigned char)*s) || *s == '+' || *s == ',')) s++;
            while (*s && !isspace((unsigned char)*s) && *s != '+' && *s != ',' && n < sizeof tok - 1)
                tok[n++] = (char)toupper((unsigned char)*s++);
            tok[n] = 0;
            if (!n) break;
            if (strcmp(tok, "-") == 0 || strcmp(tok, "NONE") == 0) continue;
            {
                size_t k; int ok = 0;
                for (k = 0; k < sizeof k_buttons / sizeof k_buttons[0]; k++)
                    if (strcmp(tok, k_buttons[k].name) == 0) { word &= (uint16_t)~k_buttons[k].bit; ok = 1; break; }
                if (!ok) {
                    fprintf(stderr, "psxrecomp capture: %s:%d: unknown button '%s'\n", path, lineno, tok);
                    fclose(f); return 0;
                }
            }
        }
        ev_push(player, frame, word);
    }
    fclose(f);
    s_have_script = 1;
    return 1;
}

int psx_capture_input(int *p1, int *p2)
{
    int p;
    if (!s_have_script) return 0;
    for (p = 0; p < 2; p++) {
        while (s_ev_pos[p] < s_ev_n[p] && s_ev[p][s_ev_pos[p]].frame <= s_cur) {
            s_hold[p] = s_ev[p][s_ev_pos[p]].word;
            s_ev_pos[p]++;
        }
    }
    if (p1) *p1 = s_hold[0];
    if (p2) *p2 = s_ev_n[1] ? (int)s_hold[1] : -1;
    return 1;
}

/* ---- files ---------------------------------------------------------- */

static void make_dirs(const char *path)
{
    char tmp[1024];
    size_t n = strlen(path), i;
    if (n >= sizeof tmp) return;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            if (!(i == 2 && tmp[1] == ':')) PSXCAP_MKDIR(tmp);
            tmp[i] = c;
        }
    }
    PSXCAP_MKDIR(tmp);
}

static void wav_header(FILE *f, uint32_t data_bytes)
{
    uint8_t h[44];
    uint32_t rate = 44100, byte_rate = 44100 * 4, riff = 36 + data_bytes;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVEfmt ", 8);
    { uint32_t v = 16; memcpy(h + 16, &v, 4); }
    { uint16_t v = 1; memcpy(h + 20, &v, 2); v = 2; memcpy(h + 22, &v, 2); }
    memcpy(h + 24, &rate, 4); memcpy(h + 28, &byte_rate, 4);
    { uint16_t v = 4; memcpy(h + 32, &v, 2); v = 16; memcpy(h + 34, &v, 2); }
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data_bytes, 4);
    fwrite(h, 1, 44, f);
}

void psx_capture_begin(void)
{
    if (!s_requested || s_active) return;
    if (s_out_dir[0]) make_dirs(s_out_dir);
    if (s_wav_path[0]) {
        s_wav = fopen(s_wav_path, "wb");
        if (!s_wav) fprintf(stderr, "psxrecomp capture: cannot open %s: %s\n", s_wav_path, strerror(errno));
        else wav_header(s_wav, 0);
    }
    if (s_script_path[0] && !load_script(s_script_path)) {
        fprintf(stderr, "psxrecomp capture: input script rejected; exiting\n");
        exit(2);
    }
    s_active = 1;
    fprintf(stdout, "psxrecomp capture: out=%s size=%dx%d from=%llu to=%llu every=%llu wav=%s script=%s\n",
            s_out_dir[0] ? s_out_dir : "(none)", s_req_w, s_req_h,
            (unsigned long long)s_from, s_to == UINT64_MAX ? 0ull : (unsigned long long)s_to,
            (unsigned long long)s_every, s_wav_path[0] ? s_wav_path : "(none)",
            s_script_path[0] ? s_script_path : "(none)");
}

/* ---- per-frame ------------------------------------------------------ */

int psx_capture_wants_readback(void)
{
    /* Only frames inside the range are ever written: skip the GPU readback elsewhere. */
    return s_active && s_out_dir[0] && s_vb + 1 >= s_from && (s_to == UINT64_MAX || s_vb <= s_to + 2);
}

void psx_capture_store_frame(const uint8_t *rgb, int w, int h)
{
    size_t need = (size_t)w * (size_t)h * 3u;
    if (!rgb || w <= 0 || h <= 0) return;
    if (w != s_rgb_w || h != s_rgb_h || !s_rgb) {
        free(s_rgb);
        s_rgb = (uint8_t *)malloc(need);
        if (!s_rgb) { s_rgb_w = s_rgb_h = 0; return; }
        s_rgb_w = w; s_rgb_h = h;
    }
    memcpy(s_rgb, rgb, need);
    s_have_frame = 1;
}

static void write_frame(uint64_t f)
{
    char path[1100];
    FILE *fp;
    int w = s_rgb_w, h = s_rgb_h;
    uint8_t *black = NULL;
    const uint8_t *px = s_rgb;
    if (!s_have_frame) {
        w = s_req_w ? s_req_w : 640; h = s_req_h ? s_req_h : 480;
        black = (uint8_t *)calloc((size_t)w * h, 3);
        px = black;
        if (!black) return;
    }
    snprintf(path, sizeof path, "%s/frame_%06llu.%s", s_out_dir, (unsigned long long)f, s_ppm ? "ppm" : "png");
    fp = fopen(path, "wb");
    if (fp) {
        if (s_ppm) {
            fprintf(fp, "P6\n%d %d\n255\n", w, h);
            fwrite(px, 1, (size_t)w * h * 3, fp);
        } else {
            png_write_rgb(fp, px, (uint32_t)w, (uint32_t)h);
        }
        fclose(fp);
        s_frames_written++;
    } else {
        fprintf(stderr, "psxrecomp capture: cannot write %s: %s\n", path, strerror(errno));
    }
    free(black);
}

int psx_capture_vblank(void)
{
    uint64_t end;
    if (!s_active) return 0;
    s_cur = s_vb++;
    if (s_cur > 0) {
        uint64_t f = s_cur - 1;
        if (s_out_dir[0] && f >= s_from && f <= s_to && ((f - s_from) % s_every) == 0)
            write_frame(f);
        if (s_wav && f >= s_to) s_wav_stopped = 1;
    }
    if (s_cur >= s_from) s_wav_started = 1;
    end = s_exit_at < s_to ? s_exit_at : s_to;
    if (end != UINT64_MAX && s_cur > end) return 1;
    return 0;
}

void psx_capture_audio(const int16_t *stereo, uint32_t frames)
{
    if (!s_active || !s_wav || !s_wav_started || s_wav_stopped || !frames) return;
    fwrite(stereo, 4, frames, s_wav);
    s_wav_frames += frames;
}

void psx_capture_finish(void)
{
    if (!s_active || s_finished) return;
    s_finished = 1;
    if (s_wav) {
        uint32_t bytes = (uint32_t)(s_wav_frames * 4u);
        fflush(s_wav);
        fseek(s_wav, 0, SEEK_SET);
        wav_header(s_wav, bytes);
        fclose(s_wav);
        s_wav = NULL;
    }
    fprintf(stdout, "psxrecomp capture: done frames_written=%llu audio_samples=%llu present_size=%dx%d "
            "handlers=%llu\n", (unsigned long long)s_frames_written, (unsigned long long)s_wav_frames,
            s_rgb_w, s_rgb_h, (unsigned long long)s_vb);
    fflush(stdout);
}
