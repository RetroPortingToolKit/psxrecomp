/* Render thread core (render_thread.c): ordering, payload integrity across
 * ring wraps, the frame bound (backpressure), stale-frame detection, the
 * sync point (drain + context hand-off) and shutdown. The "context" here is a
 * token that may be current on one thread at a time; ctx() checks that. */
#include "render_thread.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int checks, failures;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

/* ---- context token ------------------------------------------------------- */
static _Atomic int ctx_owner;          /* 0 none, 1 emu, 2 render */
static _Atomic int ctx_violations;
static __thread int t_is_render;       /* set by exec on first call */
static void fake_ctx(void *user, int current) {
    (void)user;
    int me = rt_on_render_thread() ? 2 : 1;
    if (current) {
        int expect = 0;
        if (!atomic_compare_exchange_strong(&ctx_owner, &expect, me))
            atomic_fetch_add(&ctx_violations, 1);
    } else {
        int expect = me;
        if (!atomic_compare_exchange_strong(&ctx_owner, &expect, 0))
            atomic_fetch_add(&ctx_violations, 1);
    }
}

/* ---- executor ------------------------------------------------------------ */
static uint64_t exec_next;             /* next expected sequence id */
static _Atomic uint64_t exec_count;
static _Atomic int exec_errors;
static _Atomic int exec_off_thread;
static _Atomic int exec_without_ctx;
static _Atomic int stale_seen;
static _Atomic int far_behind_seen;   /* two complete frames queued after this one */
static int exec_delay_us;
static int g_max_frames = 1;

static void sleep_us(int us) {
    struct timespec ts = { us / 1000000, (long)(us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

enum { OP_DATA = 1, OP_PRESENT = 2 };

static void fake_exec(void *user, const RtCmd *c, const void *payload) {
    (void)user;
    t_is_render = 1;
    if (!rt_on_render_thread()) atomic_fetch_add(&exec_off_thread, 1);
    if (atomic_load(&ctx_owner) != 2) atomic_fetch_add(&exec_without_ctx, 1);
    if (c->op == OP_PRESENT) {
        if (rt_frames_ahead() >= 1) atomic_fetch_add(&stale_seen, 1);
        if (rt_frames_ahead() >= 2) atomic_fetch_add(&far_behind_seen, 1);
        if (rt_frames_ahead() > g_max_frames) atomic_fetch_add(&exec_errors, 1);  /* the bound */
    } else if (c->op == OP_DATA) {
        const uint64_t *p = (const uint64_t *)payload;
        if (c->payload < 8 || p[0] != exec_next) atomic_fetch_add(&exec_errors, 1);
        const uint8_t *b = (const uint8_t *)payload;
        for (uint32_t i = 8; i < c->payload; i++)
            if (b[i] != (uint8_t)(p[0] * 31u + i)) { atomic_fetch_add(&exec_errors, 1); break; }
        exec_next++;
    }
    if (exec_delay_us) sleep_us(exec_delay_us);
    atomic_fetch_add(&exec_count, 1);
}

static uint64_t emit_seq;
static int emit(uint32_t bytes) {
    if (bytes < 8) bytes = 8;
    uint8_t *p = (uint8_t *)rt_cmd_begin(OP_DATA, 0, bytes);
    if (!p) return 0;
    if (((uintptr_t)p & (RT_ALIGN - 1)) != 0) { failures++; fprintf(stderr, "FAIL unaligned payload\n"); }
    memcpy(p, &emit_seq, 8);
    for (uint32_t i = 8; i < bytes; i++) p[i] = (uint8_t)(emit_seq * 31u + i);
    rt_cmd_commit();
    emit_seq++;
    return 1;
}

static void reset_state(void) {
    exec_next = 0; emit_seq = 0;
    atomic_store(&exec_count, 0);
    atomic_store(&exec_errors, 0);
    atomic_store(&exec_off_thread, 0);
    atomic_store(&exec_without_ctx, 0);
    atomic_store(&stale_seen, 0);
    atomic_store(&far_behind_seen, 0);
    atomic_store(&ctx_violations, 0);
    atomic_store(&ctx_owner, 1);      /* the "context" starts current here */
    exec_delay_us = 0;
}

static int start(size_t ring, int frames) {
    g_max_frames = frames;
    RtConfig cfg = { ring, frames, fake_exec, fake_ctx, NULL };
    return rt_start(&cfg);
}

static uint32_t rng = 12345u;
static uint32_t rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* 1. Ordering and payload integrity across many wraps of a small ring. */
static void test_order_and_wrap(void) {
    reset_state();
    CHECK(start(1u << 20, 3), "start");
    CHECK(atomic_load(&ctx_owner) != 1, "context left the starting thread");
    for (int f = 0; f < 400; f++) {
        int n = 1 + (int)(rnd() % 40);
        for (int i = 0; i < n; i++) CHECK(emit(8 + rnd() % 20000), "emit");
        rt_frame_end();
    }
    rt_drain();
    CHECK(atomic_load(&exec_count) == emit_seq, "all records executed (%llu/%llu)",
          (unsigned long long)atomic_load(&exec_count), (unsigned long long)emit_seq);
    CHECK(atomic_load(&exec_errors) == 0, "order/payload errors %d", atomic_load(&exec_errors));
    CHECK(atomic_load(&exec_off_thread) == 0, "exec on render thread only");
    CHECK(atomic_load(&exec_without_ctx) == 0, "exec with context current");
    RtStats st; rt_get_stats(&st);
    CHECK(st.frames_produced == 400 && st.frames_consumed == 400, "frame counters");
    CHECK(st.ring_high_water <= (1u << 20), "high water within ring");
    rt_stop();
    CHECK(atomic_load(&ctx_owner) == 1, "stop returns the context to the caller");
    CHECK(atomic_load(&ctx_violations) == 0, "context never current on two threads");
}

/* 2. Oversize records are refused (caller runs them directly). */
static void test_oversize(void) {
    reset_state();
    CHECK(start(1u << 20, 2), "start");
    CHECK(rt_cmd_begin(OP_DATA, 0, (1u << 19)) == NULL, "half-ring record refused");
    CHECK(emit((1u << 19) - 64), "just under half fits");
    rt_drain();
    RtStats st; rt_get_stats(&st);
    CHECK(st.oversize == 1, "oversize counted");
    rt_stop();
}

/* 3. Backpressure: with a slow render thread the producer never runs more
 * than max_frames closed frames ahead, and stale frames are flagged. */
static void test_backpressure(void) {
    reset_state();
    exec_delay_us = 2000;      /* 2 ms per record, 2 records per frame */
    CHECK(start(1u << 22, 2), "start");
    uint64_t worst = 0;
    for (int f = 0; f < 40; f++) {
        emit(64);
        uint8_t *p = (uint8_t *)rt_cmd_begin(OP_PRESENT, 0, 0); (void)p; rt_cmd_commit();
        rt_frame_end();
        RtStats st; rt_get_stats(&st);
        uint64_t d = st.frames_produced - st.frames_consumed;
        if (d > worst) worst = d;
    }
    rt_drain();
    RtStats st; rt_get_stats(&st);
    CHECK(worst <= 2, "in-flight bound held (worst %llu)", (unsigned long long)worst);
    CHECK(st.backpressure_waits > 0, "producer was held back");
    CHECK(atomic_load(&stale_seen) > 0, "presents one frame behind seen");
    CHECK(atomic_load(&far_behind_seen) > 0, "presents two frames behind seen (skip case)");
    CHECK(atomic_load(&exec_errors) == 0, "order kept under backpressure");
    rt_stop();

    /* A fast render thread never sees a stale frame for a 1-deep queue. */
    reset_state();
    CHECK(start(1u << 20, 1), "start");
    for (int f = 0; f < 50; f++) {
        emit(64);
        rt_cmd_begin(OP_PRESENT, 0, 0); rt_cmd_commit();
        rt_frame_end();
        rt_drain();
    }
    CHECK(atomic_load(&stale_seen) == 0, "no stale frames when drained each frame");
    rt_stop();
}

/* 4. Sync point: acquire drains everything first and moves the context;
 * release hands it back; interleaving with records and frames stays ordered
 * and never puts the context on two threads. */
static void test_acquire_release(void) {
    reset_state();
    exec_delay_us = 50;
    CHECK(start(1u << 20, 3), "start");
    uint64_t expected_acquires = 0;
    for (int round = 0; round < 300; round++) {
        if (rt_held()) {
            /* The backend never records while it holds the context. */
            if (rnd() % 2) rt_release(); else { rt_frame_end(); continue; }
        }
        int n = (int)(rnd() % 8);
        for (int i = 0; i < n; i++) emit(8 + rnd() % 4000);
        if (rnd() % 3 == 0) rt_frame_end();
        uint64_t before = emit_seq;
        rt_acquire("test");
        expected_acquires++;
        CHECK(atomic_load(&exec_count) >= before, "acquire drained");
        CHECK(atomic_load(&ctx_owner) == 1, "context current on the caller while held");
        CHECK(rt_held(), "held");
        rt_frame_end(); /* held: counted, not queued */
    }
    rt_release();
    rt_drain();
    RtStats st; rt_get_stats(&st);
    CHECK(st.acquires == expected_acquires, "acquire count %llu", (unsigned long long)st.acquires);
    CHECK(atomic_load(&ctx_violations) == 0, "context exclusive (%d)", atomic_load(&ctx_violations));
    CHECK(atomic_load(&exec_without_ctx) == 0, "exec always with context");
    RtAcquireEvent ev[4];
    CHECK(rt_acquire_events(ev, 4) == 4 && !strcmp(ev[0].reason, "test"), "acquire ring");
    rt_stop();
    CHECK(atomic_load(&ctx_owner) == 1, "context back on caller after stop");
}

/* 5. Stop while held: the caller keeps the context; nothing is lost. */
static void test_stop_while_held(void) {
    reset_state();
    CHECK(start(1u << 20, 3), "start");
    for (int i = 0; i < 100; i++) emit(100);
    rt_acquire("stop");
    rt_stop();
    CHECK(atomic_load(&exec_count) == 100, "drained before stop");
    CHECK(atomic_load(&ctx_owner) == 1, "caller owns the context");
    CHECK(atomic_load(&ctx_violations) == 0, "no violations");
    CHECK(!rt_running(), "stopped");
    /* Restart works. */
    CHECK(start(1u << 20, 3), "restart");
    emit(16);
    rt_stop();
}

/* 6. Timed work (frame generation's hook): an exec asks for a tick 5 ms out
 * while the ring then goes idle; the idle render thread wakes for it (not
 * only when records arrive), holds the context, and each tick can ask for
 * the next. Ticks never run before their deadline. */
static _Atomic int tick_count, tick_early, tick_no_ctx, tick_off_thread;
static uint64_t tick_due;
static uint64_t fake_tick(void *user, uint64_t now) {
    (void)user;
    if (now < tick_due) atomic_fetch_add(&tick_early, 1);
    if (atomic_load(&ctx_owner) != 2) atomic_fetch_add(&tick_no_ctx, 1);
    if (!rt_on_render_thread()) atomic_fetch_add(&tick_off_thread, 1);
    int n = atomic_fetch_add(&tick_count, 1) + 1;
    if (n >= 4) return 0;
    tick_due = now + 2000000u;
    return tick_due;
}
static void tick_exec(void *user, const RtCmd *c, const void *payload) {
    fake_exec(user, c, payload);
    if (c->op == OP_PRESENT) {
        tick_due = rt_now_ns() + 5000000u;
        rt_tick_at(tick_due);
    }
}
static void test_tick(void) {
    reset_state();
    atomic_store(&tick_count, 0);
    g_max_frames = 2;
    RtConfig cfg = { 1u << 20, 2, tick_exec, fake_ctx, NULL, fake_tick };
    CHECK(rt_start(&cfg), "start with tick");
    void *p = rt_cmd_begin(OP_PRESENT, 0, 16);
    CHECK(p != NULL, "present record");
    rt_cmd_commit();
    rt_frame_end();
    const uint64_t t0 = rt_now_ns();
    while (atomic_load(&tick_count) < 4 && rt_now_ns() - t0 < 2000000000ull) sleep_us(500);
    const uint64_t el = rt_now_ns() - t0;
    CHECK(atomic_load(&tick_count) == 4, "four ticks while idle (%d)", atomic_load(&tick_count));
    CHECK(el >= 10000000u, "ticks waited for their deadlines (%.1f ms)", (double)el * 1e-6);
    CHECK(atomic_load(&tick_early) == 0, "no tick before its deadline");
    CHECK(atomic_load(&tick_no_ctx) == 0 && atomic_load(&tick_off_thread) == 0,
          "ticks on the render thread, holding the context");
    sleep_us(20000);
    CHECK(atomic_load(&tick_count) == 4, "no tick after 0 was returned");
    rt_stop();
}

int main(void) {
    test_order_and_wrap();
    test_oversize();
    test_backpressure();
    test_acquire_release();
    test_stop_while_held();
    test_tick();
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
