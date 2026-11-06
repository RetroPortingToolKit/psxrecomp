/* frame_trace.c — opt-in per-frame stereo/OpenXR timing ring. See frame_trace.h.
 *
 * Emulation/main thread only. The record path allocates nothing and writes no
 * files; export functions fill a caller buffer and are called outside the frame.
 */

#include "frame_trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Compile-time ring bound. The live capacity is a power of two <= this, chosen
 * by PSX_FRAME_TRACE_CAP (default 2048, ~34 s of 60 Hz or ~68 s of 30 Hz pairs).
 * Static, so enabling the trace never adds an allocation to the frame path. */
#define PSX_FRAME_TRACE_MAX 4096
#define PSX_FRAME_TRACE_DEFAULT 2048

static PSXFrameTraceRecord s_ring[PSX_FRAME_TRACE_MAX];
/* An in-progress frame never overwrites the retained committed window. */
static PSXFrameTraceRecord s_pending;

static int      s_configured;
static int      s_enabled;
static uint32_t s_cap = PSX_FRAME_TRACE_DEFAULT;

static uint64_t s_opened;      /* records opened since reset (== newest seq) */
static uint64_t s_committed;   /* records committed since reset */
static uint64_t s_missing;     /* commits that carried no stage at all */
static uint64_t s_mono_high;   /* last timestamp handed out (monotonic floor) */

static int      s_have_open;
static int      s_have_publish;
static uint64_t s_last_publish_ms;

/* Session metadata (see psx_frame_trace_session). */
static char     s_runtime[128];
static uint32_t s_target_period_us;

static PSXFrameTraceClock s_clock;

static const char *const s_stage_names[PSX_TRACE_STAGE_COUNT] = {
    "guest_sim", "pair_total",
    "eye_begin_l", "eye_guest_l", "eye_end_l", "eye_restore_l",
    "eye_begin_r", "eye_guest_r", "eye_end_r", "eye_restore_r",
    "xr_wait_frame", "xr_begin_frame", "xr_acquire", "xr_wait_image",
    "xr_copy", "xr_release", "xr_end_frame", "pose_age", "hud_capture",
    "hud_draw", "hud_readback",
    "game_env", "game_world", "game_text", "game_traverse", "game_icon",
    "game_submit", "game_laser", "game_select",
    "snapshot_extract", "snapshot_publish", "xr_locate", "host_prep"
};

static const struct { uint32_t bit; const char *name; } s_flag_names[] = {
    { PSX_FRAME_TRACE_STEREO_PUBLISHED, "published" },
    { PSX_FRAME_TRACE_STALE_PAIR,       "stale" },
    { PSX_FRAME_TRACE_SHED,             "shed" },
    { PSX_FRAME_TRACE_FAILED,           "failed" },
    { PSX_FRAME_TRACE_XR_EMPTY,         "xr_empty" },
    { PSX_FRAME_TRACE_SCENE_TRANSITION, "scene" },
    { PSX_FRAME_TRACE_IO_EVENT,         "io" },
    { PSX_FRAME_TRACE_NO_TELEMETRY,     "no_telemetry" },
    { PSX_FRAME_TRACE_BASELINE_REUSED,  "baseline_reused" },
    { PSX_FRAME_TRACE_XR_REPEAT,        "xr_repeat" }
};

const char *psx_frame_trace_stage_name(uint32_t stage) {
    return stage < PSX_TRACE_STAGE_COUNT ? s_stage_names[stage] : "?";
}

const char *psx_frame_trace_flag_names(uint32_t flags, char *buf, int cap) {
    int off = 0;
    if (!buf || cap <= 0) return "";
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof s_flag_names / sizeof s_flag_names[0]; ++i) {
        if (!(flags & s_flag_names[i].bit)) continue;
        off += snprintf(buf + off, (size_t)(cap - off), "%s%s",
                        off ? "|" : "", s_flag_names[i].name);
        if (off >= cap) break;
    }
    return buf;
}

