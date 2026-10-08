/* Shared PsxNetPad -> SIO apply (offline, netplay, selfcheck) and pad
 * normalization, checked on the real SIO wire. */

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

#include "psx_netplay.h"

static int failures;
#define EXPECT(label, expected, actual) do {                                  \
    unsigned e_ = (unsigned)(expected), a_ = (unsigned)(actual);              \
    if (e_ != a_) {                                                           \
        fprintf(stderr, "FAIL %s: expected=0x%X actual=0x%X\n",             \
                label, e_, a_);                                               \
        failures++;                                                           \
    }                                                                         \
} while (0)

static uint8_t xchg(uint8_t tx) {
    const uint16_t ctrl = (uint16_t)(CTRL_TX_EN | CTRL_SELECT | CTRL_ACK_IRQ_EN);
    sio_write(SIO_CTRL, ctrl);
    sio_write(SIO_TX_DATA, tx);
    sio_tick(2000);
    const uint8_t rx = (uint8_t)sio_read(SIO_RX_DATA);
    sio_write(SIO_CTRL, ctrl | CTRL_ACK);
    return rx;
}

/* Poll slot 0 twice (the first applies the deferred type) and capture the
 * second poll's nine response bytes. */
static void poll_bytes(uint8_t out[9]) {
    for (int pass = 0; pass < 2; pass++) {
        out[0] = xchg(0x01);
        out[1] = xchg(0x42);
        for (int i = 2; i < 9; i++) out[i] = xchg(0x00);
    }
}

static void fresh(void) {
    sio_init();
    sio_connect_pad(0);
    sio_set_pad_type(0, SIO_PAD_DIGITAL, 0x80, 0x80, 0x80, 0x80);
}

int main(void) {
    uint8_t a[9], b[9];

    /* A DualShock pad through the shared apply produces exactly the bytes of
     * the pre-NeGcon path (state + sticks + request). */
    PsxNetPad ds = { 0xFFBEu, 0x10, 0x20, 0x30, 0x40, SIO_PAD_DUALSHOCK, 1 };
    fresh();
    sio_set_pad_state_slot(0, ds.buttons);
    sio_set_pad_sticks(0, ds.lx, ds.ly, ds.rx, ds.ry);
    sio_request_pad_type(0, ds.analog);
    poll_bytes(a);
    fresh();
    psx_pad_apply_to_sio(0, &ds);
    poll_bytes(b);
    for (int i = 0; i < 9; i++) EXPECT("dualshock.identical", a[i], b[i]);
    EXPECT("dualshock.rx", 0x30, b[5]);

    /* NeGcon unpacks lx=twist, rx=I, ry=II, ly=L into the 0x23 frame. */
    PsxNetPad neg = { 0xBFFFu, 0x44, 0x07, 0xC0, 0x60, SIO_PAD_NEGCON, 1 };
    fresh();
    psx_pad_apply_to_sio(0, &neg);
    poll_bytes(b);
    EXPECT("negcon.id", 0x23, b[1]);
    EXPECT("negcon.buttons.high", 0xBF, b[4]);
    EXPECT("negcon.twist", 0x44, b[5]);
    EXPECT("negcon.I", 0xC0, b[6]);
    EXPECT("negcon.II", 0x60, b[7]);
    EXPECT("negcon.L", 0x07, b[8]);

    /* Back to DualShock: pressures are cleared, sticks are the pad's. */
    psx_pad_apply_to_sio(0, &ds);
    poll_bytes(b);
    EXPECT("back.id", 0x73, b[1]);
    {
        uint8_t p[3];
        sio_get_pad_negcon(0, p);
        EXPECT("back.pressure.cleared", 0, p[0] | p[1] | p[2]);
    }

    /* An out-of-range type applies as digital. */
    PsxNetPad bad = ds;
    bad.analog = 9;
    psx_pad_apply_to_sio(0, &bad);
    poll_bytes(b);
    EXPECT("bad.type.digital", 0x41, b[1]);

    /* Normalization: NeGcon pressures are exact, twist is deadzoned; the
     * DualShock deadzone is unchanged; unknown types become centred digital. */
    {
        PsxNetPad n = { 0xFFFFu, 0x90, 0x70, 0x88, 0x7F, SIO_PAD_NEGCON, 0 };
        psx_netplay_normalize_pad(&n);
        EXPECT("norm.negcon.twist", 0x80, n.lx);
        EXPECT("norm.negcon.L", 0x70, n.ly);
        EXPECT("norm.negcon.I", 0x88, n.rx);
        EXPECT("norm.negcon.II", 0x7F, n.ry);
        EXPECT("norm.connected", 1, n.connected);
        PsxNetPad d = { 0xFFFFu, 0x90, 0x70, 0x88, 0x10, SIO_PAD_DUALSHOCK, 1 };
        psx_netplay_normalize_pad(&d);
        EXPECT("norm.ds.ly", 0x80, d.ly);
        EXPECT("norm.ds.ry", 0x10, d.ry);
        PsxNetPad u = { 0xFFFFu, 0x00, 0x00, 0x00, 0x00, 7, 1 };
        psx_netplay_normalize_pad(&u);
        EXPECT("norm.unknown.type", 0, u.analog);
        EXPECT("norm.unknown.centred", 0x80, u.lx);
    }

    if (failures) {
        fprintf(stderr, "psx_pad_apply: %d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "psx_pad_apply: passed\n");
    return 0;
}
