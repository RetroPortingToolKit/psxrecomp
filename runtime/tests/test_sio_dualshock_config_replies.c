/* DualShock config-mode replies on the production SIO device. 0x45 answers
 * type 01, 02, the analog flag, 02 01 00; 0x46, 0x47 and 0x4C choose their
 * reply by the data byte sent after the 5A byte. The expected bytes are the
 * replies the DualShock protocol documents (psx-spx, "Controllers and Memory
 * Cards", config mode commands). */
#include "../src/sio.c"
#include <stdio.h>
#include <stdlib.h>
uint32_t i_stat, i_mask, g_debug_current_func_addr, g_debug_last_store_pc;
int g_psx_render_pass_active;
static uint64_t clock_now;
uint64_t psx_get_cycle_count(void) { return clock_now; }
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t a) { (void)a;return 0; }
uint32_t psx_read_word(uint32_t a) { (void)a;return 0; }
void psx_write_word(uint32_t a,uint32_t v) { (void)a;(void)v; }
uint32_t memory_get_sr(void) { return 0; }
static unsigned irq7_raises;
void psx_irq_raise(uint32_t bit,uint32_t detail) { (void)detail;i_stat|=1u<<bit;if(bit==7)irq7_raises++; }
void debug_server_poll(void) {}
void debug_server_log_sio_write(uint32_t a,uint32_t v,uint8_t w) { (void)a;(void)v;(void)w; }
uint16_t debug_server_update_poll(int slot,uint16_t buttons,int analog) { (void)slot;(void)analog;return buttons; }
void event_ring_record_aux(uint16_t a,uint8_t b,uint32_t c) { (void)a;(void)b;(void)c; }
void starvation_ring_record(uint8_t a,uint8_t b,uint8_t c,uint16_t d,uint16_t e,int f,int g,int h,int i,int j,uint8_t k,uint32_t l,uint8_t m,uint8_t n,uint8_t o,uint8_t p,int q) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i;(void)j;(void)k;(void)l;(void)m;(void)n;(void)o;(void)p;(void)q; }
void card_read_summary_record(uint8_t a,uint8_t b,uint16_t c,uint8_t d,uint8_t e,const uint8_t *f) { (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; }
void card_data_writes_arm(uint8_t a,uint16_t b,uint8_t c,uint8_t d) { (void)a;(void)b;(void)c;(void)d; }
int memcard_is_present(int slot) { (void)slot;return 0; }
int memcard_read_sector(int slot,int sector,uint8_t *buf) { (void)slot;(void)sector;memset(buf,0,128);return 0; }
int memcard_write_sector(int slot,int sector,const uint8_t *buf) { (void)slot;(void)sector;(void)buf;return 0; }
void memcard_flush(int slot) { (void)slot; }


static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void advance(unsigned n) { while (n--) { clock_now++; sio_advance(1); } }

static void power_on(int analog) {
    sio_init();
    sio_set_pad_connected(0, 1);
    sio_set_pad_config_capable(0, 1);
    sio_set_pad_analog(0, analog, 128, 128, 128, 128);
    sio_set_pad_state_slot(0, 0xFFFF);
    sio_write(0x1F801048, 0xD);
    sio_write(0x1F80104E, 0x88);
    i_stat = 0;
}

/* One selected transaction: send n bytes, keep each rx byte. */
static void txn(const uint8_t *tx, uint8_t *rx, int n) {
    sio_write(0x1F80104A, 0);
    sio_write(0x1F80104A, 0x1003);
    for (int k = 0; k < n; ++k) {
        sio_write(0x1F801040, tx[k]);
        advance(2088);
        rx[k] = (uint8_t)(sio_read(0x1F801040) & 0xFF);
        sio_write(0x1F80104A, 0x1013);
        i_stat &= ~0x80u;
    }
    sio_write(0x1F80104A, 0);
}

static void enter_config(int analog) {
    static const uint8_t tx[9] = { 0x01, 0x43, 0x00, 0x01, 0, 0, 0, 0, 0 };
    uint8_t rx[9];
    txn(tx, rx, analog ? 9 : 5);
}

typedef struct { const char *name; int analog; uint8_t cmd, data; uint8_t want[6]; } Case;
static const Case cases[] = {
    {"45 digital", 0, 0x45, 0x00, {0x01,0x02,0x00,0x02,0x01,0x00}},
    {"45 analog",  1, 0x45, 0x00, {0x01,0x02,0x01,0x02,0x01,0x00}},
    {"46 00",      0, 0x46, 0x00, {0x00,0x00,0x01,0x02,0x00,0x0A}},
    {"46 01",      0, 0x46, 0x01, {0x00,0x00,0x01,0x01,0x01,0x14}},
    {"46 02",      0, 0x46, 0x02, {0x00,0x00,0x00,0x00,0x00,0x00}},
    {"47 00",      0, 0x47, 0x00, {0x00,0x00,0x02,0x00,0x01,0x00}},
    {"47 01",      0, 0x47, 0x01, {0x00,0x00,0x00,0x00,0x00,0x00}},
    {"4C 00",      0, 0x4C, 0x00, {0x00,0x00,0x00,0x04,0x00,0x00}},
    {"4C 01",      0, 0x4C, 0x01, {0x00,0x00,0x00,0x07,0x00,0x00}},
    {"4C 02",      0, 0x4C, 0x02, {0x00,0x00,0x00,0x00,0x00,0x00}},
};

int main(void) {
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const Case *k = &cases[c];
        power_on(k->analog);
        enter_config(k->analog);
        const uint8_t tx[9] = { 0x01, k->cmd, 0x00, k->data, 0, 0, 0, 0, 0 };
        uint8_t rx[9];
        txn(tx, rx, 9);
        CHECK(rx[1] == 0xF3 && rx[2] == 0x5A, "%s: header %02X %02X != F3 5A", k->name, rx[1], rx[2]);
        for (int j = 0; j < 6; ++j)
            CHECK(rx[3 + j] == k->want[j], "%s byte %d: rx %02X != %02X", k->name, 3 + j, rx[3 + j], k->want[j]);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: DualShock 0x45, 0x46, 0x47 and 0x4C config replies");
    return 0;
}

/* The fixture card flush succeeds. */
int memcard_last_flush_result(int slot) { (void)slot; return 0; }
