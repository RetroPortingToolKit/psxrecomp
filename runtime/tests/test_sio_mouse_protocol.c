/* PlayStation Mouse (SCPH-1030) wire protocol regression: ID 5A12h, the
 * six-byte 0x42 poll with ACK on every byte but the last, active-low button
 * bits 10 (right) and 11 (left), signed relative motion that accumulates
 * between polls and is consumed exactly once, large motion spread over
 * several polls, click latching, the quarter-second stale clear, unsupported
 * commands, device swaps with the DualShock family and NeGcon, snapshot and
 * multitap seats (built a second time with PSX_MAX_PLAYERS=5).
 *
 * Reference: PSX-SPX controllersandmemorycards "Mouse" and Mednafen
 * psx/input/mouse.cpp (also the libretro Beetle PSX core). */

#include "sio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t sio_snapshot_bytes(void);
void sio_snapshot_write(uint8_t *p);
int sio_snapshot_read(const uint8_t *p, uint32_t len);

uint32_t i_stat = 0;
uint32_t i_mask = 0;
uint32_t g_debug_current_func_addr = 0;
uint32_t g_debug_last_store_pc = 0;
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_read_word(uint32_t addr) { (void)addr; return 0; }

/* sio.c gates card-transfer deferral on `psx_get_cycle_count() < deadline`, so
 * a constant clock would defer forever. Advance monotonically; tests jump it
 * forward to cross the mouse's stale-motion window. */
static uint64_t s_clock;
uint64_t psx_get_cycle_count(void) { return s_clock += 64; }
static void clock_skip(uint64_t cycles) { s_clock += cycles; }
uint32_t memory_get_sr(void) { return 0; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t a, uint32_t v, uint8_t w) {
    (void)a; (void)v; (void)w;
}
void starvation_ring_record(uint8_t k, uint8_t tx, uint8_t rx, uint16_t c,
                            uint16_t st, uint8_t d, uint8_t ss, uint16_t ms,
                            uint8_t mc, uint16_t sec, uint8_t di, uint32_t f) {
    (void)k; (void)tx; (void)rx; (void)c; (void)st; (void)d; (void)ss;
    (void)ms; (void)mc; (void)sec; (void)di; (void)f;
}
void card_read_summary_record(uint8_t s, uint8_t c, uint16_t sec, uint8_t e,
                              uint8_t chk, uint8_t d0, uint8_t d1, uint32_t f) {
    (void)s; (void)c; (void)sec; (void)e; (void)chk; (void)d0; (void)d1; (void)f;
}
void card_data_writes_arm(uint8_t v, uint16_t s, uint8_t i, uint8_t sl) {
    (void)v; (void)s; (void)i; (void)sl;
}
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) {
    (void)k; (void)d; (void)a;
}
void psx_irq_raise(uint32_t bit, uint32_t detail) {
    (void)detail; i_stat |= 1u << bit;
}
int memcard_is_present(int card) { (void)card; return 0; }
int memcard_read_sector(int card, int sector, uint8_t *buf) {
    (void)card; (void)sector; memset(buf, 0, 128); return -1;
}
int memcard_write_sector(int card, int sector, const uint8_t *buf) {
    (void)card; (void)sector; (void)buf; return -1;
}
void memcard_flush(int card) { (void)card; }
int memcard_last_flush_result(int card) { (void)card; return -1; }

#define SIO_TX_DATA 0x1F801040u
#define SIO_RX_DATA 0x1F801040u
#define SIO_CTRL    0x1F80104Au
#define CTRL_TX_EN      (1u << 0)
#define CTRL_SELECT     (1u << 1)
#define CTRL_ACK        (1u << 4)
#define CTRL_ACK_IRQ_EN (1u << 12)
#define CTRL_SLOT       (1u << 13)

#define NONE  0xFFFFu
#define LEFT  ((uint16_t)~SIO_MOUSE_BTN_LEFT)
#define RIGHT ((uint16_t)~SIO_MOUSE_BTN_RIGHT)
#define BOTH  ((uint16_t)~(SIO_MOUSE_BTN_LEFT | SIO_MOUSE_BTN_RIGHT))

static int failures;

