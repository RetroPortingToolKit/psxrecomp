/* Device-level regression: guest RAM is not SIO hardware state.
 * Uses production SIO with a synthetic clock/card, no BIOS or retail data.
 * Independent references: Beetle frontio.cpp DoDSRIRQ/Write and irq.cpp
 * IRQ_Assert/IRQ_Write: SIO cannot modify the INTC mask, and deasserting
 * SELECT cancels the device's pending DSR pulse.
 */
#include "../src/sio.c"
#include <stdio.h>

uint32_t i_stat, i_mask, g_debug_current_func_addr, g_debug_last_store_pc;
static uint64_t clock_now;
static int card_present;
static int checks, failures;

uint64_t psx_get_cycle_count(void) { return clock_now; }
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t a) { (void)a; return 0; }
uint32_t psx_read_word(uint32_t a) {
    return (a == 0x800A6C10u || a == 0x800B4E30u) ? 1u : 0u;
}
uint32_t memory_get_sr(void) { return 0; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= 1u << bit; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void event_ring_record_aux(uint16_t a, uint8_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void starvation_ring_record(uint8_t a, uint8_t b, uint8_t c, uint16_t d, uint16_t e,
    int f, int g, int h, int i, int j, uint8_t k, uint32_t l,
    uint8_t m, uint8_t n, uint8_t o, uint8_t p, int q) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    (void)h; (void)i; (void)j; (void)k; (void)l;
    (void)m; (void)n; (void)o; (void)p; (void)q;
}
void card_read_summary_record(uint8_t a, uint8_t b, uint16_t c, uint8_t d,
    uint8_t e, const uint8_t *f) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}
void card_data_writes_arm(uint8_t a, uint16_t b, uint8_t c, uint8_t d) {
    (void)a; (void)b; (void)c; (void)d;
}
int memcard_is_present(int slot) { return card_present && slot == 0; }
int memcard_read_sector(int slot, int sector, uint8_t *buf) {
    (void)slot; (void)sector; memset(buf, 0, 128); return 0;
}
int memcard_write_sector(int slot, int sector, const uint8_t *buf) {
    (void)slot; (void)sector; (void)buf; return 0;
}
void memcard_flush(int slot) { (void)slot; }

static void check(const char *name, int ok) {
    checks++; if (!ok) failures++;
    printf("%s %s (I_MASK=%03X I_STAT=%03X)\n", ok ? "PASS" : "FAIL", name, i_mask, i_stat);
}
static void advance(unsigned cycles) { clock_now += cycles; sio_advance((int)cycles); }
int main(int argc, char **argv) {
    const char *scenario = argc > 1 ? argv[1] : "absent";
    sio_init(); clock_now = 0; i_mask = 0x0d; i_stat = 0x201;
    card_present = strcmp(scenario, "absent") != 0;
    sio_write(0x1F801048, 0x0d);
    sio_write(0x1F80104e, 0x88);
    sio_write(0x1F80104a, card_present ? 0x1003 : 3);
    sio_write(0x1F801040, 0x81);
    advance(1088);
    if (!card_present) {
        sio_write(0x1F80104a, 0x40);
        check("absent-card reset cannot enable interrupts", i_mask == 0x0d);
        check("absent-card reset cannot inject IRQ7", i_stat == 0x201);
    } else if (!strcmp(scenario, "deselect")) {
        check("ACK has not arrived before SELECT drop", !(i_stat & 0x80));
        sio_write(0x1F80104a, 0x1001);
        check("SELECT drop cancels pending ACK", !(i_stat & 0x80));
        advance(1000);
        check("cancelled ACK cannot fire later", !(i_stat & 0x80));
    } else if (!strcmp(scenario, "pending")) {
        i_stat |= 0x80;
        advance(170);
        check("ACK cannot queue on INTC pending bit", !sio_pending_ack);
        check("ACK sets device IRQ latch even with INTC pending", (sio_peek_stat() & 0x200) != 0);
    } else if (!strcmp(scenario, "coarse")) {
        /* Restart with a single step spanning shift completion and ACK. */
        sio_write(0x1F80104a, 0x40);
        sio_write(0x1F801048, 0x0d);
        sio_write(0x1F80104e, 0x88);
        sio_write(0x1F80104a, 0x1003);
        sio_write(0x1F801040, 0x81);
        advance(1088 + 170);
        check("coarse advance consumes shift and ACK deadlines", (i_stat & 0x80) != 0);
        check("coarse advance leaves no overdue ACK", !sio_pending_ack);
    } else {
        return 2;
    }
    check("SIO preserves unrelated SPU pending interrupt", (i_stat & 0x200) != 0);
    printf("%d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
