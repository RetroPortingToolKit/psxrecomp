/* When the freeze heartbeat writes a full dump.
 *
 * The unit under test is the heartbeat itself: this file includes
 * runtime/src/freeze_heartbeat.c and gives it one sample for each call that
 * its thread makes every 100 ms. No thread is started, so a scenario is exact.
 * The dump policy is the production runtime/src/freeze_dump_policy.c.
 *
 * Every check reads what a person would read after a run: the dump files in
 * the working directory (their count, "wedge_kind" and "frame_count") and the
 * fields of psx_freeze_heartbeat.json.
 *
 *   test_freeze_heartbeat_wedge <scenario>
 *
 * One process runs one scenario, in a working directory of its own.
 */
#include "../src/freeze_heartbeat.c"

#include <stdlib.h>

/* ---- the rest of the runtime, as far as the heartbeat reads it ---- */
uint64_t s_frame_count;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
uint32_t i_stat;
uint32_t i_mask;
uint64_t g_dirty_ram_blocks_run;
uint64_t g_dirty_ram_insns_run;
uint64_t g_dirty_pump_max_gap_insns;
uint64_t g_dirty_pump_count;
uint32_t g_bail_first_site_ra, g_bail_first_wild_pc, g_bail_first_frame;
uint32_t g_bail_first_in_exc;
uint64_t g_vblank_raise_count, g_vblank_deliver_count, g_irq_deliver_count;
uint64_t g_vblank_ack_count;
uint32_t g_ra_tw_latched, g_ra_tw_site, g_ra_tw_pc, g_ra_tw_prev_ra;
uint32_t g_ra_tw_frame, g_ra_tw_in_exc, g_ra_tw_sp;
uint32_t g_present_slow_count;
int      g_present_vsync_disabled;
uint8_t *g_psx_ram;
uint64_t g_psx_bail_first, g_psx_bail_resolved, g_psx_bail_flattened;
uint64_t g_psx_bail_anomaly;
const char *g_psx_fatal_reason;
CPUState *debug_cpu_ptr;

/* A video: while it plays, every sample has a colour decode in it. */
static int      t_video_playing = 0;
static int      t_video_seen = 0;
static uint64_t t_video_last_frame = 0;

uint64_t psx_get_cycle_count(void) { return s_frame_count * 564480ull; }

void psx_get_freeze_diag(uint64_t *total_checks, uint32_t *dispatch_count,
                         int *in_exception, int *post_exception_cooldown,
                         uint64_t *exception_entries,
                         uint64_t *exception_reentry_blocks) {
    *total_checks = 0;
    *dispatch_count = 0;
    *in_exception = 0;
    *post_exception_cooldown = 0;
    *exception_entries = 0;
    *exception_reentry_blocks = 0;
}

void sio_get_freeze_diag(int *irq_pending, int *irq_countdown,
                         uint16_t *sio_stat_out, uint16_t *sio_ctrl_out,
                         int *card_active) {
    *irq_pending = 0;
    *irq_countdown = 0;
    *sio_stat_out = 0;
    *sio_ctrl_out = 0;
    *card_active = 0;
}

int sio_get_mc_max_state(void) { return 0; }
int sio_get_tx_writes(void) { return 0; }

void timers_get_debug(int t, uint16_t *counter, uint16_t *target,
                      uint32_t *mode, uint64_t *irq_fired) {
    (void)t;
    if (counter) *counter = 0;
    if (target) *target = 0;
    if (mode) *mode = 0;
    if (irq_fired) *irq_fired = 0;
}

void psx_bail_ledger_top(uint32_t *site_ra, uint32_t *wild_pc,
                         uint32_t *site_sp, uint32_t *guest_sp,
                         uint64_t *count, uint32_t *unique) {
    *site_ra = 0;
    *wild_pc = 0;
    *site_sp = 0;
    *guest_sp = 0;
    *count = 0;
    *unique = 0;
}

uint8_t *memory_get_scratchpad_ptr(void) { return NULL; }
uint64_t debug_server_get_tcp_stall_ms(void) { return 0; }
uint32_t debug_server_get_tcp_drops(void) { return 0; }

#define EMPTY_RING(name)                                                      \
    void name(FILE *f, uint32_t max_count) { (void)max_count; fputs("[]", f); }
EMPTY_RING(debug_server_freeze_dump_wtrace_all_json)
EMPTY_RING(debug_server_freeze_dump_wtrace_json)
EMPTY_RING(debug_server_freeze_dump_frame_history_json)
EMPTY_RING(debug_server_freeze_dump_sio_pc_json)
EMPTY_RING(debug_server_freeze_dump_thread_trace_json)
EMPTY_RING(debug_server_freeze_dump_restore_trace_json)
EMPTY_RING(debug_server_freeze_dump_fn_entry_json)
EMPTY_RING(debug_server_freeze_dump_dirty_block_json)