static uint32_t next_pow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v && p < PSX_FRAME_TRACE_MAX) p <<= 1;
    return p;
}

static void configure_from_env(void) {
    const char *e = getenv("PSX_FRAME_TRACE");
    s_enabled = (e && e[0] && e[0] != '0') ? 1 : 0;
    e = getenv("PSX_FRAME_TRACE_CAP");
    if (e && e[0]) {
        long v = strtol(e, NULL, 0);
        if (v >= 64) s_cap = next_pow2((uint32_t)v);
    }
}

static void ensure_configured(void) {
    if (!s_configured) {
        s_configured = 1;
        configure_from_env();
    }
}

void psx_frame_trace_configure_from_env(void) {
    s_configured = 1;
    configure_from_env();
}

void psx_frame_trace_set_clock(PSXFrameTraceClock fn) { s_clock = fn; }

void psx_frame_trace_set_enabled(int on) {
    ensure_configured();
    s_enabled = on ? 1 : 0;
}

int psx_frame_trace_enabled(void) {
    ensure_configured();
    return s_enabled;
}

void psx_frame_trace_reset(void) {
    memset(s_ring, 0, sizeof s_ring);
    s_opened = s_committed = s_missing = s_mono_high = 0;
    s_have_open = s_have_publish = 0;
    s_last_publish_ms = 0;
    s_runtime[0] = '\0';
    s_target_period_us = 0;
}

static PSXFrameTraceRecord *open_record(void) {
    return &s_pending;
}

static uint64_t stamp_now(void) {
    uint64_t now = s_clock ? s_clock() : 0;
    /* Monotonic floor: a coarse or throttled clock must never make a later
     * record look older than the one before it. */
    if (now < s_mono_high) now = s_mono_high;
    s_mono_high = now;
    return now;
}

int psx_frame_trace_open(void) {
    ensure_configured();
    if (!s_enabled || s_have_open) return 0;
    uint64_t seq = s_opened + 1u;
    PSXFrameTraceRecord *r = &s_pending;
    memset(r, 0, sizeof *r);
    r->seq = seq;
    r->eye_failed = -1;
    r->open_ms = stamp_now();
    s_opened = seq;
    s_have_open = 1;
    return 1;
}

void psx_frame_trace_add(uint32_t stage, uint64_t us) {
    if (!s_have_open || stage >= PSX_TRACE_STAGE_COUNT) return;
    PSXFrameTraceRecord *r = open_record();
    uint64_t v = (uint64_t)r->stages_us[stage] + us;
    r->stages_us[stage] = v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}

void psx_frame_trace_ids(uint64_t guest_frame, uint64_t guest_cycle) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    r->guest_frame = guest_frame;
    r->guest_cycle = guest_cycle;
}

void psx_frame_trace_attempt(uint64_t attempt_id, uint32_t shed_skipped,
                             uint32_t shed_streak_ms) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    r->stereo_attempt = attempt_id;
    r->shed_skipped = shed_skipped;
    r->shed_streak_ms = shed_streak_ms;
    if (shed_skipped) r->flags |= PSX_FRAME_TRACE_SHED;
}

void psx_frame_trace_result(int published, uint64_t pair_id, int eye_failed,
                            const char *failure) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    r->pair_id = pair_id;
    r->eye_failed = eye_failed < 0 ? -1 : eye_failed;
    if (published) {
        r->flags |= PSX_FRAME_TRACE_STEREO_PUBLISHED;
        r->source = 1;
        return;
    }
    r->flags |= PSX_FRAME_TRACE_STALE_PAIR;
    if (failure && failure[0]) {
        r->flags |= PSX_FRAME_TRACE_FAILED;
        snprintf(r->failure, sizeof r->failure, "%s", failure);
    }
}

