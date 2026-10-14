/* Synthetic-sector integration tests for the actual CD controller. */
#include "../src/cdrom.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t s_frame_count, psx_cycle_count;
static int test_dma_active;
int dma_cdrom_transfer_active(void) { return test_dma_active; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= bit; }
int iso_read_raw_sector(void* h, uint32_t lba, uint8_t* out, int size) {
    (void)h; memset(out, 0, size); out[15]=2; out[18]=out[22]=8;
    memset(out+24, (unsigned char)lba, 2048); return 1;
}
int iso_read_sector(void* h,uint32_t lba,uint8_t* out,int size) {(void)h;memset(out,(unsigned char)lba,size);return 1;}

static void setup(void) {
    test_dma_active = 0;
    psx_cycle_count = 1000000;
    cdrom_init("synthetic");
    mode_reg = 0x80;
    reading = 1;
    read_min = 0; read_sec = 3; read_sect = 25; /* LBA 100 */
}
static void sector(void) {
    psx_cycle_count += 225792;
    read_delay = 0;
    process_read_stream(0);
}
static void ack(void) {
    cdrom_write(0x1f801800, 1);
    cdrom_write(0x1f801803, 7);
    cdrom_write(0x1f801800, 0);
}
static void request(void) {
    cdrom_write(0x1f801800, 0);
    cdrom_write(0x1f801803, 0);
    cdrom_write(0x1f801803, 0x80);
}
static void drain(unsigned lba, int words) {
    for (int n = 0; n < words; n++) {
        CHECK(cdrom_dma_ready());
        CHECK(cdrom_dma_read() == lba * 0x01010101u);
    }
}
static void delayed_notification(void) {
    setup(); sector(); sector(); /* 100 announced, 101 pending */
    CHECK(pending_dataready && last_sector_lba == 101);
    ack(); request();
    psx_cycle_count += CDROM_PEND_PRESENT_DELAY;
    cdrom_advance(0); /* 101 becomes visible between BFRD and DMA */
    CHECK(irq_flag == CDIRQ_DATA_READY && !pending_dataready);
    drain(100, 512);
    CHECK(!cdrom_dma_ready());
    ack(); request();
    drain(101, 512);
    CHECK(s_int1_lost == 0);
}
static void dma_continuation(void) {
    setup(); sector(); request(); drain(100, 512);
    test_dma_active = 1;
    sector(); drain(101, 512);
    sector(); drain(102, 512);
    CHECK(!pending_dataready);
    CHECK(s_int1_lost == 0);
    test_dma_active = 0;
}
static void dma_arrival_mid_drain(void) {
    setup(); sector(); request();
    test_dma_active = 1;
    drain(100, 17);
    sector(); /* Decoder advances while the old FIFO still has unread bytes. */
    drain(100, 495);
    drain(101, 512);
    CHECK(!cdrom_dma_ready());
    CHECK(!pending_dataready && s_int1_lost == 0);
    test_dma_active = 0;
}
static void request_rearms_retained_sector(void) {
    setup(); sector(); request(); drain(100, 512); ack();
    sector(); /* BFRD remains asserted and preloads 101. */
    request(); /* BIOS clears BFRD before requesting that same sector. */
    drain(101, 512);
    CHECK(!cdrom_dma_ready());
}
static void snapshot_pending_deadline(void) {
    setup(); sector(); request(); sector(); ack();
    psx_cycle_count += 123;
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size);
    CHECK(wire);
    cdrom_snapshot_write(wire);
    setup();
    psx_cycle_count = 0; /* Restore at a different cycle origin. */
    CHECK(cdrom_snapshot_read(wire, size));
    CHECK(pending_present_due == CDROM_PEND_PRESENT_DELAY - 123);
    psx_cycle_count = pending_present_due - 1;
    cdrom_advance(0);
    CHECK(irq_flag == 0 && pending_dataready);
    psx_cycle_count++;
    cdrom_advance(0);
    CHECK(irq_flag == CDIRQ_DATA_READY && !pending_dataready);
    drain(100, 512);
    request(); drain(101, 512);
    free(wire);
}
static void snapshot_between_request_and_dma(void) {
    setup(); sector(); request(); drain(100, 11); sector();
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size), *after = malloc(size);
    CHECK(wire && after);
    cdrom_snapshot_write(wire);
    CHECK(cdrom_snapshot_validate(wire, size));
    setup();
    CHECK(cdrom_snapshot_read(wire, size));
    ack();
    psx_cycle_count += CDROM_PEND_PRESENT_DELAY;
    cdrom_advance(0);
    drain(100, 501);
    ack(); request(); drain(101, 512);

    /* Malformed ring indices must be rejected before controller mutation. */
    cdrom_snapshot_write(after);
    const uint32_t legacy = cdrom_snapshot_legacy_bytes();
    wire[legacy + 4] = 0xff;
    CHECK(!cdrom_snapshot_validate(wire, size));
    CHECK(!cdrom_snapshot_read(wire, size));
    cdrom_snapshot_write(wire);
    CHECK(!memcmp(wire, after, size));

    CHECK(!cdrom_snapshot_validate(NULL, size));
    CHECK(!cdrom_snapshot_validate(after, size - 1));
    memcpy(wire, after, size);
    memset(wire + legacy + 16, 0x7f, 4); /* Impossible pending delay. */
    CHECK(!cdrom_snapshot_read(wire, size));
    cdrom_snapshot_write(wire);
    CHECK(!memcmp(wire, after, size));

    /* Old states retain their original wire prefix and readable payload. */
    setup(); sector(); request(); drain(100, 7);
    PstW writer;
    pst_w_init(&writer, wire, size);
    CHECK(cdrom_snap_emit(&writer, 0));
    CHECK(writer.written == legacy);
    setup();
    CHECK(cdrom_snapshot_validate(wire, legacy));
    CHECK(cdrom_snapshot_read(wire, legacy));
    drain(100, 505);
    free(wire); free(after);
}
int main(void) {
    delayed_notification();
    dma_continuation();
    dma_arrival_mid_drain();
    request_rearms_retained_sector();
    snapshot_between_request_and_dma();
    snapshot_pending_deadline();
    puts("PASS: CD FIFO latching, pending IRQ, DMA continuation, and state compatibility");
    return 0;
}