/* As the runtime's own: was there a colour decode in the last N frames? */
int mdec_recently_active(uint32_t within_frames) {
    if (!t_video_seen) return 0;
    return s_frame_count - t_video_last_frame <= within_frames;
}

/* ---- the test's own bookkeeping ---- */
static int failures = 0;

#define CHECK(cond, ...) do {                                                 \
    if (!(cond)) {                                                            \
        failures++;                                                           \
        fprintf(stderr, "FAIL line %d: ", __LINE__);                          \
        fprintf(stderr, __VA_ARGS__);                                         \
        fputc('\n', stderr);                                                  \
    }                                                                         \
} while (0)

typedef struct {
    uint32_t sample;             /* the sample that wrote it, counted from 1 */
    long long kind;              /* "wedge_kind" of the file */
    long long frame;             /* "frame_count" of the file */
} DumpSeen;

static DumpSeen t_dumps[16];
static int      t_dump_count = 0;
static uint32_t t_samples = 0;

/* The integer after "name": in `text`, or -1. */
static long long json_integer(const char *text, const char *name) {
    char key[64];
    snprintf(key, sizeof(key), "\"%s\":", name);
    const char *at = strstr(text, key);
    if (!at) return -1;
    return strtoll(at + strlen(key), NULL, 10);
}

/* A dump the heartbeat counted must be a file. Read its head, then remove
 * it: the file is this test's own output. */
static void record_dump(uint32_t sequence, long long first_wall,
                        long long last_wall) {
    for (long long wall = first_wall; wall <= last_wall; wall++) {
        char path[128];
        if (!freeze_dump_format_path(path, sizeof(path), "psx-runtime",
                                     wall, sequence))
            continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        static char head[8192];
        size_t n = fread(head, 1, sizeof(head) - 1, f);
        head[n] = 0;
        fclose(f);
        remove(path);
        if (t_dump_count < (int)(sizeof(t_dumps) / sizeof(t_dumps[0]))) {
            t_dumps[t_dump_count].sample = t_samples;
            t_dumps[t_dump_count].kind = json_integer(head, "wedge_kind");
            t_dumps[t_dump_count].frame = json_integer(head, "frame_count");
        }
        t_dump_count++;
        return;
    }
    failures++;
    fprintf(stderr, "FAIL: dump %u was counted but no file is on disk\n",
            sequence);
}

/* One heartbeat sample. The frame count moves by `frames`; the other three
 * values are the ones the spin class reads. */
static void sample(unsigned frames, uint32_t current_func, uint32_t store_pc,
                   uint64_t dirty_insns) {
    uint32_t before = s_dump_sequence;
    long long first_wall = (long long)time(NULL);
    s_frame_count += frames;
    if (t_video_playing) {
        t_video_seen = 1;
        t_video_last_frame = s_frame_count;
    }
    g_debug_current_func_addr = current_func;
    g_debug_last_store_pc = store_pc;
    g_dirty_ram_insns_run = dirty_insns;
    heartbeat_write();
    t_samples++;
    long long last_wall = (long long)time(NULL);
    for (uint32_t s = before; s != s_dump_sequence; s++)
        record_dump(s, first_wall, last_wall);
}

/* A game at work: the last store is at another address in every sample. */
static void moving(unsigned count, unsigned frames) {
    for (unsigned i = 0; i < count; i++)
        sample(frames, (t_samples & 1u) ? 0x00000F40u : 0x80012340u,
               0x80100000u + 4u * t_samples, 1000);
}

/* Frames arrive, but only one in four samples: 5 a window, the slow class. */
static void slow(unsigned count) {
    for (unsigned i = 0; i < count; i++)
        sample((t_samples % 4u) == 3u ? 1u : 0u, 0x80012340u,
               0x80100000u + 4u * t_samples, 1000);
}

/* The frame count stands still: the hard class. */
static void stopped(unsigned count) {
    for (unsigned i = 0; i < count; i++)
        sample(0, 0x80012340u, 0x80055AA0u, 1000);
}

/* Frames arrive and the three values stand still: a wait, or a spin. */
static void pinned(unsigned count, unsigned frames) {
    for (unsigned i = 0; i < count; i++)
        sample(frames, 0x00000F40u, 0x80065E94u, 1000);
}

/* A field of psx_freeze_heartbeat.json as the last sample wrote it, or -1. */
static long long heartbeat_field(const char *name) {
    static char text[64 * 1024];
    FILE *f = fopen(HB_FILE, "rb");
    if (!f) return -1;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    text[n] = 0;
    fclose(f);
    return json_integer(text, name);
}