void psx_frame_trace_source(uint8_t source) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    /* First writer wins: a published stereo pair must not be relabelled
     * "native" by a later end-of-frame annotation. */
    if (!r->source) r->source = source;
}

void psx_frame_trace_draw(int eye, uint32_t prims, uint32_t textured) {
    if (!s_have_open || eye < 0 || eye > 1) return;
    PSXFrameTraceRecord *r = open_record();
    /* Per-eye fields first: `prims`/`textured_prims` are the legacy pair-level
     * view of the same counter and stay the last eye's values, as before. */
    if (eye == 0) { r->prims_l = prims; r->tex_prims_l = textured; }
    else { r->prims_r = prims; r->tex_prims_r = textured; }
    r->prims = prims;
    r->textured_prims = textured;
}

void psx_frame_trace_eye_cycles(int eye, uint32_t cycles) {
    if (!s_have_open || eye < 0 || eye > 1) return;
    PSXFrameTraceRecord *r = open_record();
    if (eye == 0) r->eye_cycles_l = cycles;
    else r->eye_cycles_r = cycles;
}

void psx_frame_trace_gpu(uint64_t tex_upload_bytes, uint32_t vram_leaks) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    r->tex_upload_bytes += tex_upload_bytes;
    uint64_t leaks = (uint64_t)r->vram_leaks + vram_leaks;
    r->vram_leaks = leaks > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)leaks;
}

void psx_frame_trace_gpu_sample(uint32_t scene_us, uint32_t present_us,
                                uint32_t age_frames) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    r->gpu_scene_us = scene_us;
    r->gpu_present_us = present_us;
    r->gpu_sample_age_frames = age_frames;
}

void psx_frame_trace_xr_cadence(uint32_t display_delta_us, int submit_fresh) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    r->xr_display_delta_us = display_delta_us;
    r->xr_submit_fresh = submit_fresh ? 1u : 0u;
    if (!submit_fresh) r->flags |= PSX_FRAME_TRACE_XR_REPEAT;
}

void psx_frame_trace_flag(uint32_t flag) {
    if (!s_have_open) return;
    open_record()->flags |= flag;
}

void psx_frame_trace_note(const char *tag) {
    if (!s_have_open || !tag) return;
    snprintf(open_record()->note, PSX_FRAME_TRACE_NOTE, "%s", tag);
}

void psx_frame_trace_io(const char *tag) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    r->flags |= PSX_FRAME_TRACE_IO_EVENT;
    if (tag) snprintf(r->note, PSX_FRAME_TRACE_NOTE, "%s", tag);
}

void psx_frame_trace_session(const char *runtime, uint32_t target_period_us) {
    if (!s_enabled) return;
    if (target_period_us != s_target_period_us) s_target_period_us = target_period_us;
    if (runtime && runtime != s_runtime && strcmp(runtime, s_runtime) != 0)
        snprintf(s_runtime, sizeof s_runtime, "%s", runtime);
}

void psx_frame_trace_commit(void) {
    if (!s_have_open) return;
    PSXFrameTraceRecord *r = open_record();
    int any = 0;
    for (uint32_t i = 0; i < PSX_TRACE_STAGE_COUNT; ++i)
        if (r->stages_us[i]) { any = 1; break; }
    /* A frame that only shed its pair genuinely has no stage: that is "stale",
     * not "no telemetry". Only an expected-to-be-instrumented frame that
     * recorded nothing at all is missing. */
    if (!any && !(r->flags & PSX_FRAME_TRACE_SHED)) {
        s_missing++;
        r->flags |= PSX_FRAME_TRACE_NO_TELEMETRY;
    }
    if (r->flags & PSX_FRAME_TRACE_STEREO_PUBLISHED) {
        s_have_publish = 1;
        s_last_publish_ms = r->open_ms;
        r->pair_age_ms = 0;
    } else if (s_have_publish) {
        r->pair_age_ms = r->open_ms - s_last_publish_ms;
    }
    s_ring[(r->seq - 1u) & (uint64_t)(s_cap - 1u)] = *r;
    s_committed++;
    s_have_open = 0;
}