void spu_cd_audio_reset(void) {}
void spu_cd_audio_push(const int16_t* stereo,int frames) {(void)stereo;(void)frames;}
void* iso_open(const char* p) {(void)p;return (void*)1;}
void iso_close(void* h) {(void)h;}
int iso_has_subq_replacements(void* h) {(void)h;return 0;}
int iso_read_subq(void* h,uint32_t lba,uint8_t* b,int n,int* v) {(void)h;(void)lba;(void)b;(void)n;*v=0;return 0;}
int iso_read_cdda_sector(void* h,uint32_t lba,uint8_t* b,int n) {(void)h;(void)lba;memset(b,0,n);return 0;}
uint32_t iso_cdda_track_start_lba(void* h,int track) {(void)h;(void)track;return 0;}
uint32_t iso_cdda_track_pregap_lba(void* h,int track) {(void)h;(void)track;return 0;}
int iso_cdda_track_count(void* h) {(void)h;return 1;}
uint32_t iso_cdda_sector_count(void* h) {(void)h;return 1000;}
int iso_cdda_track_is_audio(void* h,int track) {(void)h;(void)track;return 0;}
uint32_t iso_sector_count(void* h) {(void)h;return 1000;}
int iso_track_count(void* h) {(void)h;return 1;}
void event_ring_record_aux(uint16_t k,uint8_t d,uint32_t a) {(void)k;(void)d;(void)a;}
void event_ring_record(uint16_t k,uint8_t d) {(void)k;(void)d;}
void savestate_set_disc_scope(int n) {(void)n;}
int psx_netplay_cd_bisect_active(void) {return 0;}
uint32_t psx_netplay_sim_tick(void) {return 0;}
uint32_t interrupts_get_cycles_since_vblank(void) {return 0;}
int psx_netplay_is_resimulating(void) {return 0;}
void audio_trace_event(uint16_t k,uint32_t a,uint32_t b) {(void)k;(void)a;(void)b;}
