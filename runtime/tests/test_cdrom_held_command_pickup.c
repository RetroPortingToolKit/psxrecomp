/* A command held behind an unacked INT is picked up CDROM_IRQ_PRESENT_DELAY
 * cycles after the ack, never inside the ack store (PSX-SPX cdromdrive,
 * "First Response (INT3) (or INT5 if failed)"). Regression for issue #873: a
 * held ReadN executed inline restarted the read and emptied the sector buffer
 * before the ISR's BFRD, so PsyQ CD_getsector polled DRQSTS forever. */
#include "../src/cdrom.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
uint32_t i_stat, g_debug_current_func_addr, g_debug_last_store_pc;
uint64_t s_frame_count, psx_cycle_count;
int dma_cdrom_transfer_active(void) { return 0; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)detail; i_stat |= bit; }
int iso_read_raw_sector(void* h, uint32_t lba, uint8_t* out, int size) {
    int m, s, f;
    (void)h;
    memset(out, 0, size);
    lba_to_msf((int)lba, 150, &m, &s, &f);
    out[12] = bin_to_bcd(m); out[13] = bin_to_bcd(s); out[14] = bin_to_bcd(f);
    out[15] = 2; out[16] = out[20] = 9; out[17] = out[21] = 7; out[18] = out[22] = 8;
    memset(out + 24, (unsigned char)lba, 2048);
    return 1;
}
int iso_read_sector(void* h, uint32_t lba, uint8_t* out, int size) {
    (void)h; memset(out, (unsigned char)lba, size); return 1;
}

static void setup(void) {
    psx_cycle_count = 1000000;
    cdrom_init("synthetic");
    setloc_pending = 0;
    mode_reg = 0x80;
    reading = 1;
    read_min = 0; read_sec = 3; read_sect = 25; /* LBA 100 */
}
static void sector(void) { psx_cycle_count += 225792; read_delay = 0; process_read_stream(0); }
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
/* The guest writes a command register while the previous INT is unacked. */
static void hold_readn(void) {
    cdrom_write(0x1f801800, 0);
    cdrom_write(0x1f801801, 0x06);
    CHECK(queued_cmd.pending && queued_cmd.cmd == 0x06);
}

/* Move the destination without consuming the current stream's next sector.
 * A plain ReadN without a new destination uses read_continues_current_stream()
 * and cannot expose the ring-clearing restart that this regression covers. */
static void set_future_location(void) {
    cdrom_write(0x1f801800, 0);
    cdrom_write(0x1f801802, 0x00);
    cdrom_write(0x1f801802, 0x05);
    cdrom_write(0x1f801802, 0x00); /* LBA 225, distinct from current stream */
    cdrom_write(0x1f801801, 0x02);
    ack();
}

static void held_command_picked_up_after_ack(void) {
    setup(); set_future_location(); sector(); /* INT1 announces LBA 100 */
    CHECK(irq_flag == CDIRQ_DATA_READY);
    hold_readn();
    ack();
    /* Not inside the ack store: the ISR still fetches the announced sector. */
    CHECK(queued_cmd.pending && irq_flag == 0);
    CHECK(queued_release_due == psx_cycle_count + CDROM_IRQ_PRESENT_DELAY);
    request(); drain(100, 512);
    psx_cycle_count += CDROM_IRQ_PRESENT_DELAY - 1;
    CHECK(cdrom_cycles_to_internal_event() == 1);
    cdrom_advance(0);
    CHECK(queued_cmd.pending && irq_flag == 0);
    psx_cycle_count++;
    cdrom_advance(0);
    CHECK(!queued_cmd.pending && queued_release_due == 0);
    CHECK(irq_flag == CDIRQ_ACK);
}

static void every_full_ack_restarts_the_pickup(void) {
    setup(); sector();
    hold_readn();
    ack();
    psx_cycle_count += CDROM_IRQ_PRESENT_DELAY / 2;
    set_irq(CDIRQ_DATA_READY);                /* another INT lands before pickup */
    cdrom_advance(0);
    CHECK(queued_cmd.pending);                /* never runs over an unacked INT */
    psx_cycle_count += CDROM_IRQ_PRESENT_DELAY;
    cdrom_advance(0);
    CHECK(queued_cmd.pending && irq_flag == CDIRQ_DATA_READY);
    ack();
    const uint64_t acked = psx_cycle_count;
    psx_cycle_count = acked + CDROM_IRQ_PRESENT_DELAY - 1;
    cdrom_advance(0);
    CHECK(queued_cmd.pending);
    psx_cycle_count = acked + CDROM_IRQ_PRESENT_DELAY;
    cdrom_advance(0);
    CHECK(!queued_cmd.pending && irq_flag == CDIRQ_ACK);
}

