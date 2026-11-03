#!/usr/bin/env python3
"""Multi-seat Verify protocol regression for a lost POST (#636, PR #662).

Builds every seat from the production Verify code in psx_netplay_rb.c: the
POST receive loop, the Verify retransmit/wait/timeout block, the quorum
publish and seat-progress helpers, clear_post_handshake and
RB_VERIFY_TIMEOUT_MS are cut from the source verbatim, and the quorum itself
is recomp-net's rnet_rb_quorum.c. Each seat is its own translation unit (its
own copy of the file-static state), and a C driver supplies a controlled clock
and a lossy/delaying POST transport between them.

What is simulated, not production code: the input tips each seat reports
(rnet_session_remote_tip), the replay/Live schedule, and the realignment
itself. abort_episode_realign is a recording stub; the realignment that
follows it is driven as a new episode on all seats through the same
production Verify path, and the driver checks that every seat agrees on the
POST digest and the confirmed frontier moves forward.

The same scenarios run against a pre-fix build (Verify re-armed by
rb_owing_seats_progressed) to show the lost-POST case hangs there.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from test_netplay_load_apply_flow import function_body

ROOT = Path(__file__).resolve().parents[2]


def block_after(source: str, marker: str) -> str:
    """Text from marker through the brace block that follows it."""
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start:pos + 1]
    raise AssertionError(f"unterminated block: {marker}")


SEAT_PRELUDE = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "recomp_net/rb_quorum.h"
#include "recomp_net/rb_post.h"
#include "rbv_harness.h"
#define netplay_rb_peer_post_tip_ok rnet_rb_peer_post_tip_ok
typedef uint32_t rnet_u32;
typedef uint8_t rnet_u8;
typedef struct RNetSession RNetSession;
typedef enum { nRNetRbPhaseReplay = 1, nRNetRbPhaseVerify = 2 } RNetRbPhase;
typedef struct { int active; RNetRbPhase phase; uint32_t epoch, target; } FakeRb;
static FakeRb g_rb_state;
static FakeRb *g_rb = &g_rb_state;
static int rnet_rb_is_active(const FakeRb *r) { return r->active; }
static RNetRbPhase rnet_rb_get_phase(const FakeRb *r) { return r->phase; }
static uint32_t rnet_rb_get_epoch_id(const FakeRb *r) { return r->epoch; }
static uint32_t rnet_rb_get_target_tick(const FakeRb *r) { return r->target; }
static void rnet_rb_on_post_diverge(FakeRb *r) { (void)r; }
static RNetSession *sess(void) { return (RNetSession *)&g_rb_state; }
static uint64_t rb_mono_ms(void) { return rbv_now_ms; }
static uint32_t psx_netplay_peer_seats(void) { return rbv_expect[SEAT_ID]; }
static int rnet_session_remote_tip(RNetSession *s, int seat, rnet_u32 *tip)
{ (void)s; return rbv_remote_tip(SEAT_ID, seat, tip); }
static int rnet_session_send_rb_post(RNetSession *s, rnet_u32 epoch, rnet_u32 target,
                                     rnet_u32 dig, rnet_u32 av, rnet_u8 match)
{ (void)s; return rbv_send_post(SEAT_ID, epoch, target, dig, av, match); }
static int g_last_from = -1;
static int rnet_session_take_rb_post(RNetSession *s, rnet_u32 *epoch, rnet_u32 *target,
                                     rnet_u32 *dig, rnet_u32 *av, rnet_u8 *match)
{ (void)s; return rbv_take_post(SEAT_ID, &g_last_from, epoch, target, dig, av, match); }
static int rnet_session_rb_last_take_from(RNetSession *s) { (void)s; return g_last_from; }

/* Episode state the cut code reads (same names/types as psx_netplay_rb.c). */
static int g_local_post_sent;
static int g_peer_post_ok;
static RNetRbQuorum g_post_q;
static uint32_t g_peer_progress_tip[RNET_RB_QUORUM_SEATS];
static uint8_t g_peer_post_match;
static uint32_t g_post_digest, g_post_av, g_post_target;
static uint32_t g_peer_post_digest, g_peer_post_av;
static int g_post_rexmit_logged;
static uint64_t g_verify_wait_ms;
static uint32_t g_rb_rtt_ema_ms;

static int maybe_abandon_tip_extend(void) { return 0; }
static void clear_post_handshake(void);
static void ownership_on_post_match(uint32_t tip)
{
    int i;
    rbv_outcome(SEAT_ID, RBV_MATCHED, tip, g_peer_post_digest);
    /* Leaving Verify through tip-hold bursts our POST for any seat still
     * waiting (ownership_exit_to_live's RB_POST_BURST loop). */
    for (i = 0; i < RB_POST_BURST; ++i)
        (void)rnet_session_send_rb_post(sess(), g_rb->epoch, g_post_target, g_post_digest,
                                        g_post_av, 1u);
    g_rb->active = 0; /* episode finished: Live */
}
static void abort_episode_realign(const char *why)
{
    rbv_outcome(SEAT_ID, strstr(why, "verify timeout") ? RBV_TIMEOUT : RBV_DIVERGED,
                g_rb->target, 0u);
    g_rb->active = 0; /* Live, waiting for the realignment episode */
    clear_post_handshake();
}
'''

SEAT_API = r'''
static void seat_begin(uint32_t epoch, uint32_t target)
{
    g_rb->active = 1;
    g_rb->epoch = epoch;
    g_rb->target = target;
    g_rb->phase = nRNetRbPhaseReplay;
    clear_post_handshake();
}
/* enter_verify_at_tip's wire steps. */
static void seat_enter_verify(uint32_t digest, uint32_t av)
{
    RNetSession *s = sess();
    g_post_digest = digest;
    g_post_av = av;
    g_post_target = g_rb->target;
    g_rb->phase = nRNetRbPhaseVerify;
    g_verify_wait_ms = rb_mono_ms();
    (void)rnet_session_send_rb_post(s, g_rb->epoch, g_rb->target, g_post_digest, g_post_av, 1u);
    g_local_post_sent = 1;
    g_post_rexmit_logged = 0;
    rb_post_publish();
}
static void seat_pump(void)
{
    RNetSession *s = sess();
    rnet_u32 epoch, target, dig_m, in_dig;
    rnet_u8 match;
    RX_LOOP
    VERIFY_BLOCK
}
static void seat_reset_session(void) { memset(g_peer_progress_tip, 0, sizeof(g_peer_progress_tip)); }
static int seat_phase(void) { return g_rb->active ? (int)g_rb->phase : 0; }
const RbvSeatOps SEAT_OPS = { seat_begin, seat_enter_verify, seat_pump, seat_reset_session, seat_phase };
'''

HEADER = r'''
#ifndef RBV_HARNESS_H
#define RBV_HARNESS_H
#include <stdint.h>
enum { RBV_PENDING = 0, RBV_MATCHED = 1, RBV_TIMEOUT = 2, RBV_DIVERGED = 3 };
extern uint64_t rbv_now_ms;
extern uint32_t rbv_expect[4];
int rbv_remote_tip(int self, int seat, uint32_t *tip);
int rbv_send_post(int from, uint32_t epoch, uint32_t target, uint32_t dig, uint32_t av, uint8_t m);
int rbv_take_post(int self, int *from, uint32_t *epoch, uint32_t *target, uint32_t *dig,
                  uint32_t *av, uint8_t *m);
void rbv_outcome(int seat, int kind, uint32_t target, uint32_t digest);
typedef struct {
    void (*begin)(uint32_t epoch, uint32_t target);
    void (*enter_verify)(uint32_t digest, uint32_t av);
    void (*pump)(void);
    void (*reset_session)(void);
    int (*phase)(void);
} RbvSeatOps;
#endif
'''

DRIVER = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rbv_harness.h"
extern const RbvSeatOps rbv_seat0, rbv_seat1, rbv_seat2, rbv_seat3;
static const RbvSeatOps *seat[4] = {&rbv_seat0, &rbv_seat1, &rbv_seat2, &rbv_seat3};

#define STEP_MS 16u
#define TIMEOUT_MS RB_VERIFY_TIMEOUT_MS
uint64_t rbv_now_ms;
uint32_t rbv_expect[4];
static int n_seats;
static uint32_t present;             /* seats still connected */
static uint32_t tip[4];              /* each seat's input tip as peers see it */
static uint32_t drop_from;           /* POSTs from these seats are lost */
static uint32_t delay_from;          /* ... or held until release_delayed() */
static int dup_posts;                /* deliver every POST twice */
static int outcome[4];
static uint64_t outcome_ms[4];
static uint32_t outcome_digest[4];
static uint32_t frontier;            /* highest target every seat confirmed */

typedef struct { int from, to; uint32_t epoch, target, dig, av; uint8_t m; } Post;
static Post q[8192], held[8192];
static int qn, heldn;

int rbv_remote_tip(int self, int s, uint32_t *t)
{
    if (s == self || s >= n_seats || !(present & (1u << s)))
        return 0; /* unoccupied/disconnected slot: rnet_session_remote_tip fails */
    *t = tip[s];
    return 1;
}
int rbv_send_post(int from, uint32_t epoch, uint32_t target, uint32_t dig, uint32_t av, uint8_t m)
{
    int to, k;
    if (!(present & (1u << from)) || (drop_from & (1u << from)))
        return 1;
    for (to = 0; to < n_seats; ++to) {
        if (to == from || !(present & (1u << to)))
            continue;
        for (k = 0; k < (dup_posts ? 2 : 1); ++k) {
            Post p = {from, to, epoch, target, dig, av, m};
            if (delay_from & (1u << from)) {
                if (heldn < 8192) held[heldn++] = p;
            } else if (qn < 8192) {
                q[qn++] = p;
            }
        }
    }
    return 1;
}
int rbv_take_post(int self, int *from, uint32_t *epoch, uint32_t *target, uint32_t *dig,
                  uint32_t *av, uint8_t *m)
{
    int i;
    for (i = 0; i < qn; ++i) {
        if (q[i].to != self)
            continue;
        *from = q[i].from; *epoch = q[i].epoch; *target = q[i].target;
        *dig = q[i].dig; *av = q[i].av; *m = q[i].m;
        memmove(&q[i], &q[i + 1], (size_t)(qn - i - 1) * sizeof(Post));
        --qn;
        return 1;
    }
    return 0;
}
static void release_delayed(void)
{
    int i;
    for (i = 0; i < heldn && qn < 8192; ++i)
        q[qn++] = held[i];
    heldn = 0;
}
void rbv_outcome(int s, int kind, uint32_t target, uint32_t digest)
{
    (void)target;
    if (outcome[s] == RBV_PENDING) {
        outcome[s] = kind;
        outcome_ms[s] = rbv_now_ms;
        outcome_digest[s] = digest;
    }
}

/* Deterministic emulation stand-in: every seat that replayed the same inputs
 * to `target` hashes the same state. */
static uint32_t state_digest(uint32_t target) { return 0x9E3779B9u * target ^ 0x5bd1e995u; }
static uint32_t input_digest(uint32_t target) { return 0x85ebca6bu * (target + 7u); }

static void setup(int n)
{
    int s;
    n_seats = n;
    present = (1u << n) - 1u;
    drop_from = delay_from = 0;
    dup_posts = 0;
    qn = heldn = 0;
    for (s = 0; s < 4; ++s) {
        rbv_expect[s] = present & ~(1u << s);
        tip[s] = 0;
        seat[s]->reset_session(); /* each scenario is a new session */
    }
}
static void begin_all(uint32_t epoch, uint32_t target)
{
    int s;
    for (s = 0; s < n_seats; ++s) {
        rbv_expect[s] = present & ~(1u << s);
        outcome[s] = RBV_PENDING;
        outcome_ms[s] = 0;
        seat[s]->begin(epoch, target);
    }
}
static void pump_all(void)
{
    int s;
    for (s = 0; s < n_seats; ++s)
        if (present & (1u << s))
            seat[s]->pump();
}
static void step(void) { pump_all(); rbv_now_ms += STEP_MS; }

/* Per-seat schedule for one episode. */
typedef struct {
    uint64_t verify_at;     /* ms at which the seat finishes replay and POSTs */
    uint32_t replay_from;   /* tip while replaying ramps from here to target */
    int live_runs_ahead;    /* after a match the seat runs Live (tip grows) */
} Plan;

/* Runs until every connected seat has an outcome or `limit_ms` passes.
 * Returns 1 when all finished. */
static int run_episode(uint32_t epoch, uint32_t target, const Plan *plan, uint64_t limit_ms)
{
    uint64_t t0 = rbv_now_ms;
    int s, done;
    begin_all(epoch, target);
    for (;;) {
        for (s = 0; s < n_seats; ++s) {
            uint64_t el = rbv_now_ms - t0;
            if (!(present & (1u << s)))
                continue;
            if (seat[s]->phase() == 1 && el >= plan[s].verify_at) {
                tip[s] = target > tip[s] ? target : tip[s];
                seat[s]->enter_verify(state_digest(target), input_digest(target));
            } else if (seat[s]->phase() == 1 && plan[s].verify_at > 0) {
                /* genuine slow replay: tip moves toward (never past) target */
                uint32_t span = target - plan[s].replay_from;
                uint32_t want = plan[s].replay_from +
                                (uint32_t)((uint64_t)span * el / plan[s].verify_at);
                if (want > tip[s]) tip[s] = want;
            } else if (outcome[s] != RBV_PENDING && plan[s].live_runs_ahead) {
                tip[s] += 1; /* Live: one input per frame, past the target */
            }
        }
        step();
        done = 1;
        for (s = 0; s < n_seats; ++s)
            if ((present & (1u << s)) && outcome[s] == RBV_PENDING)
                done = 0;
        if (done)
            return 1;
        if (rbv_now_ms - t0 > limit_ms)
            return 0;
    }
}

/* Realign: the next episode at a later agreed tip, every seat present,
 * POSTs delivered. All seats must confirm the same digest and the frontier
 * must advance. */
static void realign_and_converge(uint32_t epoch, uint32_t target)
{
    Plan p[4];
    uint32_t before = frontier;
    int s;
    drop_from = delay_from = 0;
    memset(p, 0, sizeof(p));
    for (s = 0; s < 4; ++s) p[s].verify_at = 32u * (uint64_t)(s + 1);
    assert(run_episode(epoch, target, p, 2u * TIMEOUT_MS));
    for (s = 0; s < n_seats; ++s) {
        if (!(present & (1u << s)))
            continue;
        if (outcome[s] != RBV_MATCHED)
            fprintf(stderr, "realign epoch=%u seat=%d outcome=%d at=%llu\n", (unsigned)epoch, s,
                    outcome[s], (unsigned long long)outcome_ms[s]);
        assert(outcome[s] == RBV_MATCHED);
        assert(outcome_digest[s] == state_digest(target));
    }
    frontier = target;
    assert(frontier > before);
}

#define expect_timeout_bound(s, r) expect_timeout_bound_at(s, r, __LINE__)
static void expect_timeout_bound_at(int s, uint64_t last_rearm_ms, int line)
{
    if (outcome[s] != RBV_TIMEOUT || outcome_ms[s] < last_rearm_ms + TIMEOUT_MS ||
        outcome_ms[s] > last_rearm_ms + TIMEOUT_MS + 2u * STEP_MS)
        fprintf(stderr, "line %d seat %d: outcome=%d at=%llu, want timeout at %llu+%u\n",
                line, s, outcome[s], (unsigned long long)outcome_ms[s],
                (unsigned long long)last_rearm_ms, (unsigned)TIMEOUT_MS);
    assert(outcome[s] == RBV_TIMEOUT);
    assert(outcome_ms[s] >= last_rearm_ms + TIMEOUT_MS);
    assert(outcome_ms[s] <= last_rearm_ms + TIMEOUT_MS + 2u * STEP_MS);
}

/* 1. Three seats; seat 2 matches, goes Live past the target, its POST lost. */
static int lost_post_live_seat(int prefix_build)
{
    Plan p[4] = {{16, 0, 0}, {16, 0, 0}, {16, 0, 1}};
    uint64_t t0;
    setup(3);
    tip[0] = tip[1] = tip[2] = 990;
    rbv_now_ms = 1000;
    frontier = 900;
    t0 = rbv_now_ms;
    drop_from = 1u << 2;
    if (!run_episode(10, 1000, p, 30000)) {
        assert(outcome[2] == RBV_MATCHED);
        assert(outcome[0] == RBV_PENDING && outcome[1] == RBV_PENDING);
        return 0; /* stuck in Verify: the #636 hang */
    }
    assert(!prefix_build);
    assert(outcome[2] == RBV_MATCHED);
    /* Seat 2 crossed the target on entering Verify; nothing re-arms after. */
    expect_timeout_bound(0, t0 + 16);
    expect_timeout_bound(1, t0 + 16);
    assert(tip[2] > 1000);
    realign_and_converge(11, tip[2] + 30);
    return 1;
}

/* 2. Genuine slow replay up to and at the target keeps re-arming. */
static void slow_replay_to_target(void)
{
    /* seat 2 needs 12 s, ramping 1000 ticks with no gap near the timeout */
    Plan p[4] = {{16, 0, 0}, {16, 0, 0}, {12000, 0, 0}};
    setup(3);
    tip[2] = 1;
    rbv_now_ms = 5000;
    frontier = 0;
    assert(run_episode(20, 1000, p, 30000));
    assert(outcome[0] == RBV_MATCHED && outcome[1] == RBV_MATCHED && outcome[2] == RBV_MATCHED);
    assert(outcome_digest[0] == outcome_digest[1] && outcome_digest[1] == outcome_digest[2]);
    frontier = 1000;
    /* At the target exactly: the tip reaching `cap` still counts as replay. */
    {
        uint64_t t0;
        setup(3);
        tip[2] = 1990;
        rbv_now_ms = 20000;
        t0 = rbv_now_ms;
        begin_all(21, 2000);
        seat[0]->enter_verify(state_digest(2000), input_digest(2000));
        seat[1]->enter_verify(state_digest(2000), input_digest(2000));
        while (rbv_now_ms - t0 < 3000) step();
        tip[2] = 2000;           /* reaches the target: re-arm */
        step();
        {
            uint64_t rearm = rbv_now_ms - STEP_MS;
            while (outcome[0] == RBV_PENDING && rbv_now_ms - t0 < 20000) {
                tip[2] += 1;     /* then runs Live past it, POST lost */
                step();
            }
            expect_timeout_bound(0, rearm);
            expect_timeout_bound(1, rearm);
        }
        realign_and_converge(22, tip[2] + 30);
    }
}

/* 3. First observation past the target (fresh progress table) never re-arms. */
static void first_observation(void)
{
    uint64_t t0;
    int s;
    setup(3);
    for (s = 0; s < 3; ++s) seat[s]->reset_session();
    tip[2] = 5000;
    rbv_now_ms = 40000;
    t0 = rbv_now_ms;
    drop_from = 1u << 2;
    begin_all(30, 3000);
    seat[0]->enter_verify(state_digest(3000), input_digest(3000));
    seat[1]->enter_verify(state_digest(3000), input_digest(3000));
    while (outcome[0] == RBV_PENDING && rbv_now_ms - t0 < 20000) { tip[2] += 1; step(); }
    expect_timeout_bound(0, t0);
    expect_timeout_bound(1, t0);
    /* First sight of a tip below the target only records it. */
    setup(3);
    for (s = 0; s < 3; ++s) seat[s]->reset_session();
    tip[2] = 3500;
    rbv_now_ms = 60000;
    t0 = rbv_now_ms;
    begin_all(31, 4000);
    seat[0]->enter_verify(state_digest(4000), input_digest(4000));
    seat[1]->enter_verify(state_digest(4000), input_digest(4000));
    while (outcome[0] == RBV_PENDING && rbv_now_ms - t0 < 20000) step();
    expect_timeout_bound(0, t0);
    realign_and_converge(32, 4100);
}

/* 4. Four seats, two owing: seat 3 replays slowly, seat 2 is Live (POST lost). */
static void multiple_owing_seats(void)
{
    uint64_t t0, last_slow;
    setup(4);
    tip[2] = 6000;
    tip[3] = 5800;
    rbv_now_ms = 80000;
    t0 = rbv_now_ms;
    drop_from = 1u << 2;
    begin_all(40, 6000);
    seat[0]->enter_verify(state_digest(6000), input_digest(6000));
    seat[1]->enter_verify(state_digest(6000), input_digest(6000));
    seat[2]->enter_verify(state_digest(6000), input_digest(6000));
    /* seat 3: 10 ticks every 512 ms for 9 s (well past one timeout) */
    while (rbv_now_ms - t0 < 9000) {
        if ((rbv_now_ms - t0) % 512u == 0u) tip[3] += 10;
        tip[2] += 1;
        step();
        assert(outcome[0] == RBV_PENDING && outcome[1] == RBV_PENDING);
    }
    tip[3] = 6000;
    step();                       /* at the target: last re-arm */
    last_slow = rbv_now_ms - STEP_MS;
    {
        uint64_t seat3_verify = rbv_now_ms;
        seat[3]->enter_verify(state_digest(6000), input_digest(6000));
        while ((outcome[0] == RBV_PENDING || outcome[3] == RBV_PENDING) &&
               rbv_now_ms - t0 < 30000) {
            tip[2] += 1;
            step();
        }
        expect_timeout_bound(0, last_slow);
        expect_timeout_bound(1, last_slow);
        expect_timeout_bound(3, seat3_verify);
    }
    realign_and_converge(41, tip[2] + 30);

    /* Both owing seats Live with lost POSTs: plain timeout. */
    setup(4);
    tip[2] = tip[3] = 7000;
    rbv_now_ms = 120000;
    t0 = rbv_now_ms;
    drop_from = (1u << 2) | (1u << 3);
    begin_all(42, 6900);
    seat[0]->enter_verify(state_digest(6900), input_digest(6900));
    seat[1]->enter_verify(state_digest(6900), input_digest(6900));
    while (outcome[0] == RBV_PENDING && rbv_now_ms - t0 < 20000) { tip[2]++; tip[3]++; step(); }
    expect_timeout_bound(0, t0);
    expect_timeout_bound(1, t0);
    realign_and_converge(43, tip[2] + 30);
}

/* 5. A seat that disconnects mid-Verify, and one that never joined. */
static void missing_seat(void)
{
    uint64_t t0;
    setup(3);
    tip[2] = 7990;
    rbv_now_ms = 150000;
    t0 = rbv_now_ms;
    begin_all(50, 8000);
    seat[0]->enter_verify(state_digest(8000), input_digest(8000));
    seat[1]->enter_verify(state_digest(8000), input_digest(8000));
    step(); step();
    present &= ~(1u << 2);        /* seat 2 drops: its remote tip is gone */
    while (outcome[0] == RBV_PENDING && rbv_now_ms - t0 < 20000) step();
    expect_timeout_bound(0, t0);
    expect_timeout_bound(1, t0);
    /* Remaining seats realign between themselves. */
    realign_and_converge(51, 8100);

    /* Expected seat 3 never present at all (4-seat quorum, 3 connected). */
    setup(4);
    present = 0x7u;
    rbv_now_ms = 170000;
    t0 = rbv_now_ms;
    begin_all(52, 8200);
    rbv_expect[0] = rbv_expect[1] = rbv_expect[2] = 0xFu;
    {
        int s;
        for (s = 0; s < 3; ++s) {
            rbv_expect[s] &= ~(1u << s);
            seat[s]->begin(52, 8200);
        }
        for (s = 0; s < 3; ++s)
            seat[s]->enter_verify(state_digest(8200), input_digest(8200));
    }
    while (outcome[0] == RBV_PENDING && rbv_now_ms - t0 < 20000) step();
    expect_timeout_bound(0, t0);
    expect_timeout_bound(1, t0);
    expect_timeout_bound(2, t0);
}

/* 6. Late and duplicate POSTs. */
static void late_and_duplicate_post(void)
{
    Plan p[4] = {{16, 0, 0}, {48, 0, 0}, {80, 0, 0}};
    uint64_t t0;
    /* Duplicates: everything delivered twice, one clean match per seat. */
    setup(3);
    dup_posts = 1;
    rbv_now_ms = 200000;
    assert(run_episode(60, 9000, p, 10000));
    assert(outcome[0] == RBV_MATCHED && outcome[1] == RBV_MATCHED && outcome[2] == RBV_MATCHED);
    dup_posts = 0;
    qn = 0;

    /* Late: seat 2's POST is held past the others' timeout, then arrives. */
    setup(3);
    tip[2] = 9100;
    rbv_now_ms = 210000;
    t0 = rbv_now_ms;
    delay_from = 1u << 2;
    begin_all(61, 9100);
    seat[0]->enter_verify(state_digest(9100), input_digest(9100));
    seat[1]->enter_verify(state_digest(9100), input_digest(9100));
    seat[2]->enter_verify(state_digest(9100), input_digest(9100));
    while (outcome[0] == RBV_PENDING && rbv_now_ms - t0 < 20000) { tip[2]++; step(); }
    expect_timeout_bound(0, t0);
    expect_timeout_bound(1, t0);
    delay_from = 0;
    release_delayed();            /* stale epoch-61 POSTs now in flight */
    step();                       /* ignored: no longer in that Verify */
    assert(outcome[0] == RBV_TIMEOUT && outcome[1] == RBV_TIMEOUT);
    /* A stale epoch-61 POST (wrong digest) still in flight when the
     * realignment episode opens must be dropped by the epoch filter. */
    seat[2]->begin(61, 9100);
    seat[2]->enter_verify(0xBADu, 0xBADu);
    realign_and_converge(62, tip[2] + 30);
}

/* 7. Rematch (session reset, ticks restart low) and a later epoch in one session. */
static void new_epoch_and_rematch(void)
{
    Plan slow[4] = {{16, 0, 0}, {16, 0, 0}, {10000, 0, 0}};
    int s;
    /* Later epoch, same session: tips keep rising, slow replay still re-arms. */
    setup(3);
    rbv_now_ms = 250000;
    {
        Plan quick[4] = {{16, 0, 0}, {32, 0, 0}, {48, 0, 0}};
        tip[0] = tip[1] = tip[2] = 9400;
        assert(run_episode(69, 9450, quick, 10000));
        assert(outcome[2] == RBV_MATCHED);
    }
    tip[2] = 9500;
    slow[2].replay_from = 9500;
    assert(run_episode(70, 10500, slow, 30000));
    assert(outcome[0] == RBV_MATCHED && outcome[1] == RBV_MATCHED && outcome[2] == RBV_MATCHED);
    /* Rematch: cold reset clears the progress table, the clock restarts. */
    setup(3);
    for (s = 0; s < 3; ++s) seat[s]->reset_session();
    tip[2] = 1;
    slow[2].replay_from = 1;
    rbv_now_ms = 300000;
    frontier = 0;
    assert(run_episode(1, 600, slow, 30000));
    assert(outcome[0] == RBV_MATCHED && outcome[1] == RBV_MATCHED && outcome[2] == RBV_MATCHED);
    frontier = 600;
    /* And the lost-POST case after the rematch still times out and heals. */
    {
        Plan p[4] = {{16, 0, 0}, {16, 0, 0}, {16, 0, 1}};
        uint64_t t0 = rbv_now_ms;
        drop_from = 1u << 2;
        assert(run_episode(2, 700, p, 30000));
        expect_timeout_bound(0, t0 + 16);
        realign_and_converge(3, tip[2] + 30);
    }
}

int main(int argc, char **argv)
{
    int prefix = argc > 1 && strcmp(argv[1], "--prefix") == 0;
    if (prefix) {
        if (lost_post_live_seat(1)) {
            fprintf(stderr, "pre-fix build did not hang\n");
            return 1;
        }
        printf("pre-fix build: Verify hangs on the lost POST (expected)\n");
        return 0;
    }
    assert(lost_post_live_seat(0));
    slow_replay_to_target();
    first_observation();
    multiple_owing_seats();
    missing_seat();
    late_and_duplicate_post();
    new_epoch_and_rematch();
    printf("verify lost-POST protocol: all scenarios passed\n");
    return 0;
}
'''


def run(cmd):
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise AssertionError(f"failed: {' '.join(map(str, cmd))}")
    return proc.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--recomp-net", default=str(ROOT / "lib/recomp-net"))
    args = parser.parse_args()
    rnet = Path(args.recomp_net)
    source = (ROOT / "runtime/src/psx_netplay_rb.c").read_text(encoding="utf-8")

    timeout = "\n".join(line for line in source.splitlines()
                         if line.startswith(("#define RB_VERIFY_TIMEOUT_MS",
                                             "#define RB_POST_BURST")))
    assert timeout.count("#define") == 2
    rx = block_after(source, "while (rnet_session_take_rb_post(s, &epoch, &target, &dig_m, &in_dig, &match))")
    verify = block_after(source, "/* Retransmit POST while Verify waits for peer")
    # The fix under test is in the cut code, not this harness.
    assert "rb_owing_seats_replaying(rnet_rb_quorum_missing(&g_post_q)," in verify
    helpers = ""
    for sig in ("static uint32_t rb_quorum_epoch(void)",
                "static int rb_reply_seat(int from)",
                "static void rb_post_publish(void)",
                "static int rb_owing_seats_progressed(uint32_t owing)",
                "static int rb_owing_seats_replaying(uint32_t owing, uint32_t cap)",
                "static void clear_post_handshake(void)"):
        helpers += sig + "\n{" + function_body(source, sig + "\n{") + "}\n"
    api = SEAT_API.replace("RX_LOOP", rx).replace("VERIFY_BLOCK", verify)
    seat_fixed = timeout + "\n" + SEAT_PRELUDE + helpers + api
    prefix_verify = verify.replace(
        "rb_owing_seats_replaying(rnet_rb_quorum_missing(&g_post_q),\n"
        "                                          rnet_rb_get_target_tick(g_rb))",
        "rb_owing_seats_progressed(rnet_rb_quorum_missing(&g_post_q))")
    assert prefix_verify != verify
    seat_prefix = timeout + "\n" + SEAT_PRELUDE + helpers + \
        SEAT_API.replace("RX_LOOP", rx).replace("VERIFY_BLOCK", prefix_verify)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        (tmp / "rbv_harness.h").write_text(HEADER, encoding="utf-8")
        (tmp / "driver.c").write_text(timeout + "\n" + DRIVER, encoding="utf-8")
        common = [args.compiler, "-std=c11", "-Wall", "-Wno-unused-function",
                  "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                  "-I", str(tmp), "-I", str(rnet / "include")]
        quorum = rnet / "src/rollback/rnet_rb_quorum.c"
        for name, text in (("fixed", seat_fixed), ("prefix", seat_prefix)):
            (tmp / f"seat_{name}.c").write_text(text, encoding="utf-8")
            objs = []
            for i in range(4):
                obj = tmp / f"seat_{name}{i}.o"
                run(common + ["-c", str(tmp / f"seat_{name}.c"), "-o", str(obj),
                              f"-DSEAT_ID={i}", f"-DSEAT_OPS=rbv_seat{i}"])
                objs.append(str(obj))
            exe = tmp / f"rbv_{name}.exe"
            run(common + [str(tmp / "driver.c"), str(quorum)] + objs + ["-o", str(exe)])
            out = run([str(exe)] + (["--prefix"] if name == "prefix" else []))
            print(out.strip())


if __name__ == "__main__":
    main()