static void set_dump_at_frame(const char *value) {
#ifdef _WIN32
    _putenv_s("PSX_DUMP_AT_FRAME", value ? value : "");
#else
    if (value)
        setenv("PSX_DUMP_AT_FRAME", value, 1);
    else
        unsetenv("PSX_DUMP_AT_FRAME");
#endif
}

/* Startup spends the ordinary slot on slow frames. After more than
 * 60 healthy seconds a second wedge must still get a dump; a third must not,
 * so that the output of one process stays bounded. */
static void budget_after_startup(void) {
    slow(40);
    CHECK(t_dump_count == 1 && t_dumps[0].kind == 3,
          "startup slow frames: %d dump(s), kind %lld; expected 1 of kind 3",
          t_dump_count, t_dumps[0].kind);

    moving(700, 6);
    CHECK(t_dump_count == 1, "70 healthy seconds wrote a dump");
    slow(40);
    CHECK(t_dump_count == 2,
          "a wedge after 70 healthy seconds: %d dump(s) in all; expected 2",
          t_dump_count);
    if (t_dump_count >= 2)
        CHECK(t_dumps[1].kind == 3, "the late dump is kind %lld; expected 3",
              t_dumps[1].kind);
    CHECK(heartbeat_field("automatic_freeze_dumps") == 2,
          "automatic_freeze_dumps is %lld; expected 2",
          heartbeat_field("automatic_freeze_dumps"));

    moving(700, 6);
    slow(40);
    CHECK(t_dump_count == 2,
          "a third wedge: %d dump(s) in all; the bound is 2 of this class",
          t_dump_count);
    CHECK(heartbeat_field("suppressed_freeze_events") >= 1,
          "suppressed_freeze_events is %lld; the third wedge must be counted",
          heartbeat_field("suppressed_freeze_events"));
}

/* Both automatic slots are spent at startup, as in a long run that wedged
 * later. PSX_DUMP_AT_FRAME=5000 must still write one dump, at the first
 * sample whose frame count is 5000 or more, and must not count as automatic. */
static void dump_at_frame(void) {
    set_dump_at_frame("5000");
    slow(40);
    stopped(25);
    CHECK(t_dump_count == 2 && t_dumps[0].kind == 3 && t_dumps[1].kind == 1,
          "startup: %d dump(s), kinds %lld and %lld; expected kinds 3 and 1",
          t_dump_count, t_dumps[0].kind, t_dumps[1].kind);

    while (s_frame_count + 50u < 5000u) moving(1, 50);
    CHECK(t_dump_count == 2, "a dump was written before frame 5000");
    moving(1, 50);
    CHECK(t_dump_count == 3,
          "frame %llu reached: %d dump(s) in all; expected 3",
          (unsigned long long)s_frame_count, t_dump_count);
    if (t_dump_count >= 3) {
        CHECK(t_dumps[2].kind == 6, "the requested dump is kind %lld; expected 6",
              t_dumps[2].kind);
        CHECK(t_dumps[2].frame >= 5000 && t_dumps[2].frame < 5050,
              "the requested dump is at frame %lld; expected 5000 to 5049",
              t_dumps[2].frame);
    }

    moving(100, 50);
    CHECK(t_dump_count == 3, "the requested dump was written %d times",
          t_dump_count - 2);
    CHECK(heartbeat_field("automatic_freeze_dumps") == 2,
          "automatic_freeze_dumps is %lld; the requested dump must not count",
          heartbeat_field("automatic_freeze_dumps"));
    CHECK(heartbeat_field("requested_dumps") == 1,
          "requested_dumps is %lld; expected 1",
          heartbeat_field("requested_dumps"));
}

/* A value that is not a whole frame count asks for nothing. */
static void dump_at_frame_bad_value(void) {
    set_dump_at_frame("5000x");
    moving(200, 50);
    CHECK(t_dump_count == 0, "PSX_DUMP_AT_FRAME=5000x wrote %d dump(s)",
          t_dump_count);
}

/* The shape that six healthy runs recorded: the last store is at
 * another address in every sample, and the sample 19 back happens to hold the
 * same values as the newest. Nothing stands still. Six addresses, 19 steps. */
static void spin_ends_only(void) {
#define PC(n) (0x80020000u + 0x104u * (n))
    static const uint32_t pc[19] = {
        PC(0), PC(1), PC(2), PC(0), PC(3), PC(1), PC(4), PC(0), PC(5), PC(2),
        PC(0), PC(1), PC(3), PC(0), PC(4), PC(5), PC(1), PC(0), PC(2) };
#undef PC
    for (unsigned i = 0; i < 300; i++)
        sample(6, 0x00000F40u, pc[i % 19u], 1000);
    CHECK(t_dump_count == 0,
          "30 s of a game at work wrote %d dump(s), the first of kind %lld "
          "at sample %u", t_dump_count, t_dumps[0].kind, t_dumps[0].sample);
    CHECK(heartbeat_field("automatic_freeze_dumps") == 0,
          "automatic_freeze_dumps is %lld; expected 0",
          heartbeat_field("automatic_freeze_dumps"));
}

