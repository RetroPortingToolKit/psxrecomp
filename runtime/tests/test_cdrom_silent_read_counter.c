/* The drive counts read streams that ended before their first sector, and
 * the longest run of them in a row. The run report carries the three numbers
 * (psx_last_run_report.json "cdrom"), so a game whose reads never deliver can
 * be found from a release build.
 *
 * What is counted:
 *   - a stream "starts" at every ReadN/ReadS that begins a new stream; a Read
 *     that continues the current stream is not a start;
 *   - it "ends" at a Pause, a Stop, or the next read start;
 *   - it is "silent" when no sector was read off the disc between the two;
 *   - a stream that is still open is not counted;
 *   - a resident completion delivers its sectors, so it is not silent.
 * The run resets when a stream that read a sector ends.
 *
 * Controller-level, on the synthetic-sector fixture; no BIOS or game data. */
#define main fifo_baseline_main
#include "test_cdrom_sector_fifo.c"
#undef main

static int failures;
static uint32_t starts, silent, longest;
#define EXPECT(s, q, l, what) do { \
    cdrom_silent_read_stats(&starts, &silent, &longest); \
    if (starts != (uint32_t)(s) || silent != (uint32_t)(q) || longest != (uint32_t)(l)) { \
        fprintf(stderr, "FAIL: %s: starts %u silent %u longest run %u, want %d %d %d\n", \
                what, starts, silent, longest, (s), (q), (l)); \
        ++failures; \
    } } while (0)
#define EXPECT_RUN(n, what) do { if (s_cd_silent_run != (uint32_t)(n)) { \
    fprintf(stderr, "FAIL: %s: current run %u, want %d\n", what, s_cd_silent_run, (n)); \
    ++failures; } } while (0)

/* Acknowledge, let the command's second response fall due, acknowledge. */
static void settle(void) {
    response_clear(); ack();
    psx_cycle_count += 100000000;
    process_pending(0);
    response_clear(); ack();
}
static void target(int lba) {
    int m, s, f;
    lba_to_msf(lba, 150, &m, &s, &f);
    seek_min = (uint8_t)m; seek_sec = (uint8_t)s; seek_sect = (uint8_t)f;
    s_setloc_lba = lba;
    setloc_pending = 1;
}
/* Setloc + ReadN/ReadS, acknowledged. */
static void read_at(uint8_t cmd, int lba) {
    target(lba);
    exec_command(cmd);
    response_clear(); ack();
}
static void pause_drive(void) { exec_command(0x09); settle(); }
static void stop_drive(void) { exec_command(0x08); settle(); }
/* A read cancelled before its first sector, as a library retry does. */
static void silent_read(int lba) { read_at(0x06, lba); pause_drive(); }
/* A read that delivers `sectors` sectors and is then paused. */
static void good_read(int lba, int sectors) {
    read_at(0x06, lba);
    for (int i = 0; i < sectors; ++i) { sector(); response_clear(); ack(); }
    pause_drive();
}

int main(void) {
    setup();
    reading = 0;   /* the fixture forces reading on without opening a stream */
    EXPECT(0, 0, 0, "a new process has counted nothing");

    good_read(1000, 3);
    EXPECT(1, 0, 0, "a read that delivers is not silent");

    read_at(0x06, 5000);
    EXPECT(2, 0, 0, "an open read is not counted while it waits");
    pause_drive();
    EXPECT(2, 1, 1, "a Pause before the first sector ends a silent read");

    /* The retry pattern: the same read restarted and cancelled, five more. */
    for (int i = 0; i < 5; ++i) silent_read(5000);
    EXPECT(7, 6, 6, "six silent reads in a row");

    good_read(5000, 2);
    EXPECT(8, 6, 6, "a read that delivers keeps the record");
    EXPECT_RUN(0, "a read that delivers ends the run");
    silent_read(9000);
    silent_read(9000);
    EXPECT(10, 8, 6, "a new run starts from one; the longest stays");
    EXPECT_RUN(2, "two silent reads since the good one");

    /* Ended by the next read start, with no Pause between. */
    read_at(0x06, 20000);
    read_at(0x06, 30000);
    EXPECT(12, 9, 6, "a read started over a silent one ends it");
    stop_drive();
    EXPECT(12, 10, 6, "Stop ends a silent read");
    stop_drive(); pause_drive();
    EXPECT(12, 10, 6, "Stop and Pause with nothing open count nothing");

    read_at(0x1B, 40000);
    pause_drive();
    EXPECT(13, 11, 6, "ReadS is a read stream too");
    EXPECT_RUN(5, "five silent reads since the good one");

    for (int i = 0; i < 9; ++i) silent_read(40000);
    EXPECT(22, 20, 14, "fourteen in a row is the new record");

    /* A Read of the sector the stream is on continues it: no new start. */
    read_at(0x06, 50000);
    sector(); response_clear(); ack();
    setloc_pending = 0;
    exec_command(0x06); response_clear(); ack();
    EXPECT(23, 20, 14, "a Read that continues the stream is not a start");
    pause_drive();
    EXPECT(23, 20, 14, "the continued stream delivered");
    EXPECT_RUN(0, "the continued stream ends the run");

    /* A resident completion hands the guest its sectors at once. */
    silent_read(60000);
    uint8_t payload[2048];
    memset(payload, 102, sizeof payload);
    resident_setup(0x80);
    CHECK(cdrom_resident_complete_read(100, 3, payload, resident_copy, &test_copy_calls));
    EXPECT(25, 21, 14, "a resident completion is not silent");
    EXPECT_RUN(0, "a resident completion ends the run");

    /* Diagnostics only: a read is timed the same with and without silent
     * reads on the books. */
    int delay[2]; uint8_t status[2];
    for (int pass = 0; pass < 2; ++pass) {
        if (pass) for (int i = 0; i < 4; ++i) silent_read(7000);
        setup(); reading = 0;
        read_at(0x06, 12000);
        delay[pass] = read_delay; status[pass] = stat_reg;
        pause_drive();
    }
    CHECK(delay[0] == delay[1] && status[0] == status[1]);

    if (failures) { fprintf(stderr, "%d checks failed\n", failures); return 1; }
    cdrom_silent_read_stats(&starts, &silent, &longest);
    printf("PASS: silent read streams are counted (starts %u silent %u longest run %u)\n",
           starts, silent, longest);
    return 0;
}
