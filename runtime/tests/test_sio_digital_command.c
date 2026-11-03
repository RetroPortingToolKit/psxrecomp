/* Plain digital pads shift their ID during every command byte.
 * Synthetic bus transactions over the real sio.c; no BIOS/disc/core fixture. */
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
static void setup(unsigned slot) {
    sio_set_multitap(0);
    sio_init();
    sio_connect_pad(slot);
    sio_set_pad_analog(slot, SIO_PAD_DIGITAL, 0x80, 0x80, 0x80, 0x80);
    sio_set_pad_config_capable(slot, 0);
    sio_set_pad_state_slot(slot, 0xABCD);
}
int main(void) {
    for (unsigned slot = 0; slot < 2; ++slot) {
        for (unsigned cmd = 0; cmd < 256; ++cmd) {
            setup(slot);
            expect("address", 0xFF, transfer(slot, 0x01));
            expect("address ACK", 1, irq);
            expect("command ID", 0x41, transfer(slot, (uint8_t)cmd));
            expect("command ACK", cmd == 0x42, irq);
            if (cmd == 0x42) {
                expect("poll marker", 0x5A, transfer(slot, 0));
                expect("marker ACK", 1, irq);
                expect("buttons low", 0xCD, transfer(slot, 0));
                expect("low ACK", 1, irq);
                expect("buttons high", 0xAB, transfer(slot, 0));
                expect("last byte has no ACK", 0, irq);
            }
        }
        /* Config-capable digital and analog devices keep their own replies. */
        for (unsigned type = SIO_PAD_DIGITAL; type <= SIO_PAD_JOGCON; ++type) {
            sio_init(); sio_connect_pad(slot);
            sio_set_pad_type(slot, type, 0x80, 0x80, 0x80, 0x80);
            sio_set_pad_config_capable(slot, 1);
            expect("capable address", 0xFF, transfer(slot, 0x01));
            expect("capable unknown command", 0xFF, transfer(slot, 0x99));
            expect("capable unknown ACK", 0, irq);
        }
#if PSX_MAX_PLAYERS >= 5
        /* The tap's own non-poll-command behavior is deliberately unchanged. */
        sio_init(); sio_set_multitap(1); sio_set_multitap_port(slot);
#if PSX_MAX_PLAYERS >= 8
        const unsigned logical = slot ? 4u : 0u;
#else
        const unsigned logical = slot ? 1u : 0u;
#endif
        sio_connect_pad(logical);
        sio_set_pad_type(logical, SIO_PAD_DIGITAL, 0x80, 0x80, 0x80, 0x80);
        sio_set_pad_config_capable(logical, 0);
        sio_set_pad_state_slot(logical, 0xABCD);
        expect("tap poll address", 0xFF, transfer(slot, 0x01));
        expect("tap poll ID", 0x41, transfer(slot, 0x42));
        expect("tap poll ID ACK", 1, irq);
        expect("tap poll marker", 0x5A, transfer(slot, 0));
        expect("tap poll low", 0xCD, transfer(slot, 0));
        expect("tap poll high", 0xAB, transfer(slot, 0));
        expect("tap poll final ACK", 0, irq);
        sio_write(CTRL, 0); /* New DTR session for the rejected command. */
        expect("tap address", 0xFF, transfer(slot, 0x01));
        expect("tap unknown command", 0xFF, transfer(slot, 0x99));
        expect("tap unknown ACK", 0, irq);
#endif
        /* sio_init preserves multitap preferences; return to standalone. */
        sio_set_multitap(0);
        /* A disconnected port still floats; do not manufacture an ID. */
        sio_init();
        expect("empty address", 0xFF, transfer(slot, 0x01));
        expect("empty command", 0xFF, transfer(slot, 0x43));
        expect("empty ACK", 0, irq);
    }
    printf("digital command: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
