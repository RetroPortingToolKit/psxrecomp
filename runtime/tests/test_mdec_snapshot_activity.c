#include "psx_thread_local.h"
#undef NDEBUG /* the Release test target must execute its assertions */
/* The MDEC FMV-activity stamp survives a snapshot round trip as itself:
 * "never decoded" stays never (issue #475: it used to load as "decoded 1000
 * cycles ago", a false FMV for ~8 frames after every early snapshot load),
 * a real recent decode stays recent, and v1 payloads still load. */
#include "../src/mdec.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

PSX_THREAD_LOCAL uint64_t psx_cycle_count;
uint64_t s_frame_count;
int debug_server_fmv_quiet(void) { return 1; }

#define FRAME 338688ull

static uint8_t *snap(uint32_t *len) {
    *len = mdec_snapshot_bytes();
    uint8_t *p = (uint8_t *)malloc(*len);
    assert(p);
    mdec_snapshot_write(p);
    return p;
}

static void put_u32(uint8_t *p, uint32_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) p[off + i] = (uint8_t)(v >> (8 * i));
}
static void put_u64(uint8_t *p, uint32_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) p[off + i] = (uint8_t)(v >> (8 * i));
}

int main(void) {
    uint32_t len;
    uint8_t *p;
    const uint32_t age_off = MDEC_SNAP_OFF_COUNTS + 8u;

    /* 1. Nothing decoded: save, load later in the session -> still idle. */
    mdec_init();
    psx_cycle_count = 100ull * FRAME;
    assert(!mdec_recently_active(8));
    p = snap(&len);
    psx_cycle_count = 200ull * FRAME;
    assert(mdec_snapshot_read(p, len));
    assert(!mdec_recently_active(8));
    assert(mdec_color_age_cycles() == (uint64_t)0 - 1u);
    free(p);

    /* 2. A real decode 3 frames before the save stays 3 frames old. */
    mdec_init();
    psx_cycle_count = 500ull * FRAME;
    mdec_last_color_decode_cycle = psx_cycle_count - 3ull * FRAME;
    p = snap(&len);
    mdec_init();
    assert(!mdec_recently_active(8));
    psx_cycle_count = 500ull * FRAME;
    assert(mdec_snapshot_read(p, len));
    assert(mdec_recently_active(8));
    assert(mdec_color_age_cycles() == 3ull * FRAME);
    free(p);

    /* 3. A v1 payload's "never" (age 1000) loads as never... */
    mdec_init();
    psx_cycle_count = 50ull * FRAME;
    p = snap(&len);
    put_u32(p, 0u, MDEC_SNAP_VER_V1);
    put_u64(p, age_off, MDEC_SNAP_V1_AGE_NEVER);
    assert(mdec_snapshot_validate(p, len));
    assert(mdec_snapshot_read(p, len));
    assert(!mdec_recently_active(8));
    /* ...and any other v1 age is a real decode. */
    put_u64(p, age_off, 2ull * FRAME);
    assert(mdec_snapshot_read(p, len));
    assert(mdec_recently_active(8));
    assert(mdec_color_age_cycles() == 2ull * FRAME);
    free(p);

    /* 4. A decode older than the restored clock's origin is not recent. */
    mdec_init();
    psx_cycle_count = 900ull * FRAME;
    mdec_last_color_decode_cycle = 100ull * FRAME;
    p = snap(&len);
    psx_cycle_count = 10ull * FRAME;
    assert(mdec_snapshot_read(p, len));
    assert(!mdec_recently_active(8));
    free(p);

    /* 5. Unknown versions are still refused. */
    mdec_init();
    p = snap(&len);
    put_u32(p, 0u, 3u);
    assert(!mdec_snapshot_validate(p, len));
    assert(!mdec_snapshot_read(p, len));
    free(p);

    puts("mdec_snapshot_activity_test: PASS");
    return 0;
}
