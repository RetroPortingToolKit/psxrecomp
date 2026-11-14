/* Host port swap (sio_set_pads_swapped) over the real sio.c: the first pad
 * answers on console Port 2, in one-, two- and five-player builds. Synthetic
 * bus transactions; no BIOS/disc/core fixture. */
#include "sio.h"
#include "digital_pad_snapshot.h"
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


#define DATA 0x1F801040u
#define STAT 0x1F801044u
#define CTRL 0x1F80104Au
#define SELECT 2u
#define TXEN 1u
#define ACK 16u
#define IRQEN 0x1000u
#define SLOT 0x2000u
static unsigned failures, checks;
static unsigned irq;
static void expect(const char *what, unsigned expected, unsigned got) {
    ++checks;
    if (expected != got) {
        fprintf(stderr, "%s: expected %X, got %X\n", what, expected, got);
        ++failures;
    }
}
static uint8_t transfer(unsigned slot, uint8_t tx) {
    const uint16_t ctrl = TXEN | SELECT | IRQEN | (slot ? SLOT : 0);
    i_stat = 0;
    sio_write(CTRL, ctrl);
    sio_write(DATA, tx);
    sio_tick(2000);
    const uint8_t rx = (uint8_t)sio_read(DATA);
    irq = !!(i_stat & (1u << 7));
    sio_write(CTRL, ctrl | ACK);
    return rx;
}

/* Poll one console port: the two button bytes, or -1 when nothing answers. */
static int poll_port(unsigned slot) {
    sio_write(CTRL, 0);                   /* new DTR session */
    if (transfer(slot, 0x01) != 0xFF || !irq) return -1;
    if (transfer(slot, 0x42) != 0x41 || !irq) return -1;
    if (transfer(slot, 0) != 0x5A) return -1;
    const unsigned lo = transfer(slot, 0);
    const unsigned hi = transfer(slot, 0);
    return (int)(lo | hi << 8);
}

static void pad(unsigned logical, uint16_t buttons) {
    sio_connect_pad((int)logical);
    sio_set_pad_analog((int)logical, SIO_PAD_DIGITAL, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_config_capable((int)logical, 0);
    sio_set_pad_state_slot((int)logical, buttons);
}

int main(void) {
    sio_set_multitap(0);
    sio_set_pads_swapped(0);
    sio_init();
    pad(0, 0xABCD);
#if PSX_MAX_PLAYERS >= 2
    pad(1, 0x1234);
    const int port2_plain = 0x1234;
#else
    const int port2_plain = -1;           /* a one-player build has no pad 1 */
#endif
    expect("plain: port 1 is pad 0", 0xABCD, (unsigned)poll_port(0));
    expect("plain: port 2", (unsigned)port2_plain, (unsigned)poll_port(1));

    sio_set_pads_swapped(1);
    expect("swap reported", 1, (unsigned)sio_get_pads_swapped());
    expect("swapped: port 2 is pad 0", 0xABCD, (unsigned)poll_port(1));
    expect("swapped: port 1", (unsigned)port2_plain, (unsigned)poll_port(0));
    /* The pad keeps its own state: a new press shows on its new port. */
    sio_set_pad_state_slot(0, 0xFFBF);
    expect("swapped: live buttons on port 2", 0xFFBF, (unsigned)poll_port(1));
    /* An unplugged pad 0 leaves port 2 empty while swapped. */
    sio_set_pad_connected(0, 0);
    expect("swapped: unplugged pad 0", (unsigned)-1, (unsigned)poll_port(1));
    sio_set_pad_connected(0, 1);
    sio_set_pad_state_slot(0, 0xABCD);

    sio_set_pads_swapped(0);
    expect("back: port 1 is pad 0", 0xABCD, (unsigned)poll_port(0));
    expect("back: port 2", (unsigned)port2_plain, (unsigned)poll_port(1));

#if PSX_MAX_PLAYERS >= 5
    /* With a multitap the swap is ignored: the tap stays on its port. */
    sio_init(); sio_set_multitap(1); sio_set_multitap_port(0);
    pad(0, 0xABCD);
    sio_set_pads_swapped(1);
    expect("tap: port 1 still slot A", 0xABCD, (unsigned)poll_port(0));
    sio_set_pads_swapped(0);
    sio_set_multitap(0);
#endif

    /* A netplay session starts from the plain layout. */
    sio_set_pads_swapped(1);
    sio_netplay_canonicalize_session_pads(2);
    expect("netplay clears the swap", 0, (unsigned)sio_get_pads_swapped());

    printf("port swap (%d players): %u checks, %u failures\n", PSX_MAX_PLAYERS, checks, failures);
    return failures ? 1 : 0;
}

/* Standalone device test: not executing a render-only checkpoint. */
int g_psx_render_pass_active;

/* Match the current SIO flush-status query in this no-file fixture. */
int memcard_last_flush_result(int card) { (void)card;return 0; }