uint32_t psx_frame_trace_capacity(void) { return s_cap; }
uint64_t psx_frame_trace_committed(void) { return s_committed; }
uint64_t psx_frame_trace_dropped(void) {
    return s_committed > s_cap ? s_committed - s_cap : 0;
}
uint64_t psx_frame_trace_missing(void) { return s_missing; }

uint32_t psx_frame_trace_read(PSXFrameTraceRecord *out, uint32_t capacity) {
    uint32_t n = 0;
    if (!out || !capacity || !s_committed) return 0;
    uint64_t retained = s_committed < s_cap ? s_committed : s_cap;
    uint64_t take = capacity < retained ? capacity : retained;
    uint64_t first = s_committed - take + 1u;   /* oldest of the newest `take` */
    for (uint64_t seq = first; seq <= s_committed; ++seq) {
        const PSXFrameTraceRecord *r =
            &s_ring[(seq - 1u) & (uint64_t)(s_cap - 1u)];
        if (r->seq != seq) continue;            /* overwritten / not ours */
        out[n++] = *r;
    }
    return n;
}

/* ---- export ------------------------------------------------------------- */

static int app(char *buf, int cap, int off, const char *fmt, ...) {
    va_list ap;
    if (off < 0) off = 0;
    if (buf && off < cap) {
        va_start(ap, fmt);
        int n = vsnprintf(buf + off, (size_t)(cap - off), fmt, ap);
        va_end(ap);
        return n < 0 ? off : off + n;
    }
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);        /* measure only */
    va_end(ap);
    return n < 0 ? off : off + n;
}

/* JSON string body with the few escapes a generated tool must not break on. */
static const char *jstr(const char *s, char *tmp, int cap) {
    int o = 0;
    if (cap <= 0) return "";
    tmp[0] = '\0';
    for (const unsigned char *p = (const unsigned char *)s; p && *p; ++p) {
        const unsigned char c = *p;
        const int need = c == '"' || c == '\\' || c == '\n' ? 2 : c < 0x20 ? 6 : 1;
        if (need > cap - 1 - o) break;
        if (c == '"' || c == '\\') { tmp[o++] = '\\'; tmp[o++] = (char)c; }
        else if (c == '\n') { tmp[o++] = '\\'; tmp[o++] = 'n'; }
        else if (c < 0x20) o += snprintf(tmp + o, (size_t)(cap - o), "\\u%04x", c);
        else tmp[o++] = (char)c;
    }
    tmp[o] = '\0';
    return tmp;
}

