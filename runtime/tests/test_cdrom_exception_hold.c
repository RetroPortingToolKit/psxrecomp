/* A CD response is not presented to the interrupt controller while the CPU is
 * inside its exception handler. Synthetic sectors only; no BIOS or game data.
 *
 * Nightmare Creatures (USA) keeps its own CD handler installed and opens a
 * file through the BIOS. The BIOS driver issues Pause from its data-ready
 * handler; the response became visible 5000 cycles later, in the middle of the
 * vertical-blank exception, after the kernel had already passed the BIOS
 * driver's chain entry. The game's handler acknowledged it, the BIOS driver
 * stayed busy, CdAsyncGetStatus failed nine times and the BIOS took its
 * disk-error halt with the display still off. */
#include "../src/cdrom.c"

uint64_t psx_cycle_count, s_frame_count;
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t debug_guest_ra(void) { return 0; }
static int failures, irq_raises, in_handler;
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
    (void)p; memset(b, (int)(lba & 255u), (size_t)n); return 1;
}
int iso_read_raw_sector(void *p, uint32_t lba, uint8_t *b, int n) {
    (void)p; (void)lba; (void)b; (void)n; return 0;
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

#define CHECK(c, m) do { if (!(c)) { fprintf(stderr,"FAIL: %s\n",m); failures++; } } while(0)
static int handler_active(void) { return in_handler; }
static void command(uint8_t c) { cdrom_write(0x1f801800,0); cdrom_write(0x1f801801,c); }
static void advance(int n) { psx_cycle_count += (uint32_t)n; cdrom_advance((uint32_t)n); }
static void setup(void) {
    psx_cycle_count=1000; cdrom_init("synthetic"); mode_reg=0x80;
    i_stat=0; irq_raises=0; in_handler=0;
}
int main(void) {
    const uint32_t cd=1u<<2;
    /* The Nightmare Creatures order: the response comes due while a handler
     * runs, and is presented only after that handler has returned. */
    cdrom_set_exception_probe(handler_active);
    setup(); command(1);
    CHECK(irq_flag==CDIRQ_ACK,"GetStat answers with INT3");
    advance(CDROM_IRQ_PRESENT_DELAY-1000);
    in_handler=1;
    advance(1000);
    CHECK(i_stat==0,"a response that comes due inside a handler is not presented");
    CHECK(irq_flag==CDIRQ_ACK,"the controller flag stays readable while the response is held");
    advance(1200);
    CHECK(i_stat==0&&irq_raises==0,"the response stays held for the rest of the handler");
    in_handler=0;
    advance(1);
    CHECK((i_stat&cd)&&irq_raises==1,"the response is presented once the handler has returned");
    advance(5000);
    CHECK(irq_raises==1,"the held response is presented exactly once");

    /* Outside a handler the command latency is unchanged. */
    setup(); command(1);
    advance(CDROM_IRQ_PRESENT_DELAY-1);
    CHECK(i_stat==0,"no presentation before the command latency");
    advance(1);
    CHECK((i_stat&cd)&&irq_raises==1,"presentation at the command latency outside a handler");

    /* A handler that never returns does not starve the drive. */
    setup(); command(1); in_handler=1;
    advance(CDROM_IRQ_PRESENT_DELAY+(int)CDROM_IRQ_EXCEPTION_HOLD_MAX-1);
    CHECK(i_stat==0,"the hold lasts up to its bound");
    advance(1);
    CHECK((i_stat&cd)&&irq_raises==1,"the bound releases the response inside the handler");

    /* A second response (Pause complete) is held the same way. */
    setup(); command(9);
    advance(CDROM_IRQ_PRESENT_DELAY);
    CHECK(i_stat&cd,"Pause acknowledge presented");
    cdrom_write(0x1f801800,1); cdrom_write(0x1f801803,0x1f); cdrom_write(0x1f801800,0);
    i_stat=0; irq_raises=0; in_handler=1;
    advance(100000000);
    CHECK(irq_flag==CDIRQ_COMPLETE,"Pause completes");
    CHECK((i_stat&cd)&&irq_raises==1,"a bounded hold still presents the completion");
    setup(); command(9); advance(CDROM_IRQ_PRESENT_DELAY);
    cdrom_write(0x1f801800,1); cdrom_write(0x1f801803,0x1f); cdrom_write(0x1f801800,0);
    i_stat=0; irq_raises=0;
    while(irq_flag!=CDIRQ_COMPLETE && psx_cycle_count<200000000u) advance(64);
    in_handler=1; advance(CDROM_IRQ_PRESENT_DELAY+64);
    CHECK(irq_flag==CDIRQ_COMPLETE&&i_stat==0,"a completion that comes due inside a handler is held");
    in_handler=0; advance(1);
    CHECK((i_stat&cd)&&irq_raises==1,"the completion is presented after the handler");

    /* Without a probe the drive presents at the command latency as before. */
    cdrom_set_exception_probe(NULL);
    setup(); command(1); in_handler=1;
    advance(CDROM_IRQ_PRESENT_DELAY);
    CHECK((i_stat&cd)&&irq_raises==1,"no probe, no hold");

    if(failures) { fprintf(stderr,"FAILED (%d)\n",failures); return 1; }
    puts("CD exception hold: ALL PASS"); return 0;
}

/* Synthetic disc: the CDDA view reads the same sectors as the data view. */
int iso_cdda_track_count(void *h) { return iso_track_count(h); }
uint32_t iso_cdda_track_start_lba(void *h, int t) { return iso_track_start_lba(h, t); }
uint32_t iso_cdda_track_pregap_lba(void *h, int t) { return iso_track_pregap_lba(h, t); }
int iso_cdda_track_is_audio(void *h, int t) { return iso_track_is_audio(h, t); }
uint32_t iso_cdda_sector_count(void *h) { return iso_sector_count(h); }
int iso_read_cdda_sector(void *h, uint32_t lba, uint8_t *p, int n) { return iso_read_raw_sector(h, lba, p, n); }
int iso_read_cdda_subq(void *h, uint32_t lba, uint8_t *p, int n, int *valid) { return iso_read_subq(h, lba, p, n, valid); }
void savestate_set_disc_scope(int disc) { (void)disc; }