/* A wait that ends is not a freeze: the three values stand still
 * for 3.0 s, and later for 3.9 s, and each time the game goes on. */
static void spin_wait_ends(void) {
    moving(25, 6);
    pinned(30, 6);
    moving(25, 6);
    pinned(39, 6);
    moving(25, 6);
    CHECK(t_dump_count == 0,
          "two waits that ended wrote %d dump(s), the first of kind %lld at "
          "sample %u", t_dump_count, t_dumps[0].kind, t_dumps[0].sample);
    CHECK(heartbeat_field("spin_waits_ended") == 2,
          "spin_waits_ended is %lld; expected 2",
          heartbeat_field("spin_waits_ended"));
}

/* The other side: a spin that does not end still gets its dump. The values
 * stand still for 4.0 s while about 50 frames a second arrive. */
static void spin_real(void) {
    moving(25, 5);
    pinned(40, 5);
    CHECK(t_dump_count == 1 && t_dumps[0].kind == 5,
          "4.0 s of a spin: %d dump(s), kind %lld; expected 1 of kind 5",
          t_dump_count, t_dumps[0].kind);
    pinned(200, 5);
    CHECK(t_dump_count == 1, "one spin wrote %d dumps", t_dump_count);
}

/* A video holds the same shape for as long as it plays, and stays excluded.
 * After it, a spin needs its whole time without a decode in it. */
static void spin_video(void) {
    moving(25, 6);
    t_video_playing = 1;
    pinned(100, 6);
    t_video_playing = 0;
    CHECK(t_dump_count == 0, "a video wrote %d dump(s)", t_dump_count);
    pinned(39, 6);
    CHECK(t_dump_count == 0,
          "3.9 s after a video: %d dump(s), the first at sample %u; the spin "
          "time still holds decodes", t_dump_count, t_dumps[0].sample);
    pinned(2, 6);
    CHECK(t_dump_count == 1 && t_dumps[0].kind == 5,
          "a spin that goes on after a video: %d dump(s), kind %lld; "
          "expected 1 of kind 5", t_dump_count, t_dumps[0].kind);
}

/* A host pause (a menu, a rewind) starts the count again: the samples before
 * it do not add to the samples after it. */
static void spin_pause_restarts(void) {
    moving(25, 6);
    pinned(30, 6);
    freeze_heartbeat_set_paused(1);
    pinned(10, 6);
    freeze_heartbeat_set_paused(0);
    pinned(39, 6);
    CHECK(t_dump_count == 0,
          "3.9 s after a pause: %d dump(s), the first of kind %lld at sample "
          "%u", t_dump_count, t_dumps[0].kind, t_dumps[0].sample);
    pinned(1, 6);
    CHECK(t_dump_count == 1 && t_dumps[0].kind == 5,
          "4.0 s of a spin after a pause: %d dump(s), kind %lld; expected 1 "
          "of kind 5", t_dump_count, t_dumps[0].kind);
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } scenarios[] = {
        {"budget-after-startup", budget_after_startup},
        {"dump-at-frame", dump_at_frame},
        {"dump-at-frame-bad-value", dump_at_frame_bad_value},
        {"spin-ends-only", spin_ends_only},
        {"spin-wait-ends", spin_wait_ends},
        {"spin-real", spin_real},
        {"spin-video", spin_video},
        {"spin-pause-restarts", spin_pause_restarts},
    };
    const char *wanted = argc > 1 ? argv[1] : "";
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        if (strcmp(scenarios[i].name, wanted) != 0) continue;
        set_dump_at_frame(NULL);
        scenarios[i].run();
        if (failures) {
            fprintf(stderr, "freeze heartbeat %s: %d failure(s)\n", wanted,
                    failures);
            return 1;
        }
        printf("freeze heartbeat %s: all checks passed\n", wanted);
        return 0;
    }
    fprintf(stderr, "unknown scenario '%s'\n", wanted);
    return 2;
}


/* The heartbeat's classifier runs against its authored samples, with no
 * independent profiler stream or watchdog thread in this unit fixture. */
uint64_t guest_cycle_profile_total(void) { return 0; }
uint64_t guest_cycle_profile_oldest(void) { return 0; }
int guest_cycle_profile_get(uint64_t seq, GcpSample *out) { (void)seq; (void)out; return 0; }
void starvation_watchdog_heartbeat(void) {}