static int rec_json(char *buf, int cap, int off, const PSXFrameTraceRecord *r) {
    char t1[PSX_FRAME_TRACE_REASON * 2 + 8], t2[PSX_FRAME_TRACE_NOTE * 2 + 8];
    off = app(buf, cap, off,
              "{\"seq\":%llu,\"open_ms\":%llu,\"guest_frame\":%llu,"
              "\"guest_cycle\":%llu,\"attempt\":%llu,\"pair_id\":%llu,"
              "\"pair_age_ms\":%llu,\"source\":%u,\"eye_failed\":%d,"
              "\"shed\":%u,\"shed_ms\":%u,\"prims\":%u,\"tex_prims\":%u,"
              "\"prims_l\":%u,\"prims_r\":%u,\"tex_prims_l\":%u,"
              "\"tex_prims_r\":%u,\"cycles_l\":%u,\"cycles_r\":%u,"
              "\"vram_leaks\":%u,\"tex_bytes\":%llu,\"flags\":%u,"
              "\"gpu_scene_us\":%u,\"gpu_present_us\":%u,"
              "\"gpu_sample_age_frames\":%u,"
              "\"xr_display_delta_us\":%u,\"xr_submit_fresh\":%u,"
              "\"failure\":\"%s\",\"note\":\"%s\",\"stages_us\":{",
              (unsigned long long)r->seq, (unsigned long long)r->open_ms,
              (unsigned long long)r->guest_frame,
              (unsigned long long)r->guest_cycle,
              (unsigned long long)r->stereo_attempt,
              (unsigned long long)r->pair_id,
              (unsigned long long)r->pair_age_ms, r->source, r->eye_failed,
              r->shed_skipped, r->shed_streak_ms, r->prims, r->textured_prims,
              r->prims_l, r->prims_r, r->tex_prims_l, r->tex_prims_r,
              r->eye_cycles_l, r->eye_cycles_r,
              r->vram_leaks, (unsigned long long)r->tex_upload_bytes, r->flags,
              r->gpu_scene_us, r->gpu_present_us, r->gpu_sample_age_frames,
              r->xr_display_delta_us, (unsigned)r->xr_submit_fresh,
              jstr(r->failure, t1, sizeof t1), jstr(r->note, t2, sizeof t2));
    for (uint32_t s = 0; s < PSX_TRACE_STAGE_COUNT; ++s)
        off = app(buf, cap, off, "%s\"%s\":%u", s ? "," : "",
                  s_stage_names[s], r->stages_us[s]);
    return app(buf, cap, off, "}}");
}

/* Newest `max_records` (0 = all retained), oldest first. Returns the count and
 * the seq of the first record to read; iteration indexes the ring in place so
 * no export call touches the stack with a whole-ring copy. */
static uint32_t export_window(uint32_t max_records, uint64_t *first_seq) {
    uint64_t retained = s_committed < s_cap ? s_committed : s_cap;
    if (!retained) { *first_seq = 1; return 0; }
    uint32_t n = (uint32_t)retained;
    uint32_t take = (max_records && max_records < n) ? max_records : n;
    *first_seq = s_committed - take + 1u;
    return take;
}

static const PSXFrameTraceRecord *at_seq(uint64_t seq) {
    const PSXFrameTraceRecord *r = &s_ring[(seq - 1u) & (uint64_t)(s_cap - 1u)];
    return r->seq == seq ? r : NULL;
}

int psx_frame_trace_to_json(char *buf, int cap, uint32_t max_records) {
    char t1[256];
    uint64_t first;
    if (cap < 0 || (buf && cap == 0)) return 0;
    if (buf) buf[0] = '\0';
    uint32_t take = export_window(max_records, &first);
    int off = app(buf, cap, 0,
                  "{\"enabled\":%d,\"capacity\":%u,\"count\":%llu,"
                  "\"dropped\":%llu,\"missing\":%llu,"
                  "\"runtime\":\"%s\",\"target_period_us\":%u,\"records\":[",
                  s_enabled, s_cap, (unsigned long long)s_committed,
                  (unsigned long long)psx_frame_trace_dropped(),
                  (unsigned long long)s_missing,
                  jstr(s_runtime, t1, sizeof t1), s_target_period_us);
    int emitted = 0;
    for (uint64_t seq = first; emitted < (int)take; ++seq) {
        const PSXFrameTraceRecord *r = at_seq(seq);
        if (!r) continue;
        off = app(buf, cap, off, "%s", emitted ? "," : "");
        off = rec_json(buf, cap, off, r);
        emitted++;
    }
    return app(buf, cap, off, "]}");
}