#define EXPECT(label, expected, actual) do {                                  \
    unsigned e_ = (unsigned)(expected), a_ = (unsigned)(actual);              \
    if (e_ != a_) {                                                           \
        fprintf(stderr, "FAIL %s: expected=0x%X actual=0x%X\n",             \
                label, e_, a_);                                               \
        failures++;                                                           \
    }                                                                         \
} while (0)

static int last_ack;

/* One byte on the pad bus; last_ack reports whether the device pulsed ACK
 * (IRQ7) after it, i.e. whether it wants another byte. */
static uint8_t xchg(int slot, uint8_t tx) {
    const uint16_t ctrl = (uint16_t)(CTRL_TX_EN | CTRL_SELECT |
        CTRL_ACK_IRQ_EN | (slot ? CTRL_SLOT : 0));
    i_stat = 0;
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, tx);
    sio_tick(2000);
    const uint8_t rx = (uint8_t)sio_read(SIO_RX_DATA);
    last_ack = (i_stat & (1u << 7)) ? 1 : 0;
    sio_write(SIO_CTRL, ctrl | CTRL_ACK);
    return rx;
}

/* Full mouse 0x42 poll; returns the ID and fills the four bytes after 0x5A
 * (buttons low, buttons high, X, Y). */
/* Deassert /SEL between transactions, as a pad driver does; the port bit is
 * latched when the next transaction opens. */
static void deselect(void) { sio_write(SIO_CTRL, 0); }

static uint8_t mouse_poll(int slot, uint8_t out[4]) {
    uint8_t id;
    deselect();
    (void)xchg(slot, 0x01);
    id = xchg(slot, 0x42);
    EXPECT("poll.status", 0x5A, xchg(slot, 0x00));
    for (int i = 0; i < 4; i++) out[i] = xchg(slot, 0x00);
    EXPECT("poll.end.noack", 0, last_ack);
    return id;
}

/* ID of the next 0x42 for any pad type, draining the rest of its frame. */
static uint8_t poll_id(int slot) {
    uint8_t id;
    deselect();
    (void)xchg(slot, 0x01);
    id = xchg(slot, 0x42);
    while (last_ack) (void)xchg(slot, 0x00);
    return id;
}

