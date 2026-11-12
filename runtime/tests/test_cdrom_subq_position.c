/* GetlocP answers the eight position bytes of the recorded Q, without its reserved byte. Synthetic sectors only; no BIOS/game data.
 * Include the production unit to seed the laser position without reading an
 * entire synthetic disc. All commands and advancement use its real MMIO API.
 */
#include "../src/cdrom.c"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
static int reads, failures, irq_raises;
static int supplied_valid=1;
static const uint8_t raw_q[12]={0x41,0x01,0x01,0x23,0x06,0x05,0x00,0x03,0x08,0x01,0,0};
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
int iso_read_raw_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; (void)lba; (void)b; (void)n; return 0;
}
int iso_read_subq(void *p, uint32_t lba, uint8_t *b, int n, int *v) {
    (void)p; (void)lba; if(n!=12)abort();memcpy(b,raw_q,12);*v=supplied_valid;return 1;
}
int iso_has_subq_replacements(void *p) { (void)p; return 1; }
uint32_t iso_sector_count(void *p) { (void)p; return 333000; }
int iso_track_count(void *p) { (void)p; return 1; }
uint32_t iso_track_start_lba(void *p, int t) { (void)p; (void)t; return 0; }
uint32_t iso_track_pregap_lba(void *p, int t) { (void)p; (void)t; return 0; }
int iso_track_is_audio(void *p, int t) { (void)p; (void)t; return 0; }


int main(void) {
    const uint8_t expected[8]={1,1,0x23,6,5,3,8,1};
    cdrom_init("authored");
    for(int pass=0;pass<2;pass++) {
        /* Invalid SBI Q leaves the previous valid twelve-byte Q intact. */
        supplied_valid=!pass;
        cdrom_write(0x1f801800,0);cdrom_write(0x1f801801,0x11);
        if(irq_flag!=CDIRQ_ACK || response_count-response_read!=8) {
            fprintf(stderr,"FAIL pass %d: GetlocP is not an eight-byte INT3\n",pass);return 1;
        }
        for(int i=0;i<8;i++) {
            uint8_t got=cdrom_read(0x1f801801);
            if(got!=expected[i]) {
                fprintf(stderr,"FAIL pass %d byte %d: %02X != %02X\n",pass,i,got,expected[i]);return 2;
            }
        }
        cdrom_write(0x1f801800,1);cdrom_write(0x1f801803,0x1f);
    }
    puts("GetlocP: eight position bytes omit reserved Q byte; invalid Q preserves last valid position");
    return 0;
}

/* The CD-DA view of the synthetic disc is the same authored backing image. */
int iso_cdda_track_count(void *h) {return iso_track_count(h);}
uint32_t iso_cdda_track_start_lba(void *h,int t) {return iso_track_start_lba(h,t);}
uint32_t iso_cdda_track_pregap_lba(void *h,int t) {return iso_track_pregap_lba(h,t);}
int iso_cdda_track_is_audio(void *h,int t) {return iso_track_is_audio(h,t);}
uint32_t iso_cdda_sector_count(void *h) {return iso_sector_count(h);}
int iso_read_cdda_sector(void *h,uint32_t lba,uint8_t *p,int n) {return iso_read_raw_sector(h,lba,p,n);}
int iso_read_cdda_subq(void *h,uint32_t lba,uint8_t *p,int n,int *valid) {return iso_read_subq(h,lba,p,n,valid);}
void savestate_set_disc_scope(int disc) {(void)disc;}
