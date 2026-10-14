#ifdef NDEBUG
#undef NDEBUG
#endif

/* Executable regression for the depth24 GP1(07h) present hold (#636).
 *
 * Drives the real gpu.c through a scripted depth24 FMV: 15-bit setup, a
 * switch to 24-bit, a full-width movie A, a GP1(07h) band retarget to a
 * narrower movie B (the hold), display-mode transitions and a second
 * retarget. Each frame is: guest GP0/GP1 writes, gpu_vblank_tick(), then
 * (per present policy) the frontend present, modelled exactly as
 * sdl_vblank_present_body does it: skip the Swap when
 * gpu_depth24_present_hold_tick() says so, else stage the frame through
 * gpu_depth24_stage_scanout() (the code every CPU depth24 present runs).
 *
 * Checks:
 *  - the hold count, its latched skips and the upload span are identical
 *    per guest tick whether presents run every VBlank (windowed), never
 *    (headless) or every other VBlank (deferred/skipped presents), and so
 *    are the GPU snapshot bytes;
 *  - the hold spans exactly three VBlanks (3 -> 2 -> 1 -> 0) and skips
 *    exactly those presents; uploads during it do not grow the span;
 *  - GP1(07h) in 15-bit mode, or an unchanged band, does not arm it;
 *  - rendering: no staged frame after a retarget shows a single pixel of
 *    the stale trailing movie-A columns, movie B is visible (not a black
 *    screen), and staging that ignored the hold would expose the stale
 *    columns (so the check has teeth);
 *  - rollback: loading the snapshot of any tick (hold and held-VBlank flag
 *    restored from the snap, whatever the pre-load flag was) and
 *    replaying reproduces the live run tick for tick, including the present
 *    of the restored tick and every staged frame;
 *  - disk load: the hold and skip clear, the span comes back from the
 *    snapshot and the restored frame presents at once.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gpu.c"

#include "gpu_exec_stubs.inc"

enum { FRAMES = 30, PRESENT_W = 320, MAX_ROWS = 600 };
enum { POLICY_EVERY, POLICY_NONE, POLICY_EVERY_OTHER, POLICY_COUNT };

#define MOVIE_A 0xAAu      /* stale full-width movie bytes */
#define MOVIE_B 0x33u      /* narrower movie after the retarget */
#define B_HALFWORDS 384u   /* 256 RGB pixels */
#define A_HALFWORDS 480u   /* 320 RGB pixels */
#define RETARGET_1 7
#define MODE_15_FRAME 16
#define MODE_24_FRAME 17
#define RETARGET_2 21

typedef struct {
    int hold;
    int skip_latch;
    uint32_t span;
    uint64_t snap_hash;
    int presented;      /* -1: no present ran, 0: Swap skipped, 1: staged */
    uint64_t frame_hash;
} Tick;

static uint32_t canvas[PRESENT_W * MAX_ROWS];

static uint64_t fnv(const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static uint64_t snap_hash(void) {
    uint32_t n = gpu_snapshot_bytes();
    uint8_t *buf = (uint8_t *)malloc(n);
    assert(buf);
    gpu_snapshot_write(buf);
    uint64_t h = fnv(buf, n);
    free(buf);
    return h;
}

static void gp1_vrange(uint32_t h) {
    gpu_write_gp1(0x07000000u | 0x10u | ((0x10u + h) << 10));
}

static void upload(uint8_t byte, uint32_t halfwords, uint32_t rows) {
    const uint32_t hw = (uint32_t)byte | ((uint32_t)byte << 8);
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0(0u);                              /* x = 0, y = 0 */
    gpu_write_gp0((rows << 16) | halfwords);
    for (uint32_t i = 0; i < halfwords * rows / 2u; i++)
        gpu_write_gp0(hw | (hw << 16));
    assert(gp0_state == GP0_IDLE);
}

/* Guest work for frame f (identical in every run and every replay). */
static void guest_frame(int f) {
    switch (f) {
    case 0:
        gpu_write_gp1(0x08000001u);                 /* 15-bit, 320 */
        gpu_write_gp1(0x05000000u);                 /* display area 0,0 */
        gpu_write_gp1(0x06000000u | 0x260u | (0xC60u << 12));
        gp1_vrange(64);
        break;
    case 1: gp1_vrange(48); break;                  /* 15-bit: no hold */
    case 2: gp1_vrange(64); break;
    case 3: gpu_write_gp1(0x08000011u); break;      /* -> 24-bit */
    case 4: gp1_vrange(64); break;                  /* same band: no hold */
    case RETARGET_1: gp1_vrange(48); break;         /* MotK-style retarget */
    case MODE_15_FRAME: gpu_write_gp1(0x08000001u); break;
    case MODE_24_FRAME: gpu_write_gp1(0x08000011u); break;
    case RETARGET_2: gp1_vrange(40); break;
    default: break;
    }
    if (f >= 3 && f < RETARGET_1)
        upload(MOVIE_A, A_HALFWORDS, 64);
    else if (f >= RETARGET_1)
        upload(MOVIE_B, B_HALFWORDS, 48);
}

static int present(uint64_t *frame_hash, int honor_hold) {
    int skip = gpu_depth24_present_hold_tick();
    if (skip && honor_hold)
        return 0;
    GpuDisplayInfo di;
    gpu_get_display_info(&di);
    if (!di.depth24) { *frame_hash = 0; return 1; }
    assert(di.screen_height <= MAX_ROWS);
    uint32_t rows = gpu_depth24_stage_scanout(&di, canvas, PRESENT_W, 8u);
    assert(rows > 0u);
    *frame_hash = fnv(canvas, sizeof(uint32_t) * PRESENT_W * di.screen_height);
    /* Rendering check on the staged frame. */
    const uint32_t stale = 0xFF000000u | (MOVIE_A << 16) | (MOVIE_A << 8) | MOVIE_A;
    const uint32_t fresh = 0xFF000000u | (MOVIE_B << 16) | (MOVIE_B << 8) | MOVIE_B;
    uint32_t n_stale = 0, n_fresh = 0;
    for (uint32_t i = 0; i < PRESENT_W * di.screen_height; i++) {
        n_stale += canvas[i] == stale;
        n_fresh += canvas[i] == fresh;
    }
    return 1 + (int)(n_stale > 0) * 2 + (int)(n_fresh > 0) * 4;
}

static void reset_gpu(void) {
    gpu_reset_state(1);
    memset(test_ram, 0, sizeof(test_ram));
}

static void record(Tick *t) {
    t->hold = s_d24_present_hold;
    t->skip_latch = s_d24_present_skip;
    t->span = s_d24_upload_x1;
    t->snap_hash = snap_hash();
}

/* Present step; returns -1/0/1 and checks the staged image. */
static int present_checked(int f, uint64_t *frame_hash) {
    int r = present(frame_hash, 1);
    if (r == 0) return 0;
    if (f >= RETARGET_1) {
        if (r & 2) {
            fprintf(stderr, "frame %d: stale movie-A pixels exposed\n", f);
            abort();
        }
        if (s_d24_upload_x1 != 0u && display_depth)
            assert(r & 4);                          /* movie B on screen */
    }
    return 1;
}

static void run(int policy, Tick out[FRAMES]) {
    reset_gpu();
    for (int f = 0; f < FRAMES; f++) {
        guest_frame(f);
        gpu_vblank_tick();
        record(&out[f]);
        out[f].presented = -1;
        out[f].frame_hash = 0;
        int do_present = policy == POLICY_EVERY ||
                         (policy == POLICY_EVERY_OTHER && (f & 1));
        if (do_present)
            out[f].presented = present_checked(f, &out[f].frame_hash);
    }
}

static void check_policies_agree(void) {
    static Tick t[POLICY_COUNT][FRAMES];
    for (int p = 0; p < POLICY_COUNT; p++)
        run(p, t[p]);
    for (int p = 1; p < POLICY_COUNT; p++)
        for (int f = 0; f < FRAMES; f++) {
            assert(t[p][f].hold == t[0][f].hold);
            assert(t[p][f].skip_latch == t[0][f].skip_latch);
            assert(t[p][f].span == t[0][f].span);
            assert(t[p][f].snap_hash == t[0][f].snap_hash);
        }

    const Tick *w = t[POLICY_EVERY];
    /* 15-bit retargets and an unchanged band never arm the hold. */
    for (int f = 0; f < RETARGET_1; f++) assert(w[f].hold == 0 && !w[f].skip_latch);
    /* All three VBlanks of each hold: 2, 1, 0 after the ticks, three skips. */
    const int starts[2] = { RETARGET_1, RETARGET_2 };
    for (int k = 0; k < 2; k++) {
        int s = starts[k];
        assert(w[s].hold == 2 && w[s + 1].hold == 1 && w[s + 2].hold == 0);
        assert(w[s + 3].hold == 0);
        for (int i = 0; i < 3; i++) {
            assert(w[s + i].presented == 0);       /* Swap skipped */
            assert(w[s + i].span == 0u);           /* held uploads ignored */
        }
        assert(w[s + 3].presented == 1);           /* first staged frame */
        assert(w[s + 3].span == B_HALFWORDS);
    }
    int skipped = 0;
    for (int f = 0; f < FRAMES; f++) skipped += w[f].presented == 0;
    assert(skipped == 6);
    /* Display-mode transitions: leaving 24-bit drops the span; coming back
     * gets it from that frame's upload. */
    assert(w[MODE_15_FRAME].span == 0u);
    assert(w[MODE_24_FRAME].span == B_HALFWORDS);
    /* Before the retarget movie A is legitimately full width. */
    assert(w[RETARGET_1 - 1].presented == 1);
    /* Deferred presents (odd ticks only): a held tick's present skips, the
     * first present after the hold stages a clean frame. */
    const Tick *d = t[POLICY_EVERY_OTHER];
    assert(d[RETARGET_1].presented == 0 && d[RETARGET_1 + 1].presented == -1);
    assert(d[RETARGET_1 + 2].presented == 0 && d[RETARGET_1 + 4].presented == 1);
}

/* Teeth: a present that ignored the hold right after the retarget would
 * show stale movie-A columns (the span was reset, lim == 0). */
static void check_hold_is_needed(void) {
    reset_gpu();
    for (int f = 0; f <= RETARGET_1; f++) { guest_frame(f); gpu_vblank_tick(); }
    uint64_t h;
    int r = present(&h, 0);
    assert(r & 2);
}

static void check_rollback_and_disk(void) {
    static Tick live[FRAMES];
    static uint8_t snaps[FRAMES][4096];
    static uint16_t vrams[FRAMES][1024 * 512];
    uint32_t n = gpu_snapshot_bytes();
    assert(n <= sizeof snaps[0]);

    reset_gpu();
    for (int f = 0; f < FRAMES; f++) {
        guest_frame(f);
        gpu_vblank_tick();
        record(&live[f]);
        gpu_snapshot_write(snaps[f]);
        memcpy(vrams[f], vram, sizeof vram);
        live[f].presented = present_checked(f, &live[f].frame_hash);
    }

    for (int k = RETARGET_1 - 2; k < FRAMES - 1; k++) {
        for (int latch = 0; latch < 2; latch++) {
            s_d24_present_skip = latch;            /* whatever the tip had */
            memcpy(vram, vrams[k], sizeof vram);
            assert(gpu_snapshot_read(snaps[k], n));
            /* psx_frontend_on_rb_snap_loaded keeps the hold. */
            assert(s_d24_present_hold == live[k].hold);
            assert(s_d24_upload_x1 == live[k].span);
            /* Restored output/skip: the present of the restored tick does
             * what the live present of that tick did. */
            uint64_t h = 0;
            int r = present_checked(k, &h);
            assert(r == live[k].presented);
            if (r == 1) assert(h == live[k].frame_hash);
            for (int f = k + 1; f < FRAMES; f++) {
                Tick t;
                guest_frame(f);
                gpu_vblank_tick();
                record(&t);
                assert(t.hold == live[f].hold);
                assert(t.skip_latch == live[f].skip_latch);
                assert(t.span == live[f].span);
                assert(t.snap_hash == live[f].snap_hash);
                t.frame_hash = 0;
                t.presented = present_checked(f, &t.frame_hash);
                assert(t.presented == live[f].presented);
                assert(t.frame_hash == live[f].frame_hash);
            }
        }
    }

    /* Disk savestate load mid-hold: hold and skip clear, span from the snap,
     * the restored frame presents at once. */
    for (int k = RETARGET_2; k < RETARGET_2 + 3; k++) {
        s_d24_present_skip = 1;
        memcpy(vram, vrams[k], sizeof vram);
        assert(gpu_snapshot_read(snaps[k], n));
        gpu_depth24_on_savestate_loaded();
        assert(s_d24_present_hold == 0);
        assert(s_d24_upload_x1 == live[k].span);
        uint64_t h;
        assert(present(&h, 1) != 0);
        /* The next VBlank latches no skip: the cleared hold stays cleared. */
        gpu_vblank_tick();
        assert(gpu_depth24_present_hold_tick() == 0);
    }
    /* A disk load after the hold keeps the restored span. */
    memcpy(vram, vrams[RETARGET_1 + 3], sizeof vram);
    assert(gpu_snapshot_read(snaps[RETARGET_1 + 3], n));
    gpu_depth24_on_savestate_loaded();
    assert(s_d24_upload_x1 == B_HALFWORDS);
    uint64_t h = 0;
    int r = present_checked(RETARGET_1 + 3, &h);
    assert(r == live[RETARGET_1 + 3].presented && h == live[RETARGET_1 + 3].frame_hash);
}

int main(void) {
    check_policies_agree();
    check_hold_is_needed();
    check_rollback_and_disk();
    puts("depth24_present_hold_exec_test: ok");
    return 0;
}
