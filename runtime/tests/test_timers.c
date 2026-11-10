#include "timers.h"
#include "psx_video_timing.h"
#include <stdio.h>
#include <string.h>

uint32_t i_stat;
static unsigned irq_count[3];
static unsigned failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)detail;
    i_stat |= 1u << bit;
    ++irq_count[bit - 4];
}
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
static uint32_t addr(unsigned t) { return TIMER_BASE + t * 16u; }
static void setup(unsigned t, uint32_t mode, uint16_t target) {
    timers_init();
    memset(irq_count, 0, sizeof irq_count);
    i_stat = 0;
    timers_write(addr(t) + 8, target);
    timers_write(addr(t) + 4, mode);
}
typedef struct {
    uint16_t counter[3], target[3];
    uint32_t mode[3], frac[3];
    int32_t line[3];
} Snapshot;
static void capture(Snapshot *s) {
    memset(s, 0, sizeof *s);
    timers_get_snapshot(s->counter, s->mode, s->target, s->line, s->frac);
}
static void restore(const Snapshot *s) {
    timers_set_snapshot(s->counter, s->mode, s->target, s->line, s->frac);
}
static void reset_timing(void) {
    static const unsigned expected[] = {0,1,2,3,4,5,6,7,8,9,10,0,0,1,2,3,4,5,6,7,8,9,10,0,0};
    setup(2, 0x58, 10);
    for (unsigned c = 0; c < sizeof expected / sizeof *expected; ++c) {
        CHECK(timers_read(addr(2)) == expected[c]);
        CHECK(irq_count[2] == (c < 10 ? 0u : c < 22 ? 1u : 2u));
        unsigned distance = timers_cycles_to_irq(1u << 6);
        CHECK(distance == (c < 10 ? 10u - c : c < 22 ? 22u - c : 34u - c));
        timers_advance(1);
    }
    /* Divided clock retains the qualified 8 * target period. */
    setup(2, 0x258, 10);
    CHECK(timers_cycles_to_irq(1u << 6) == 80);
    timers_advance(79); CHECK(irq_count[2] == 0);
    timers_advance(1); CHECK(irq_count[2] == 1);
    timers_advance(80); CHECK(irq_count[2] == 2);
    /* Preserve unqualified timer 0/1 clock cadences. */
    for (unsigned t = 0; t < 2; ++t) {
        setup(t, 0x58, 10); timers_advance(100);
        CHECK(irq_count[t] == 10);
    }
}
static void one_shot(void) {
    for (unsigned t = 0; t < 3; ++t) {
        setup(t, 0x18, 10); timers_advance(100);
        CHECK(irq_count[t] == 1);
        CHECK((timers_read(addr(t) + 4) & 0x3FFu) == 0x18);
        CHECK(timers_cycles_to_irq(1u << (4 + t)) == UINT32_MAX);
        CHECK((timers_read(addr(t) + 4) & 0x1800) == 0);
        timers_write(addr(t) + 4, 0x18); timers_advance(100);
        CHECK(irq_count[t] == 2);
        /* Counter writes don't substitute for a mode-write rearm. */
        timers_write(addr(t), 0); timers_advance(100);
        CHECK(irq_count[t] == 2);
    }
    setup(0, 0x30, 0); timers_write(addr(0), 0xFFFF); timers_advance(1);
    CHECK(irq_count[0] == 1); /* coincident target and overflow: one IRQ */
    CHECK((timers_read(addr(0) + 4) & 0x1830) == 0x1830);
    setup(0, 0x98, 10); timers_advance(100);
    CHECK(irq_count[0] == 1);
    CHECK((timers_read(addr(0) + 4) & 0x498) == 0x98);
    setup(0, 0xD8, 10); timers_advance(40);
    CHECK(irq_count[0] == 2); /* toggle raises only on alternate matches */
    CHECK((timers_read(addr(0) + 4) & 0x400) != 0);
}
static void sync_modes(void) {
    for (unsigned div = 0; div <= 0x200; div += 0x200) {
        for (unsigned sync = 0; sync < 4; ++sync) {
            int stopped = sync == 0 || sync == 3;
            setup(2, div | (sync << 1) | 0x51, 200);
            timers_write(addr(2), 123);
            CHECK(timers_cycles_to_irq(1u << 6) == (stopped ? UINT32_MAX : (200u - 123u) * (div ? 8u : 1u)));
            timers_advance(2000);
            CHECK(timers_read(addr(2)) == (stopped ? 123u : 123u + (div ? 250u : 2000u)));
            CHECK(irq_count[2] == (stopped ? 0u : 1u));
            setup(2, div | (sync << 1) | 0x50, 200);
            timers_advance(2000);
            CHECK(timers_read(addr(2)) == (div ? 250u : 2000u));
        }
    }
    setup(2, 1, 10); timers_tick(2000); CHECK(timers_read(addr(2)) == 0);
}
static void snapshots(void) {
    Snapshot s;
    setup(2, 0x18, 10); timers_advance(10); capture(&s);
    CHECK(timers_read(addr(2)) == 10);
    CHECK((timers_read(addr(2) + 4) & ~0x1FFFu) == 0);
    timers_init(); restore(&s);
    CHECK(timers_cycles_to_irq(1u << 6) == UINT32_MAX);
    timers_advance(1); CHECK(timers_read(addr(2)) == 0);
    capture(&s); timers_advance(100); restore(&s);
    timers_advance(1); CHECK(timers_read(addr(2)) == 0);
    timers_advance(1); CHECK(timers_read(addr(2)) == 1);
    CHECK(irq_count[2] == 1);
    /* A counter write cancels a pending target reset. */
    setup(2, 0x58, 10); timers_advance(10);
    timers_write(addr(2), 5); timers_advance(1);
    CHECK(timers_read(addr(2)) == 6);
}
static void batching(void) {
    static const uint32_t modes[] = {0x58,0x18,0xD8,0x258,0x251,0x257,0x30,0x70};
    static const uint16_t targets[] = {0,1,10,0xFFFF};
    for (unsigned t = 0; t < 3; ++t)
    for (unsigned m = 0; m < sizeof modes / sizeof *modes; ++m)
    for (unsigned k = 0; k < sizeof targets / sizeof *targets; ++k) {
        Snapshot a, b;
        setup(t, modes[m], targets[k]); timers_advance(70003); capture(&a);
        unsigned events = irq_count[t];
        setup(t, modes[m], targets[k]);
        for (unsigned c = 0; c < 70003; ++c) timers_advance(1);
        capture(&b);
        CHECK(memcmp(&a, &b, sizeof a) == 0);
        CHECK(irq_count[t] == events);
    }
    setup(2, 0x28, 10);
    CHECK(timers_cycles_to_irq(1u << 6) == UINT32_MAX);
    timers_write(addr(2), 0xFFFE);
    CHECK(timers_cycles_to_irq(1u << 6) == 2);
    timers_advance(2); CHECK(irq_count[2] == 1);
    /* Legacy and block-clock sysclk stepping share target/IRQ semantics. */
    Snapshot a, b;
    setup(2, 0x58, 10); timers_advance(100); capture(&a);
    setup(2, 0x58, 10); timers_tick(100); capture(&b);
    CHECK(memcmp(&a, &b, sizeof a) == 0);
}
int main(void) {
    reset_timing(); one_shot(); sync_modes(); snapshots(); batching();
    if (failures) fprintf(stderr, "%u failures\n", failures);
    else puts("timer register, IRQ, snapshot and batching checks passed");
    return failures ? 1 : 0;
}
