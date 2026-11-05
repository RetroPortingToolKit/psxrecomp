/* Guest-visible read/seek header responses through the production controller,
 * including snapshots taken before the first sector arrives. */
#define main fifo_baseline_main
#include "test_cdrom_sector_fifo.c"
#undef main

static void query_header(int ready, int lba) {
    response_clear();
    ack();
    exec_command(0x10);
    if (!ready) {
        CHECK(irq_flag == CDIRQ_ERROR && response_count == 2);
        CHECK(response_fifo[0] == (uint8_t)(stat_reg | CDSTAT_ERROR));
        CHECK(response_fifo[1] == 0x80);
    } else {
        int m, s, f;
        lba_to_msf(lba, 150, &m, &s, &f);
        CHECK(irq_flag == CDIRQ_ACK && response_count == 8);
        CHECK(response_fifo[0] == bin_to_bcd(m));
        CHECK(response_fifo[1] == bin_to_bcd(s));
        CHECK(response_fifo[2] == bin_to_bcd(f));
        CHECK(response_fifo[3] == 2 && response_fifo[4] == 9);
        CHECK(response_fifo[5] == 7 && response_fifo[6] == 8);
    }
    response_clear();
    ack();
}

static void seek_read(uint8_t cmd) {
    /* Retain a header beyond the new stream's target, as in the reported XA
     * failure. Setloc alone must not discard that last decoded header. */
    setup();
    sector();
    query_header(1, 100);
    exec_command(0x09);
    response_clear(); ack();
    psx_cycle_count += 100000000;
    process_pending(0);
    response_clear(); ack();
    param_fifo[0] = 0; param_fifo[1] = 2; param_fifo[2] = 0x20;
    param_count = 3;
    exec_command(0x02);
    query_header(1, 100);
    exec_command(cmd);
    /* The status says Seek, not Read, so a game checking Read does not poll yet. */
    CHECK((stat_reg & (CDSTAT_READ | CDSTAT_SEEK)) == CDSTAT_SEEK);
    query_header(0, 0);
    /* Subchannel position stays available while the header is invalid. */
    exec_command(0x11);
    CHECK(irq_flag == CDIRQ_ACK && response_count == 8);
    response_clear(); ack();
}

static void read_seek_and_restore(uint8_t cmd) {
    seek_read(cmd);
    const uint32_t size = cdrom_snapshot_bytes();
    uint8_t *wire = malloc(size), *again = malloc(size);
    CHECK(wire && again);
    cdrom_snapshot_write(wire);
    CHECK(cdrom_snapshot_validate(wire, size));
    sector();
    CHECK((stat_reg & (CDSTAT_READ | CDSTAT_SEEK)) == CDSTAT_READ);
    query_header(1, 20);
    CHECK(cdrom_snapshot_read(wire, size));
    /* A load must not resurrect the stale pre-seek header. */
    cdrom_snapshot_write(again);
    CHECK(!memcmp(wire, again, size));
    CHECK((stat_reg & (CDSTAT_READ | CDSTAT_SEEK)) == CDSTAT_SEEK);
    query_header(0, 0);
    sector();
    query_header(1, 20);
    cdrom_snapshot_write(wire);
    /* Restoring a valid header must also clear a later seek's invalidity. */
    seek_read(cmd);
    CHECK(cdrom_snapshot_read(wire, size));
    query_header(1, 20);
    sector();
    query_header(1, 21);
    exec_command(0x09);
    query_header(1, 21);
    exec_command(cmd); /* Pause/resume without Setloc keeps the current header. */
    CHECK((stat_reg & (CDSTAT_READ | CDSTAT_SEEK)) == CDSTAT_READ);
    query_header(1, 21);
    sector();
    query_header(1, 22);
    /* Corrupt validity bytes are rejected without modifying the controller. */
    cdrom_snapshot_write(wire);
    wire[size - 1] = 2;
    CHECK(!cdrom_snapshot_validate(wire, size));
    CHECK(!cdrom_snapshot_read(wire, size));
    query_header(1, 22);
    free(wire); free(again);
}

static void read_seek_interrupted(void) {
    /* AutoPause on an end-of-file first sector ends the seek with the read. */
    setup();
    reading = 0;
    mode_reg = 0x82;
    seek_min = 0; seek_sec = 3; seek_sect = 25;
    setloc_pending = 1;
    exec_command(0x06);
    CHECK((stat_reg & (CDSTAT_READ | CDSTAT_SEEK)) == CDSTAT_SEEK);
    test_submode = XA_SUBMODE_EOF | XA_SUBMODE_AUDIO;
    sector();
    CHECK(!reading);
    CHECK((stat_reg & (CDSTAT_READ | CDSTAT_SEEK)) == CDSTAT_READ);
    /* Pause during the seek leaves neither bit, and a stale sector cannot bring one back. */
    setup();
    reading = 0;
    seek_min = 0; seek_sec = 3; seek_sect = 25;
    setloc_pending = 1;
    exec_command(0x06);
    exec_command(0x09);
    CHECK(!(stat_reg & (CDSTAT_READ | CDSTAT_SEEK)));
    read_seek_landed();
    CHECK(!(stat_reg & (CDSTAT_READ | CDSTAT_SEEK)));
}

int main(void) {
    read_seek_and_restore(0x06);
    read_seek_and_restore(0x1B);
    read_seek_interrupted();
    puts("PASS: GetlocL seek errors, sector headers, pause/resume and snapshots");
    return 0;
}
