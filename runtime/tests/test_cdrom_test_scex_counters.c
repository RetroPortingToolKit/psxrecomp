/* Test 19h sub-functions 04h/05h/20h: answer length and bytes. Synthetic disc only; no BIOS/game data.
 * Includes the production unit and drives it through its real MMIO API.
 */
#include "../src/cdrom.c"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
static int reads, failures, irq_raises;
void psx_irq_raise(uint32_t a, uint32_t b) { (void)b; i_stat |= 1u << a; irq_raises++; }
void event_ring_record(uint16_t a, uint8_t b) { (void)a; (void)b; }
void event_ring_record_aux(uint16_t a, uint8_t b, uint32_t c) { (void)a; (void)b; (void)c; }
void audio_trace_event(uint16_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
void spu_cd_audio_push(const int16_t *a, int b) { (void)a; (void)b; }
void spu_cd_audio_reset(void) {}
int psx_netplay_active(void) { return 0; }
int psx_netplay_cd_bisect_active(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
void *iso_open(const char *p) { (void)p; return (void *)1; }
void iso_close(void *p) { (void)p; }
int iso_read_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; memset(b, (int)(lba & 255u), (size_t)n); reads++; return 1;
}
/* Raw sectors are served only when raw_mode_byte >= 0: the header's mode byte
 * (offset 15) takes that value, so a test can author any mode, including 0xFF. */
static int raw_mode_byte = -1;
int iso_read_raw_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; (void)lba;
    if (raw_mode_byte < 0) return 0;
    memset(b, 0, (size_t)n); b[15] = (uint8_t)raw_mode_byte; return 1;
}
int iso_read_subq(void *p, uint32_t lba, uint8_t *b, int n, int *v) {
    (void)p; (void)lba; (void)b; (void)n; *v=0; return 0;
}
int iso_has_subq_replacements(void *p) { (void)p; return 0; }
uint32_t iso_sector_count(void *p) { (void)p; return 333000; }
int iso_track_count(void *p) { (void)p; return 1; }
uint32_t iso_track_start_lba(void *p, int t) { (void)p; (void)t; return 0; }
uint32_t iso_track_pregap_lba(void *p, int t) { (void)p; (void)t; return 0; }
int iso_track_is_audio(void *p, int t) { (void)p; (void)t; return 0; }
/* The CD-DA view of the synthetic disc is the same backing image. */
int iso_cdda_track_count(void *h) {return iso_track_count(h);}
uint32_t iso_cdda_track_start_lba(void *h,int t) {return iso_track_start_lba(h,t);}
uint32_t iso_cdda_track_pregap_lba(void *h,int t) {return iso_track_pregap_lba(h,t);}
int iso_cdda_track_is_audio(void *h,int t) {return iso_track_is_audio(h,t);}
uint32_t iso_cdda_sector_count(void *h) {return iso_sector_count(h);}
int iso_read_cdda_sector(void *h,uint32_t lba,uint8_t *p,int n) {return iso_read_raw_sector(h,lba,p,n);}
int iso_read_cdda_subq(void *h,uint32_t lba,uint8_t *p,int n,int *valid) {return iso_read_subq(h,lba,p,n,valid);}
void savestate_set_disc_scope(int disc) {(void)disc;}

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); failures++; } } while(0)
static void ack(void) {
    cdrom_write(0x1f801800,1); cdrom_write(0x1f801803,0x1f);
    cdrom_write(0x1f801800,0);
}
/* Send Test with one parameter and return the IRQ type it raised. */
static int test_cmd(uint8_t sub) {
    cdrom_write(0x1f801800,0); cdrom_write(0x1f801802,sub);
    cdrom_write(0x1f801801,0x19); return irq_flag;
}
static uint8_t byte_at(int i) { return response_fifo[(response_read+i)&15]; }

int main(void) {
    cdrom_init("synthetic");
    stat_reg |= 0x80; /* playing: a nonzero status byte */

    CHECK(test_cmd(0x05)==CDIRQ_ACK,"19h,05h answers INT3");
    CHECK(response_count==2,"19h,05h answers two bytes");
    CHECK(byte_at(0)==0x00 && byte_at(1)==0x00,"19h,05h counts are zero, not the status");
    ack();

    CHECK(test_cmd(0x04)==CDIRQ_ACK,"19h,04h answers INT3");
    CHECK(response_count==1 && byte_at(0)==stat_reg,"19h,04h answers the status");
    ack();

    CHECK(test_cmd(0x20)==CDIRQ_ACK && response_count==4,"19h,20h still answers four version bytes");
    ack();

    (void)reads; (void)irq_raises;
    if (failures) return 1;
    puts("PASS: cdrom test scex counters");
    return 0;
}