static void mouse_on(int slot) {
    sio_init();
    sio_connect_pad(slot);
    sio_set_pad_type(slot, SIO_PAD_MOUSE, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_config_capable(slot, 0);
}

/* Cases 1-4: ID 0x12, status 0x5A, six-byte frame with the ACK pattern, and
 * the neutral frame. */
static void wire(void) {
    mouse_on(0);
    EXPECT("wire.type", SIO_PAD_MOUSE, sio_get_pad_analog(0));
    EXPECT("wire.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("wire.prefix.ack", 1, last_ack);
    EXPECT("wire.id", 0x12, xchg(0, 0x42));
    EXPECT("wire.id.ack", 1, last_ack);
    EXPECT("wire.status", 0x5A, xchg(0, 0x00));
    EXPECT("wire.status.ack", 1, last_ack);
    EXPECT("wire.buttons.low", 0xFF, xchg(0, 0x00));
    EXPECT("wire.buttons.low.ack", 1, last_ack);
    EXPECT("wire.buttons.high", 0xFC, xchg(0, 0x00));
    EXPECT("wire.buttons.high.ack", 1, last_ack);
    EXPECT("wire.x", 0x00, xchg(0, 0x00));
    EXPECT("wire.x.ack", 1, last_ack);
    EXPECT("wire.y", 0x00, xchg(0, 0x00));
    EXPECT("wire.y.noack", 0, last_ack);
    /* Frame over: the next byte is hi-z and the bus is idle again. */
    EXPECT("wire.after.hiz", 0xFF, xchg(0, 0x00));
    EXPECT("wire.after.noack", 0, last_ack);

    /* The second port answers on its own, the first stays a mouse. */
    sio_connect_pad(1);
    sio_set_pad_type(1, SIO_PAD_DIGITAL, 0x80, 0x80, 0x80, 0x80);
    EXPECT("wire.port2.digital", 0x41, poll_id(1));
    EXPECT("wire.port1.still.mouse", 0x12, poll_id(0));
}

/* Cases 5-6: left and right buttons, pressed and released, active low. */
static void buttons(void) {
    uint8_t d[4];
    mouse_on(0);
    sio_set_pad_mouse(0, 0, 0, LEFT);
    EXPECT("left.id", 0x12, mouse_poll(0, d));
    EXPECT("left.low", 0xFF, d[0]);
    EXPECT("left.high", 0xF4, d[1]);
    sio_set_pad_mouse(0, 0, 0, NONE);
    mouse_poll(0, d);
    EXPECT("left.released", 0xFC, d[1]);

    sio_set_pad_mouse(0, 0, 0, RIGHT);
    mouse_poll(0, d);
    EXPECT("right.high", 0xF8, d[1]);
    sio_set_pad_mouse(0, 0, 0, NONE);
    mouse_poll(0, d);
    EXPECT("right.released", 0xFC, d[1]);

    sio_set_pad_mouse(0, 0, 0, BOTH);
    mouse_poll(0, d);
    EXPECT("both.high", 0xF0, d[1]);
    /* Held across polls: reported every poll until released. */
    mouse_poll(0, d);
    EXPECT("both.held", 0xF0, d[1]);
    sio_set_pad_mouse(0, 0, 0, NONE);
    mouse_poll(0, d);
    EXPECT("both.released", 0xFC, d[1]);

    /* Pad buttons outside bits 10/11 never reach the mouse frame. */
    sio_set_pad_mouse(0, 0, 0, 0x0000u);
    mouse_poll(0, d);
    EXPECT("other.bits.low", 0xFF, d[0]);
    EXPECT("other.bits.high", 0xF0, d[1]);
    sio_set_pad_mouse(0, 0, 0, NONE);
    mouse_poll(0, d);

    /* A click between two polls is reported once, then released. */
    sio_set_pad_mouse(0, 0, 0, LEFT);
    sio_set_pad_mouse(0, 0, 0, NONE);
    mouse_poll(0, d);
    EXPECT("click.latched", 0xF4, d[1]);
    mouse_poll(0, d);
    EXPECT("click.once", 0xFC, d[1]);
}

/* Cases 7-10: signed X/Y, summing between polls, no double reporting. */
static void motion(void) {
    uint8_t d[4];
    int dx = 0, dy = 0;
    mouse_on(0);
    sio_set_pad_mouse(0, 5, 0, NONE);
    mouse_poll(0, d);
    EXPECT("x.pos", 0x05, d[2]);
    EXPECT("x.pos.y", 0x00, d[3]);
    sio_set_pad_mouse(0, -5, 0, NONE);
    mouse_poll(0, d);
    EXPECT("x.neg", 0xFB, d[2]);
    sio_set_pad_mouse(0, 0, 9, NONE);
    mouse_poll(0, d);
    EXPECT("y.pos.x", 0x00, d[2]);
    EXPECT("y.pos", 0x09, d[3]);
    sio_set_pad_mouse(0, 0, -9, NONE);
    mouse_poll(0, d);
    EXPECT("y.neg", 0xF7, d[3]);

    sio_set_pad_mouse(0, 3, -2, NONE);
    sio_set_pad_mouse(0, 4, -6, NONE);
    sio_set_pad_mouse(0, -1, 1, NONE);
    sio_get_pad_mouse(0, &dx, &dy);
    EXPECT("sum.owed.x", 6, dx);
    EXPECT("sum.owed.y", (unsigned)-7, dy);
    mouse_poll(0, d);
    EXPECT("sum.x", 0x06, d[2]);
    EXPECT("sum.y", 0xF9, d[3]);
    sio_get_pad_mouse(0, &dx, &dy);
    EXPECT("sum.consumed.x", 0, dx);
    EXPECT("sum.consumed.y", 0, dy);
    mouse_poll(0, d);
    EXPECT("once.x", 0x00, d[2]);
    EXPECT("once.y", 0x00, d[3]);

    /* Opposite motion between polls cancels out. */
    sio_set_pad_mouse(0, 40, 0, NONE);
    sio_set_pad_mouse(0, -40, 0, NONE);
    mouse_poll(0, d);
    EXPECT("cancel.x", 0x00, d[2]);
}

/* Case 11: motion beyond a signed byte is clipped per poll and the rest is
 * delivered by later polls; the device accumulator saturates at 30 polls'
 * worth like Mednafen's. */
static void large_motion(void) {
    uint8_t d[4];
    int dx = 0, dy = 0, sum_x = 0, sum_y = 0, polls = 0;
    mouse_on(0);
    sio_set_pad_mouse(0, 300, -300, NONE);
    mouse_poll(0, d);
    EXPECT("large.1.x", 0x7F, d[2]);
    EXPECT("large.1.y", 0x80, d[3]);
    mouse_poll(0, d);
    EXPECT("large.2.x", 0x7F, d[2]);
    EXPECT("large.2.y", 0x80, d[3]);
    mouse_poll(0, d);
    EXPECT("large.3.x", 46, d[2]);
    EXPECT("large.3.y", (uint8_t)-44, d[3]);
    mouse_poll(0, d);
    EXPECT("large.4.x", 0, d[2]);
    EXPECT("large.4.y", 0, d[3]);

    sio_set_pad_mouse(0, 100000, -100000, NONE);
    sio_get_pad_mouse(0, &dx, &dy);
    EXPECT("clamp.x", 30 * 127, dx);
    EXPECT("clamp.y", (unsigned)(30 * -128), dy);
    do {
        mouse_poll(0, d);
        sum_x += (int8_t)d[2];
        sum_y += (int8_t)d[3];
    } while ((d[2] || d[3]) && ++polls < 64);
    EXPECT("clamp.delivered.x", 30 * 127, sum_x);
    EXPECT("clamp.delivered.y", (unsigned)(30 * -128), sum_y);
    EXPECT("clamp.polls", 30, polls);
}

/* Case 12: commands other than 0x42 (config, rumble, garbage) get the ID with
 * no ACK and end the transaction; they neither enter config mode nor eat the
 * owed motion. A digital-pad style address other than 0x01 is hi-z. */
static void unsupported(void) {
    static const uint8_t cmds[] = { 0x43, 0x44, 0x45, 0x46, 0x47, 0x4C, 0x4D,
                                    0x4F, 0x40, 0x00, 0xFF };
    uint8_t d[4];
    mouse_on(0);
    sio_set_pad_mouse(0, 12, -3, LEFT);
    for (unsigned i = 0; i < sizeof cmds; i++) {
        EXPECT("cmd.prefix", 0xFF, xchg(0, 0x01));
        EXPECT("cmd.prefix.ack", 1, last_ack);
        EXPECT("cmd.id", 0x12, xchg(0, cmds[i]));
        EXPECT("cmd.noack", 0, last_ack);
        EXPECT("cmd.hiz", 0xFF, xchg(0, 0x00));
        EXPECT("cmd.hiz.noack", 0, last_ack);
    }
    EXPECT("cmd.no.config", SIO_PAD_MOUSE, sio_get_pad_analog(0));
    EXPECT("cmd.addr.hiz", 0xFF, xchg(0, 0x02));
    EXPECT("cmd.addr.noack", 0, last_ack);
    EXPECT("cmd.after.id", 0x12, mouse_poll(0, d));
    EXPECT("cmd.after.left", 0xF4, d[1]);
    EXPECT("cmd.after.x", 12, d[2]);
    EXPECT("cmd.after.y", (uint8_t)-3, d[3]);

    /* Bytes the game sends during a poll drive no motors. */
    {
        uint8_t small = 1, large = 1;
        (void)xchg(0, 0x01); (void)xchg(0, 0x42); (void)xchg(0, 0x00);
        for (int i = 0; i < 4; i++) (void)xchg(0, 0xFF);
        sio_get_pad_rumble(0, &small, &large);
        EXPECT("rumble.small", 0, small);
        EXPECT("rumble.large", 0, large);
    }
}

static const uint8_t k_enter[6] = { 0x01, 0, 0, 0, 0, 0 };
static const uint8_t k_exit[6]  = { 0x00, 0, 0, 0, 0, 0 };
static const uint8_t k_lock_analog[6] = { 0x01, 0x03, 0, 0, 0, 0 };

static uint8_t config_cmd(int slot, uint8_t cmd, const uint8_t data[6]) {
    uint8_t id;
    (void)xchg(slot, 0x01);
    id = xchg(slot, cmd);
    (void)xchg(slot, 0x00);
    for (int i = 0; i < 6; i++) (void)xchg(slot, data[i]);
    return id;
}

/* Case 13: switching between the mouse and the other controllers. A swap is a
 * different device in the port: it applies at the next idle bus boundary even
 * over a game's 0x44 lock, waits out a config handshake, and leaves no motion
 * or clicks owed by the mouse that left. */
static void switching(void) {
    uint8_t d[4];
    int dx = 0, dy = 0;
    mouse_on(0);
    sio_set_pad_mouse(0, 50, 50, LEFT);

    sio_set_pad_config_capable(0, 1);
    sio_request_pad_type(0, SIO_PAD_DUALSHOCK);
    EXPECT("swap.mouse.until.idle", SIO_PAD_MOUSE, sio_get_pad_analog(0));
    EXPECT("swap.to.ds", 0x73, poll_id(0));
    sio_get_pad_mouse(0, &dx, &dy);
    EXPECT("swap.to.ds.cleared", 0, dx);
    /* Mouse input is ignored while another device is in the port. */
    sio_set_pad_mouse(0, 20, 20, LEFT);
    sio_get_pad_mouse(0, &dx, &dy);
    EXPECT("swap.ds.ignores.mouse", 0, dx);

    /* A locked DualShock gives way to the mouse, and the lock goes with it. */
    EXPECT("swap.enter", 0x73, config_cmd(0, 0x43, k_enter));
    EXPECT("swap.lock", 0xF3, config_cmd(0, 0x44, k_lock_analog));
    sio_request_pad_type(0, SIO_PAD_MOUSE);
    EXPECT("swap.held.in.config", 0xF3, config_cmd(0, 0x43, k_exit));
    EXPECT("swap.locked", 1, sio_get_pad_mode_locked(0));
    EXPECT("swap.to.mouse", 0x12, mouse_poll(0, d));
    EXPECT("swap.to.mouse.unlocked", 0, sio_get_pad_mode_locked(0));
    EXPECT("swap.to.mouse.neutral.btn", 0xFC, d[1]);
    EXPECT("swap.to.mouse.neutral.x", 0x00, d[2]);

    sio_request_pad_type(0, SIO_PAD_DIGITAL);
    EXPECT("swap.to.digital", 0x41, poll_id(0));
    sio_request_pad_type(0, SIO_PAD_NEGCON);
    EXPECT("swap.to.negcon", 0x23, poll_id(0));
    sio_request_pad_type(0, SIO_PAD_MOUSE);
    EXPECT("swap.negcon.to.mouse", 0x12, poll_id(0));
    sio_request_pad_type(0, SIO_PAD_JOGCON);
    EXPECT("swap.to.jogcon", 0xE3, poll_id(0));

    /* Out-of-range types stay rejected. */
    sio_set_pad_type(0, SIO_PAD_MOUSE + 1, 0x80, 0x80, 0x80, 0x80);
    EXPECT("swap.bad.type", SIO_PAD_DIGITAL, sio_get_pad_analog(0));

    /* Unplugging drops owed motion; an empty port is hi-z with no ACK. */
    sio_set_pad_type(0, SIO_PAD_MOUSE, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_mouse(0, 7, 7, NONE);
    sio_set_pad_connected(0, 0);
    EXPECT("unplug.hiz", 0xFF, xchg(0, 0x01));
    EXPECT("unplug.noack", 0, last_ack);
    sio_set_pad_connected(0, 1);
    sio_set_pad_type(0, SIO_PAD_MOUSE, 0x80, 0x80, 0x80, 0x80);
    mouse_poll(0, d);
    EXPECT("replug.x", 0x00, d[2]);
}

/* A mouse that is not polled for a quarter second forgets owed motion and
 * clicks (Mednafen clear_timeout); one that is polled keeps accumulating. */
static void stale_clear(void) {
    uint8_t d[4];
    int dx = 0, dy = 0;
    mouse_on(0);
    sio_set_pad_mouse(0, 30, 0, NONE);
    mouse_poll(0, d);
    sio_set_pad_mouse(0, 10, 0, LEFT);
    sio_set_pad_mouse(0, 0, 0, NONE);
    clock_skip(33868800u / 4u);
    sio_set_pad_mouse(0, 2, 0, NONE);
    sio_get_pad_mouse(0, &dx, &dy);
    EXPECT("stale.dropped", 2, dx);
    mouse_poll(0, d);
    EXPECT("stale.x", 2, d[2]);
    EXPECT("stale.click.dropped", 0xFC, d[1]);

    clock_skip(33868800u / 8u);
    sio_set_pad_mouse(0, 4, 0, NONE);
    sio_get_pad_mouse(0, &dx, &dy);
    EXPECT("fresh.kept", 4, dx);
}

/* Owed motion and latched clicks survive rollback; a snapshot from before
 * the mouse section still loads and owes nothing. */
static void snapshot(void) {
    uint8_t d[4];
    const uint32_t mouse_bytes = 14u * PSX_MAX_PLAYERS;
    mouse_on(0);
    sio_set_pad_mouse(0, 200, -5, LEFT);
    sio_set_pad_mouse(0, 0, 0, NONE);

    const uint32_t len = sio_snapshot_bytes();
    uint8_t *snap = (uint8_t *)malloc(len);
    if (!snap) { failures++; return; }
    sio_snapshot_write(snap);

    mouse_poll(0, d);
    mouse_poll(0, d);
    EXPECT("snap.restore", 1, sio_snapshot_read(snap, len));
    EXPECT("snap.restore.type", SIO_PAD_MOUSE, sio_get_pad_analog(0));
    mouse_poll(0, d);
    EXPECT("snap.restore.click", 0xF4, d[1]);
    EXPECT("snap.restore.x", 0x7F, d[2]);
    EXPECT("snap.restore.y", (uint8_t)-5, d[3]);
    mouse_poll(0, d);
    EXPECT("snap.restore.rest", 200 - 127, d[2]);

    EXPECT("snap.legacy.restore", 1, sio_snapshot_read(snap, len - mouse_bytes));
    EXPECT("snap.legacy.type", SIO_PAD_MOUSE, sio_get_pad_analog(0));
    mouse_poll(0, d);
    EXPECT("snap.legacy.x", 0, d[2]);
    EXPECT("snap.legacy.btn", 0xFC, d[1]);
    EXPECT("snap.bad.len", 0, sio_snapshot_read(snap, len - 1));
    free(snap);
}

#if PSX_MAX_PLAYERS >= 5
/* Tap seats never present a mouse, with or without the DualShock-on-tap
 * hack; the lone pad on the opposite console port still can. */
static void multitap(void) {
    sio_init();
    sio_set_multitap(1);
    sio_set_multitap_port(0);
    sio_set_multitap_analog(1);
    sio_connect_pad(0);
    sio_connect_pad(4);
    sio_set_pad_type(0, SIO_PAD_MOUSE, 0x80, 0x80, 0x80, 0x80);
    EXPECT("tap.seat.digital", SIO_PAD_DIGITAL, sio_get_pad_analog(0));
    sio_request_pad_type(0, SIO_PAD_MOUSE);
    EXPECT("tap.seat.request", 0x41, poll_id(0));

    sio_set_pad_type(4, SIO_PAD_MOUSE, 0x80, 0x80, 0x80, 0x80);
    EXPECT("tap.lone.mouse", SIO_PAD_MOUSE, sio_get_pad_analog(4));
    sio_set_pad_mouse(4, -3, 3, RIGHT);
    {
        uint8_t d[4];
        EXPECT("tap.lone.id", 0x12, mouse_poll(1, d));
        EXPECT("tap.lone.btn", 0xF8, d[1]);
        EXPECT("tap.lone.x", (uint8_t)-3, d[2]);
    }
    sio_set_multitap(0);

    /* Arming the tap under a mouse already on a seat demotes it. */
    sio_init();
    sio_connect_pad(0);
    sio_set_pad_type(0, SIO_PAD_MOUSE, 0x80, 0x80, 0x80, 0x80);
    sio_set_multitap_port(0);
    sio_set_multitap(1);
    EXPECT("tap.armed.demotes", 0x41, poll_id(0));
    sio_set_multitap(0);
}
#endif

int main(void) {
    wire();
    buttons();
    motion();
    large_motion();
    unsupported();
    switching();
    stale_clear();
    snapshot();
#if PSX_MAX_PLAYERS >= 5
    multitap();
#endif
    if (failures) {
        fprintf(stderr, "sio_mouse_protocol_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("sio_mouse_protocol_test: OK\n");
    return 0;
}
