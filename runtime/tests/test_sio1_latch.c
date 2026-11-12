/* SIO1 (serial port, 0x1F801050..5F) register latches.
 * A serial driver programs MODE, CTRL and BAUD and reads them back. libsio's
 * _sio_control(0,3,0) returns 2073600 / SIO1_BAUD; with the register reading 0
 * the compiler's divide check raises BREAK 7 (WipEout, PC 0x80077228).
 * The controller port (SIO0) keeps its own registers. */
#include "../src/sio.c"
#include <stdio.h>
#include <stdlib.h>
uint32_t i_stat, i_mask, g_debug_current_func_addr, g_debug_last_store_pc;
int g_psx_render_pass_active;
uint64_t psx_get_cycle_count(void) { return 0; }
int psx_get_in_exception(void) { return 0; }
uint8_t psx_read_byte(uint32_t a) { (void)a;return 0; }
uint32_t psx_read_word(uint32_t a) { (void)a;return 0; }
void psx_write_word(uint32_t a,uint32_t v) { (void)a;(void)v; }
uint32_t memory_get_sr(void) { return 0; }
void psx_irq_raise(uint32_t bit,uint32_t detail) { (void)detail;i_stat|=1u<<bit; }
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
int memcard_last_flush_result(int slot) { (void)slot; return 0; }
static int failures;
static void check(int ok, const char *what) { if (!ok) { printf("FAIL: %s\n", what); failures++; } }
int main(void) {
    sio_init();
    sio_write(0x1F80105E, 0x00D8);
    check(sio_read(0x1F80105E) == 0x00D8, "SIO1_BAUD reads back the written reload value");
    check(sio_read(0x1F80105E) != 0 && 2073600u / sio_read(0x1F80105E) == 9600u,
          "the libsio baud query divides by a non-zero reload value");
    sio_write(0x1F801058, 0x004E);
    check(sio_read(0x1F801058) == 0x004E, "SIO1_MODE reads back");
    sio_write(0x1F80105A, 0x0825);
    check(sio_read(0x1F80105A) == 0x0825, "SIO1_CTRL reads back");
    sio_write(0x1F80105A, 0x0825 | 0x0010 | 0x0040);
    check(sio_read(0x1F80105A) == 0x0825, "SIO1_CTRL ACK and RESET are strobes, not stored");
    check(sio_read(0x1F80104E) == 0 && sio_read(0x1F801048) == 0 && sio_read(0x1F80104A) == 0,
          "SIO1 writes leave the controller port registers alone");
    sio_write(0x1F80104E, 0x0088);
    check(sio_read(0x1F80104E) == 0x0088 && sio_read(0x1F80105E) == 0x00D8,
          "the two ports keep separate BAUD registers");
    sio_write(0x1F80104A, 0x0040);
    check(sio_read(0x1F80105E) == 0x00D8, "a controller port reset leaves SIO1_BAUD alone");
    sio_init();
    check(sio_read(0x1F80105E) == 0 && sio_read(0x1F801058) == 0 && sio_read(0x1F80105A) == 0,
          "sio_init clears the SIO1 latches");
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
