/* DualShock protocol rules on the production SIO device: a config command
 * outside config mode answers the ID byte and then gives no /ACK (P2), and
 * IRQ7 is raised only on the 0->1 edge of JOY_STAT bit 9 (P3). The expected
 * bytes and flags come from authored test programs run on a reference
 * emulator. */
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

typedef struct { uint8_t tx, rx; int ack; } PByte;
typedef struct { int n; PByte b[9]; } PTxn;
typedef struct { uint8_t tx, rx; int js_b, is_b, js_c, is_c; } P3Byte;
typedef struct { const char *name; int policy; P3Byte b[9]; } P3Case;

/* P2: config commands outside config mode. Per step, each byte's tx, rx
 * and whether /ACK followed. */
static const PTxn p2_outside[] = {
    {5,{{0x01,0x00,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {2,{{0x01,0xFF,1},{0x45,0x41,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {2,{{0x01,0xFF,1},{0x46,0x41,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {2,{{0x01,0xFF,1},{0x46,0x41,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {2,{{0x01,0xFF,1},{0x47,0x41,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {2,{{0x01,0xFF,1},{0x4C,0x41,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {2,{{0x01,0xFF,1},{0x4C,0x41,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {2,{{0x01,0xFF,1},{0x4D,0x41,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {2,{{0x01,0xFF,1},{0x44,0x41,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {5,{{0x01,0xFF,1},{0x43,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
    {5,{{0x01,0xFF,1},{0x42,0x41,1},{0x00,0x5A,1},{0x00,0xFF,1},{0x00,0xFF,0}}},
};
/* P3: tx, rx, then JOY_STAT
 * bit 9 and I_STAT bit 7 at point B (after /ACK time) and point C (after the
 * acknowledge policy: 1 = I_STAT, 2 = JOY_CTRL bit 4, 3 = both). */
static const P3Case p3_cases[] = {
    {"P3-digital-ack-both",3,{{0x01,0x00,1,1,0,0},{0x42,0x41,1,1,0,0},{0x00,0x5A,1,1,0,0},{0x00,0xFF,1,1,0,0},{0x00,0xFF,0,0,0,0},{0x00,0xFF,0,0,0,0},{0x00,0xFF,0,0,0,0},{0x00,0xFF,0,0,0,0},{0x00,0xFF,0,0,0,0}}},
    {"P3-digital-ack-istat-only",1,{{0x01,0x00,1,1,1,0},{0x42,0x41,1,0,1,0},{0x00,0x5A,1,0,1,0},{0x00,0xFF,1,0,1,0},{0x00,0xFF,1,0,1,0},{0x00,0xFF,1,0,1,0},{0x00,0xFF,1,0,1,0},{0x00,0xFF,1,0,1,0},{0x00,0xFF,1,0,1,0}}},
    {"P3-digital-ack-ctrl-only",2,{{0x01,0x00,1,1,0,1},{0x42,0x41,1,1,0,1},{0x00,0x5A,1,1,0,1},{0x00,0xFF,1,1,0,1},{0x00,0xFF,0,1,0,1},{0x00,0xFF,0,1,0,1},{0x00,0xFF,0,1,0,1},{0x00,0xFF,0,1,0,1},{0x00,0xFF,0,1,0,1}}},
    {"P3-digital-ack-neither",0,{{0x01,0x00,1,1,1,1},{0x42,0x41,1,1,1,1},{0x00,0x5A,1,1,1,1},{0x00,0xFF,1,1,1,1},{0x00,0xFF,1,1,1,1},{0x00,0xFF,1,1,1,1},{0x00,0xFF,1,1,1,1},{0x00,0xFF,1,1,1,1},{0x00,0xFF,1,1,1,1}}},
};

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void advance(unsigned n) { while (n--) { clock_now++; sio_advance(1); } }

static void power_on(void) {
    sio_init();
    sio_set_pad_connected(0, 1);
    sio_set_pad_config_capable(0, 1);
    sio_set_pad_analog(0, 0, 128, 128, 128, 128);
    sio_set_pad_state_slot(0, 0xFFFF);
    sio_write(0x1F801048, 0xD);
    sio_write(0x1F80104E, 0x88);
    i_stat = 0;
}

/* One byte: shift, then point B after ~1000 cycles. Returns rx. */
static unsigned byte_b(uint8_t tx, int *js_b, int *is_b) {
    sio_write(0x1F801040, tx);
    advance(1088);
    unsigned rx = sio_read(0x1F801040) & 0xFF;
    advance(1000);
    *js_b = (sio_read(0x1F801044) >> 9) & 1;
    *is_b = (i_stat >> 7) & 1;
    return rx;
}

/* The reference answers 00 for the very first address byte after power-on;
 * the runtime answers FF there, which this test leaves as it is. */
static uint8_t first_rx(void) { return 0xFF; }

static void run_p2_outside(void) {
    power_on();
    for (size_t s = 0; s < sizeof p2_outside / sizeof p2_outside[0]; ++s) {
        const PTxn *t = &p2_outside[s];
        sio_write(0x1F80104A, 0);
        sio_write(0x1F80104A, 0x1003);
        for (int k = 0; k < t->n; ++k) {
            int js, is;
            unsigned rx = byte_b(t->b[k].tx, &js, &is);
            uint8_t want = (s == 0 && k == 0) ? first_rx() : t->b[k].rx;
            CHECK(rx == want, "P2 step %zu byte %d tx %02X: rx %02X != %02X", s, k, t->b[k].tx, rx, want);
            CHECK(is == t->b[k].ack, "P2 step %zu byte %d tx %02X: ack %d != %d", s, k, t->b[k].tx, is, t->b[k].ack);
            sio_write(0x1F80104A, 0x1013);
            i_stat &= ~0x80u;
        }
        sio_write(0x1F80104A, 0);
    }
}

static void run_p3(void) {
    for (size_t c = 0; c < sizeof p3_cases / sizeof p3_cases[0]; ++c) {
        const P3Case *k = &p3_cases[c];
        power_on();
        sio_write(0x1F80104A, 0);
        sio_write(0x1F80104A, 0x1003);
        unsigned raises0 = irq7_raises;
        for (int j = 0; j < 9; ++j) {
            const P3Byte *e = &k->b[j];
            int js_b, is_b;
            unsigned rx = byte_b(e->tx, &js_b, &is_b);
            uint8_t want = j == 0 ? first_rx() : e->rx;
            CHECK(rx == want, "%s byte %d: rx %02X != %02X", k->name, j, rx, want);
            CHECK(js_b == e->js_b, "%s byte %d: JOY_STAT.9 at B %d != %d", k->name, j, js_b, e->js_b);
            CHECK(is_b == e->is_b, "%s byte %d: I_STAT.7 at B %d != %d", k->name, j, is_b, e->is_b);
            if (k->policy & 1) i_stat &= ~0x80u;
            if (k->policy & 2) sio_write(0x1F80104A, 0x1013);
            int js_c = (sio_read(0x1F801044) >> 9) & 1, is_c = (i_stat >> 7) & 1;
            CHECK(js_c == e->js_c, "%s byte %d: JOY_STAT.9 at C %d != %d", k->name, j, js_c, e->js_c);
            CHECK(is_c == e->is_c, "%s byte %d: I_STAT.7 at C %d != %d", k->name, j, is_c, e->is_c);
        }
        /* IRQ7 edges: every /ACK when JOY_CTRL acknowledges, else only the first. */
        unsigned want_raises = (k->policy & 2) ? 4u : 1u;
        CHECK(irq7_raises - raises0 == want_raises, "%s: %u IRQ7 raises != %u", k->name, irq7_raises - raises0, want_raises);
        sio_write(0x1F80104A, 0);
    }
}

int main(void) {
    run_p2_outside();
    run_p3();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: DualShock config commands outside config mode and the IRQ7 edge");
    return 0;
}

/* The fixture card flush succeeds. */
int memcard_last_flush_result(int slot) { (void)slot; return 0; }
