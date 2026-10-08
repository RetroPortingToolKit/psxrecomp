/* NeGcon wire protocol regression: ID 0x23 poll layout (twist, I, II, L),
 * no config mode, device-swap edges against the 0x44 mode lock, deferred type
 * requests during config, 0x44 keeping the device, snapshot of the pad mode,
 * and multitap force-digital (built a second time with PSX_MAX_PLAYERS=5). */

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
 * a constant clock would defer forever. Advance monotonically. */
uint64_t psx_get_cycle_count(void) {
    static uint64_t t;
    return t += 64;
}
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

#define SIO_TX_DATA 0x1F801040u
#define SIO_RX_DATA 0x1F801040u
#define SIO_CTRL    0x1F80104Au
#define CTRL_TX_EN      (1u << 0)
#define CTRL_SELECT     (1u << 1)
#define CTRL_ACK        (1u << 4)
#define CTRL_ACK_IRQ_EN (1u << 12)
#define CTRL_SLOT       (1u << 13)

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

/* Full 0x42 poll; returns the ID and fills the six bytes after 0x5A. */
static uint8_t poll(int slot, uint8_t out[6]) {
    uint8_t id;
    (void)xchg(slot, 0x01);
    id = xchg(slot, 0x42);
    EXPECT("poll.status", 0x5A, xchg(slot, 0x00));
    for (int i = 0; i < 6; i++) out[i] = xchg(slot, 0x00);
    return id;
}

/* Short 0x42 poll for a 0x41 pad (two data bytes). */
static uint8_t poll_id(int slot) {
    uint8_t d[6];
    uint8_t id;
    (void)xchg(slot, 0x01);
    id = xchg(slot, 0x42);
    if (id == 0x41) {
        (void)xchg(slot, 0x00); (void)xchg(slot, 0x00); (void)xchg(slot, 0x00);
        return id;
    }
    (void)xchg(slot, 0x00);
    for (int i = 0; i < 6; i++) d[i] = xchg(slot, 0x00);
    (void)d;
    return id;
}

/* 8-byte config command (0x43/0x44/0x4D...) with explicit data bytes. */
static uint8_t config_cmd(int slot, uint8_t cmd, const uint8_t data[6],
                          uint8_t echo[6]) {
    uint8_t id;
    (void)xchg(slot, 0x01);
    id = xchg(slot, cmd);
    (void)xchg(slot, 0x00);
    for (int i = 0; i < 6; i++) {
        uint8_t r = xchg(slot, data[i]);
        if (echo) echo[i] = r;
    }
    return id;
}

static const uint8_t k_enter[6]  = { 0x01, 0, 0, 0, 0, 0 };
static const uint8_t k_exit[6]   = { 0x00, 0, 0, 0, 0, 0 };
static const uint8_t k_lock_analog[6] = { 0x01, 0x03, 0, 0, 0, 0 };
static const uint8_t k_digital[6] = { 0x00, 0x02, 0, 0, 0, 0 };
static const uint8_t k_analog[6]  = { 0x01, 0x02, 0, 0, 0, 0 };
static const uint8_t k_map[6]    = { 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF };

static void negcon_wire(void) {
    uint8_t d[6];
    sio_init();
    sio_connect_pad(0);
    sio_set_pad_type(0, SIO_PAD_NEGCON, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_state_slot(0, 0xBFEFu);           /* Up + Cross held */
    sio_set_pad_sticks(0, 0x30, 0x11, 0x22, 0x33); /* twist = left X */
    sio_set_pad_negcon(0, 0xC0, 0x40, 0x07);
    EXPECT("wire.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("wire.prefix.ack", 1, last_ack);
    EXPECT("wire.id", 0x23, xchg(0, 0x42));
    EXPECT("wire.id.ack", 1, last_ack);
    EXPECT("wire.status", 0x5A, xchg(0, 0x00));
    EXPECT("wire.buttons.low", 0xEF, xchg(0, 0x00));
    EXPECT("wire.buttons.high", 0xBF, xchg(0, 0x00));
    EXPECT("wire.twist", 0x30, xchg(0, 0x00));
    EXPECT("wire.I", 0xC0, xchg(0, 0x00));
    EXPECT("wire.II", 0x40, xchg(0, 0x00));
    EXPECT("wire.L.ack", 1, last_ack);
    EXPECT("wire.L", 0x07, xchg(0, 0x00));
    EXPECT("wire.end.noack", 0, last_ack);

    /* Not config-capable: any other command gets the ID byte with no ACK and
     * the transaction ends; 0x43 never latches config mode. */
    EXPECT("cfg.prefix", 0xFF, xchg(0, 0x01));
    EXPECT("cfg.id", 0x23, xchg(0, 0x43));
    EXPECT("cfg.noack", 0, last_ack);
    EXPECT("cfg.hiz", 0xFF, xchg(0, 0x00));
    EXPECT("cfg.after.id", 0x23, poll(0, d));
    EXPECT("cfg.after.I", 0xC0, d[3]);

    /* Pressure is host input: the next poll reports the new sample. */
    sio_set_pad_negcon(0, 0x00, 0xFF, 0x00);
    EXPECT("resample.id", 0x23, poll(0, d));
    EXPECT("resample.I", 0x00, d[3]);
    EXPECT("resample.II", 0xFF, d[4]);
    {
        uint8_t back[3];
        sio_get_pad_negcon(0, back);
        EXPECT("getter.II", 0xFF, back[1]);
    }
}

/* A game-locked DualShock holds analog-button flips but not a NeGcon swap;
 * the swap clears the lock and the motor map, and the DualShock that comes
 * back powers up unlocked. */
static void lock_and_swap(void) {
    uint8_t d[6], echo[6];
    sio_init();
    sio_connect_pad(0);
    sio_set_pad_type(0, SIO_PAD_DUALSHOCK, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_config_capable(0, 1);
    EXPECT("lock.enter.id", 0x73, config_cmd(0, 0x43, k_enter, NULL));
    EXPECT("lock.mode.id", 0xF3, config_cmd(0, 0x44, k_lock_analog, NULL));
    EXPECT("lock.map.id", 0xF3, config_cmd(0, 0x4D, k_map, NULL));
    EXPECT("lock.exit.id", 0xF3, config_cmd(0, 0x43, k_exit, NULL));
    EXPECT("lock.locked", 1, sio_get_pad_mode_locked(0));

    sio_request_pad_type(0, SIO_PAD_DIGITAL);
    EXPECT("lock.holds.digital", 0x73, poll(0, d));
    EXPECT("lock.holds.type", SIO_PAD_DUALSHOCK, sio_get_pad_analog(0));

    sio_set_pad_negcon(0, 0x99, 0, 0);
    sio_request_pad_type(0, SIO_PAD_NEGCON);
    EXPECT("swap.in.id", 0x23, poll(0, d));
    EXPECT("swap.in.I", 0x99, d[3]);
    EXPECT("swap.in.unlocked", 0, sio_get_pad_mode_locked(0));
    /* A NeGcon has no motors: game bytes in a poll drive nothing. */
    {
        uint8_t small = 1, large = 1;
        (void)xchg(0, 0x01); (void)xchg(0, 0x42); (void)xchg(0, 0x00);
        (void)xchg(0, 0xFF); (void)xchg(0, 0xFF);
        for (int i = 0; i < 4; i++) (void)xchg(0, 0x00);
        sio_get_pad_rumble(0, &small, &large);
        EXPECT("swap.in.small", 0, small);
        EXPECT("swap.in.large", 0, large);
    }

    sio_request_pad_type(0, SIO_PAD_DUALSHOCK);
    EXPECT("swap.out.id", 0x73, poll(0, d));
    sio_request_pad_type(0, SIO_PAD_DIGITAL);
    EXPECT("swap.out.unlocked.flip", 0x41, poll_id(0));
    sio_request_pad_type(0, SIO_PAD_DUALSHOCK);
    EXPECT("swap.out.back", 0x73, poll(0, d));
    EXPECT("swap.out.enter", 0x73, config_cmd(0, 0x43, k_enter, NULL));
    EXPECT("swap.out.map.id", 0xF3, config_cmd(0, 0x4D, k_map, echo));
    for (int i = 0; i < 6; i++)
        EXPECT("swap.out.map.unassigned", 0xFF, echo[i]);
    (void)config_cmd(0, 0x43, k_exit, NULL);
}

/* A type request raised during a config handshake waits for config exit. */
static void deferred_during_config(void) {
    uint8_t d[6];
    sio_init();
    sio_connect_pad(0);
    sio_set_pad_type(0, SIO_PAD_DUALSHOCK, 0x80, 0x80, 0x80, 0x80);
    EXPECT("defer.enter", 0x73, config_cmd(0, 0x43, k_enter, NULL));
    sio_request_pad_type(0, SIO_PAD_NEGCON);
    EXPECT("defer.in.config", 0xF3, poll(0, d));
    EXPECT("defer.type.held", SIO_PAD_DUALSHOCK, sio_get_pad_analog(0));
    EXPECT("defer.exit", 0xF3, config_cmd(0, 0x43, k_exit, NULL));
    EXPECT("defer.applied", 0x23, poll(0, d));
}

/* 0x44 selects the analog mode of the device in the port: a JogCon that the
 * game put in digital mode returns as a JogCon, not a DualShock. */
static void set_mode_keeps_device(void) {
    uint8_t d[6];
    sio_init();
    sio_connect_pad(0);
    sio_set_pad_type(0, SIO_PAD_JOGCON, 0x80, 0x80, 0x80, 0x80);
    EXPECT("0x44.enter", 0xE3, config_cmd(0, 0x43, k_enter, NULL));
    (void)config_cmd(0, 0x44, k_digital, NULL);
    EXPECT("0x44.digital", SIO_PAD_DIGITAL, sio_get_pad_analog(0));
    (void)config_cmd(0, 0x44, k_analog, NULL);
    EXPECT("0x44.jogcon", SIO_PAD_JOGCON, sio_get_pad_analog(0));
    (void)config_cmd(0, 0x43, k_exit, NULL);
    EXPECT("0x44.poll", 0xE3, poll(0, d));

    sio_set_pad_type(0, SIO_PAD_DUALSHOCK, 0x80, 0x80, 0x80, 0x80);
    EXPECT("0x44.ds.enter", 0x73, config_cmd(0, 0x43, k_enter, NULL));
    (void)config_cmd(0, 0x44, k_digital, NULL);
    (void)config_cmd(0, 0x44, k_analog, NULL);
    EXPECT("0x44.dualshock", SIO_PAD_DUALSHOCK, sio_get_pad_analog(0));
    (void)config_cmd(0, 0x43, k_exit, NULL);
}

/* Lock and device survive rollback; a snapshot from before the pad-mode
 * section still loads, unlocked. */
static void snapshot_pad_mode(void) {
    uint8_t d[6];
    const uint32_t mode_bytes = 2u * PSX_MAX_PLAYERS;
    sio_init();
    sio_connect_pad(0);
    sio_set_pad_type(0, SIO_PAD_JOGCON, 0x80, 0x80, 0x80, 0x80);
    (void)config_cmd(0, 0x43, k_enter, NULL);
    (void)config_cmd(0, 0x44, k_lock_analog, NULL);
    (void)config_cmd(0, 0x43, k_exit, NULL);
    EXPECT("snap.locked", 1, sio_get_pad_mode_locked(0));

    const uint32_t len = sio_snapshot_bytes();
    uint8_t *snap = (uint8_t *)malloc(len);
    if (!snap) { failures++; return; }
    sio_snapshot_write(snap);

    sio_request_pad_type(0, SIO_PAD_NEGCON);
    EXPECT("snap.swap", 0x23, poll(0, d));
    EXPECT("snap.swap.unlocked", 0, sio_get_pad_mode_locked(0));

    EXPECT("snap.restore", 1, sio_snapshot_read(snap, len));
    EXPECT("snap.restore.type", SIO_PAD_JOGCON, sio_get_pad_analog(0));
    EXPECT("snap.restore.locked", 1, sio_get_pad_mode_locked(0));
    sio_request_pad_type(0, SIO_PAD_DIGITAL);
    EXPECT("snap.restore.lock.holds", 0xE3, poll(0, d));

    EXPECT("snap.legacy.restore", 1, sio_snapshot_read(snap, len - mode_bytes));
    EXPECT("snap.legacy.unlocked", 0, sio_get_pad_mode_locked(0));
    EXPECT("snap.bad.len", 0, sio_snapshot_read(snap, len - 1));
    free(snap);
}

#if PSX_MAX_PLAYERS >= 5
/* Tap seats are digital unless the DualShock-on-tap hack is armed; with it,
 * the bulk status block carries the NeGcon payload. */
static void multitap(void) {
    uint8_t r[34];
    sio_init();
    sio_set_multitap(1);
    sio_set_multitap_port(0);
    sio_set_multitap_analog(0);
    sio_connect_pad(0);
    sio_set_pad_negcon(0, 0x10, 0x20, 0x30);
    sio_set_pad_sticks(0, 0x44, 0x80, 0x80, 0x80);
    sio_request_pad_type(0, SIO_PAD_NEGCON);
    (void)xchg(0, 0x01);
    EXPECT("tap.forced.digital.id", 0x41, xchg(0, 0x42));
    (void)xchg(0, 0x00); (void)xchg(0, 0x00); (void)xchg(0, 0x00);
    EXPECT("tap.forced.digital", SIO_PAD_DIGITAL, sio_get_pad_analog(0));

    sio_set_multitap_analog(1);
    sio_request_pad_type(0, SIO_PAD_NEGCON);
    /* Slot A poll with TAP=1 applies the request and arms the bulk read. */
    (void)xchg(0, 0x01);
    EXPECT("tap.hack.slot_a.id", 0x23, xchg(0, 0x42));
    EXPECT("tap.hack.negcon", SIO_PAD_NEGCON, sio_get_pad_analog(0));
    (void)xchg(0, 0x01);
    for (int i = 0; i < 6; i++) (void)xchg(0, 0x00);
    (void)xchg(0, 0x01);
    r[0] = xchg(0, 0x42);
    for (int i = 1; i < 34; i++) r[i] = xchg(0, 0x00);
    EXPECT("tap.bulk.id", 0x80, r[0]);
    EXPECT("tap.bulk.a.id", 0x23, r[2]);
    EXPECT("tap.bulk.a.twist", 0x44, r[6]);
    EXPECT("tap.bulk.a.I", 0x10, r[7]);
    EXPECT("tap.bulk.a.II", 0x20, r[8]);
    EXPECT("tap.bulk.a.L", 0x30, r[9]);
    sio_set_multitap(0);
}
#endif

int main(void) {
    negcon_wire();
    lock_and_swap();
    deferred_during_config();
    set_mode_keeps_device();
    snapshot_pad_mode();
#if PSX_MAX_PLAYERS >= 5
    multitap();
#endif
    if (failures) {
        fprintf(stderr, "NeGcon SIO protocol (players=%d): %d failure(s)\n",
                PSX_MAX_PLAYERS, failures);
        return 1;
    }
    fprintf(stderr, "NeGcon SIO protocol (players=%d): passed\n", PSX_MAX_PLAYERS);
    return 0;
}