int psx_frame_trace_to_csv(char *buf, int cap, uint32_t max_records) {
    char flags[96];
    uint64_t first;
    if (cap < 0 || (buf && cap == 0)) return 0;
    if (buf) buf[0] = '\0';
    uint32_t take = export_window(max_records, &first);
    int off = app(buf, cap, 0,
                  "seq,open_ms,guest_frame,guest_cycle,attempt,pair_id,"
                  "pair_age_ms,source,eye_failed,flags,shed,streak_ms,prims,"
                  "tex_prims,prims_l,prims_r,tex_prims_l,tex_prims_r,cycles_l,"
                  "cycles_r,vram_leaks,tex_bytes,xr_display_delta_us,"
                  "xr_submit_fresh,gpu_scene_us,gpu_present_us,"
                  "gpu_sample_age_frames");
    for (uint32_t s = 0; s < PSX_TRACE_STAGE_COUNT; ++s)
        off = app(buf, cap, off, ",%s", s_stage_names[s]);
    off = app(buf, cap, off, ",failure,note\n");
    int emitted = 0;
    for (uint64_t seq = first; emitted < (int)take; ++seq) {
        const PSXFrameTraceRecord *r = at_seq(seq);
        if (!r) continue;
        emitted++;
        off = app(buf, cap, off,
                  "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%d,\"%s\",%u,%u,%u,"
                  "%u,%u,%u,%u,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%u",
                  (unsigned long long)r->seq, (unsigned long long)r->open_ms,
                  (unsigned long long)r->guest_frame,
                  (unsigned long long)r->guest_cycle,
                  (unsigned long long)r->stereo_attempt,
                  (unsigned long long)r->pair_id,
                  (unsigned long long)r->pair_age_ms, r->source, r->eye_failed,
                  psx_frame_trace_flag_names(r->flags, flags, sizeof flags),
                  r->shed_skipped, r->shed_streak_ms, r->prims, r->textured_prims,
                  r->prims_l, r->prims_r, r->tex_prims_l, r->tex_prims_r,
                  r->eye_cycles_l, r->eye_cycles_r,
                  r->vram_leaks, (unsigned long long)r->tex_upload_bytes,
                  r->xr_display_delta_us, (unsigned)r->xr_submit_fresh,
                  r->gpu_scene_us, r->gpu_present_us,
                  r->gpu_sample_age_frames);
        for (uint32_t s = 0; s < PSX_TRACE_STAGE_COUNT; ++s)
            off = app(buf, cap, off, ",%u", r->stages_us[s]);
        off = app(buf, cap, off, ",\"%s\",\"%s\"\n", r->failure, r->note);
    }
    return off;
}

int psx_frame_trace_to_chrome_json(char *buf, int cap, uint32_t max_records) {
    char flags[96];
    uint64_t first;
    if (cap < 0 || (buf && cap == 0)) return 0;
    if (buf) buf[0] = '\0';
    uint32_t take = export_window(max_records, &first);
    int off = app(buf, cap, 0, "{\"traceEvents\":[");
    int first_ev = 1, emitted = 0;
    for (uint64_t seq = first; emitted < (int)take; ++seq) {
        const PSXFrameTraceRecord *r = at_seq(seq);
        if (!r) continue;
        emitted++;
        uint64_t base_us = r->open_ms * 1000u, at = 0;
        for (uint32_t s = 0; s < PSX_TRACE_STAGE_COUNT; ++s) {
            uint64_t dur = r->stages_us[s];
            if (dur) {
                off = app(buf, cap, off,
                          "%s{\"name\":\"%s\",\"ph\":\"X\",\"ts\":%llu,"
                          "\"dur\":%llu,\"pid\":1,\"tid\":1,\"args\":{"
                          "\"seq\":%llu,\"attempt\":%llu,\"pair_id\":%llu,"
                          "\"flags\":\"%s\"}}",
                          first_ev ? "" : ",", s_stage_names[s],
                          (unsigned long long)(base_us + at),
                          (unsigned long long)dur,
                          (unsigned long long)r->seq,
                          (unsigned long long)r->stereo_attempt,
                          (unsigned long long)r->pair_id,
                          psx_frame_trace_flag_names(r->flags, flags, sizeof flags));
                first_ev = 0;
            }
            at += dur;
        }
    }
    return app(buf, cap, off, "]}");
}