static void parameter_fifo_reset_does_not_restart_pickup(void) {
    setup(); sector(); hold_readn(); ack();
    const uint64_t due = queued_release_due;
    psx_cycle_count += CDROM_IRQ_PRESENT_DELAY / 2;
    cdrom_write(0x1f801800, 0);
    cdrom_write(0x1f801802, 0x55);
    CHECK(param_count == 1);
    cdrom_write(0x1f801800, 1);
    cdrom_write(0x1f801803, 0x40); /* CLRPRM, no interrupt acknowledged */
    CHECK(param_count == 0);
    CHECK(queued_release_due == due);
    cdrom_write(0x1f801803, 7); /* redundant ack with no pending interrupt */
    CHECK(queued_release_due == due);
    psx_cycle_count = due;
    cdrom_advance(0);
    CHECK(!queued_cmd.pending && irq_flag == CDIRQ_ACK);
}

static void blocked_pickup_is_not_a_runnable_deadline(void) {
    setup(); sector(); hold_readn(); ack();
    psx_cycle_count += CDROM_IRQ_PRESENT_DELAY / 2;
    set_irq(CDIRQ_DATA_READY);
    /* Isolate the held command from sector/IRQ presentation deadlines. */
    reading = 0;
    irq_enable = 0;
    psx_cycle_count = queued_release_due + 1;
    cdrom_advance(0);
    CHECK(queued_cmd.pending && irq_flag == CDIRQ_DATA_READY);
    CHECK(cdrom_cycles_to_internal_event() == UINT32_MAX);
    CHECK(cdrom_cycles_to_irq(1u << 2) == UINT32_MAX);
    ack();
    CHECK(cdrom_cycles_to_internal_event() == CDROM_IRQ_PRESENT_DELAY);
    CHECK(cdrom_cycles_to_irq(1u << 2) == CDROM_IRQ_PRESENT_DELAY);
}

static void snapshot_keeps_pickup_relative(void) {
    setup(); sector();
    hold_readn();
    ack();
    psx_cycle_count += 700;
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size), *after = malloc(size);
    CHECK(wire && after);
    cdrom_snapshot_write(wire);
    memcpy(after, wire, size);

    setup();
    psx_cycle_count = 0;                      /* restore at another cycle origin */
    CHECK(cdrom_snapshot_read(wire, size));
    CHECK(queued_cmd.pending && queued_release_due == CDROM_IRQ_PRESENT_DELAY - 700);
    psx_cycle_count = queued_release_due - 1;
    cdrom_advance(0);
    CHECK(queued_cmd.pending);
    psx_cycle_count++;
    cdrom_advance(0);
    CHECK(!queued_cmd.pending && irq_flag == CDIRQ_ACK);

    /* An impossible window is rejected before the controller changes. */
    memset(wire + size - 4, 0x7f, 4);
    CHECK(!cdrom_snapshot_validate(wire, size));
    CHECK(!cdrom_snapshot_read(wire, size));
    CHECK(!queued_cmd.pending && irq_flag == CDIRQ_ACK);

    /* A CDF5 state (no window on the wire) still validates and loads. */
    memcpy(wire, after, size);
    CHECK(cdrom_snapshot_validate(wire, size - CDROM_SNAP_CDF6_BYTES));
    setup();
    CHECK(cdrom_snapshot_read(wire, size - CDROM_SNAP_CDF6_BYTES));
    CHECK(queued_release_due == 0);
    free(wire); free(after);
}

int main(void) {
    held_command_picked_up_after_ack();
    every_full_ack_restarts_the_pickup();
    parameter_fifo_reset_does_not_restart_pickup();
    blocked_pickup_is_not_a_runnable_deadline();
    snapshot_keeps_pickup_relative();
    puts("PASS: held CD command is picked up after the acknowledge");
    return 0;
}

void spu_cd_audio_reset(void) {}
void spu_cd_audio_push(const int16_t* stereo, int frames) { (void)stereo; (void)frames; }
void* iso_open(const char* p) { (void)p; return (void*)1; }
void iso_close(void* h) { (void)h; }
int iso_has_subq_replacements(void* h) { (void)h; return 0; }
int iso_read_subq(void* h, uint32_t lba, uint8_t* b, int n, int* v) { (void)h; (void)lba; (void)b; (void)n; *v = 0; return 0; }
int iso_read_cdda_sector(void* h, uint32_t lba, uint8_t* b, int n) { (void)h; (void)lba; memset(b, 0, n); return 0; }
uint32_t iso_cdda_track_start_lba(void* h, int track) { (void)h; (void)track; return 0; }
uint32_t iso_cdda_track_pregap_lba(void* h, int track) { (void)h; (void)track; return 0; }
int iso_cdda_track_count(void* h) { (void)h; return 1; }
uint32_t iso_cdda_sector_count(void* h) { (void)h; return 1000; }
int iso_cdda_track_is_audio(void* h, int track) { (void)h; (void)track; return 0; }
uint32_t iso_sector_count(void* h) { (void)h; return 1000; }
int iso_track_count(void* h) { (void)h; return 1; }
int iso_track_is_audio(void* h, int track) { (void)h; (void)track; return 0; }
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) { (void)k; (void)d; (void)a; }
void event_ring_record(uint16_t k, uint8_t d) { (void)k; (void)d; }
void savestate_set_disc_scope(int n) { (void)n; }
int psx_netplay_cd_bisect_active(void) { return 0; }
uint32_t psx_netplay_sim_tick(void) { return 0; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
int psx_netplay_is_resimulating(void) { return 0; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }
