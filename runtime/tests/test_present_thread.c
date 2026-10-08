/* present_thread_test — the present thread core (present_thread.c) against a
 * fake presenter: submission order is presentation order, a slot is never
 * composed while queued or presenting, each done token reaches the producer
 * that next takes its slot, a slow display blocks the producer (and a fast one
 * still keeps every invariant), stop presents everything queued and disposes leftover tokens, a
 * failed context start reports failure. */
#include "present_thread.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int checks, failures;
static void check(int ok, const char *label) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", label); failures++; }
}
static void sleep_us(long us) {
    struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

#define N_FRAMES 600
static _Atomic int slot_busy[PT_MAX_SLOTS];   /* 1 while queued or presenting */
static uintptr_t presented[N_FRAMES + 8];
static _Atomic int n_presented;
static int ctx_ok = 1, ctx_on, ctx_off;
static long present_us = 0;
static _Atomic int disposed;
static uintptr_t last_done[PT_MAX_SLOTS];
static _Atomic int order_bad;

static int fake_ctx(void *u, int cur) { (void)u; if (cur) { ctx_on++; return ctx_ok; } ctx_off++; return 1; }
static void *fake_present(void *u, int slot, void *ready) {
    (void)u;
    if (!atomic_load(&slot_busy[slot])) atomic_store(&order_bad, 1);
    int i = atomic_fetch_add(&n_presented, 1);
    if (i < N_FRAMES + 8) presented[i] = (uintptr_t)ready;
    if (present_us) sleep_us(present_us + (rand() % 3) * present_us / 2);
    atomic_store(&slot_busy[slot], 0);
    /* done token: slot in the low byte, frame above */
    return (void *)(((uintptr_t)ready << 8) | (uintptr_t)slot | 0x80u);
}
static void fake_dispose(void *u, void *t) { (void)u; (void)t; atomic_fetch_add(&disposed, 1); }

static PtConfig cfg(int slots) {
    PtConfig c = { slots, fake_ctx, fake_present, fake_dispose, NULL };
    return c;
}
static void reset(void) {
    for (int i = 0; i < PT_MAX_SLOTS; i++) { atomic_store(&slot_busy[i], 0); last_done[i] = 0; }
    atomic_store(&n_presented, 0);
    atomic_store(&disposed, 0);
    atomic_store(&order_bad, 0);
    ctx_on = ctx_off = 0;
}

static void run(int slots, long us, int expect_waits) {
    reset();
    present_us = us;
    PtConfig c = cfg(slots);
    check(pt_start(&c), "start");
    check(pt_current() == 0, "first slot is 0");
    int bad_reuse = 0, bad_token = 0;
    for (int f = 1; f <= N_FRAMES; f++) {
        const int cur = pt_current();
        if (atomic_load(&slot_busy[cur])) bad_reuse++;
        /* producer "composes" for a bit */
        if (f % 7 == 0) sleep_us(200);
        atomic_store(&slot_busy[cur], 1);
        void *done = NULL;
        uint64_t w = 0;
        const int next = pt_submit((void *)(uintptr_t)f, &done, &w);
        if (next < 0 || next >= slots || next == cur || atomic_load(&slot_busy[next])) bad_reuse++;
        if ((uintptr_t)done != last_done[next]) bad_token++;
        /* this slot's next token is unknown until presented again */
        last_done[next] = 0;
        /* remember what present will hand back for cur */
        last_done[cur] = (((uintptr_t)f) << 8) | (uintptr_t)cur | 0x80u;
    }
    pt_drain();
    check(atomic_load(&n_presented) == N_FRAMES, "every submitted slot presented");
    int ordered = 1;
    for (int i = 0; i < N_FRAMES; i++) if (presented[i] != (uintptr_t)(i + 1)) ordered = 0;
    check(ordered, "presented in submission order");
    check(!bad_reuse, "a slot is never composed while queued or presenting");
    check(!bad_token, "done token reaches the producer taking the slot");
    check(!atomic_load(&order_bad), "present sees only queued slots");
    PtStats st;
    pt_get_stats(&st);
    check(st.submits == N_FRAMES && st.presents == N_FRAMES, "stats count");
    check(st.queued_high <= slots, "bounded queue");
    if (expect_waits > 0) check(st.waits > 0 && st.wait_ns > 0, "slow display blocks the producer");
    /* stop with frames queued: they are presented, leftover tokens disposed */
    present_us = 2000;
    for (int f = 0; f < slots - 1; f++) {
        void *d = NULL;
        atomic_store(&slot_busy[pt_current()], 1);
        (void)pt_submit((void *)(uintptr_t)(N_FRAMES + 1 + f), &d, NULL);
    }
    pt_stop();
    check(atomic_load(&n_presented) == N_FRAMES + slots - 1, "stop presents the queue");
    check(atomic_load(&disposed) > 0, "stop disposes uncollected tokens");
    check(ctx_on == 1 && ctx_off == 1, "context taken and released once");
    check(!pt_running() && pt_submit(NULL, NULL, NULL) == -1, "stopped");
}

int main(void) {
    srand(7);
    run(3, 0, 0);
    run(3, 1500, 1);
    run(2, 800, 1);
    run(4, 300, -1);   /* may or may not wait: invariants only */
    /* context failure */
    reset();
    ctx_ok = 0;
    PtConfig c = cfg(3);
    check(!pt_start(&c), "start fails when the context cannot be made current");
    check(!pt_running(), "not running after failed start");
    ctx_ok = 1;
    PtConfig bad = cfg(1);
    check(!pt_start(&bad), "one slot refused");
    printf("present_thread_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
