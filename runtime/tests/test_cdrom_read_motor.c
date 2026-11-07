/* Synthetic controller status contract; no mechanical timing model. */
#define main fifo_baseline_main
#include "test_cdrom_sector_fifo.c"
#undef main

static int motor_failures;
#define MOTOR_CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++motor_failures; \
} } while (0)

static void complete_command(void) {
    response_clear();
    ack();
    psx_cycle_count += 100000000;
    process_pending(0);
    response_clear();
    ack();
}

int main(void) {
    const uint8_t commands[] = {0x06, 0x1b};
    for (unsigned n = 0; n < sizeof(commands); ++n) {
        setup();
        exec_command(0x08);
        complete_command();
        MOTOR_CHECK(!(stat_reg & CDSTAT_MOTOR));
        exec_command(commands[n]);
        MOTOR_CHECK(irq_flag == CDIRQ_ACK && reading);
        MOTOR_CHECK(stat_reg & CDSTAT_MOTOR);
        MOTOR_CHECK(response_count == 1 && (response_fifo[0] & CDSTAT_MOTOR));
        response_clear(); ack();
        exec_command(0x09);
        MOTOR_CHECK(stat_reg & CDSTAT_MOTOR);
        complete_command();
        MOTOR_CHECK(stat_reg & CDSTAT_MOTOR);
        exec_command(0x08);
        complete_command();
        MOTOR_CHECK(!(stat_reg & CDSTAT_MOTOR));

        setup();
        exec_command(commands[n]);
        MOTOR_CHECK(stat_reg & CDSTAT_MOTOR);

        /* A read issued while paused still starts the motor without moving
         * the admitted cursor back to the old Setloc target. */
        setup();
        exec_command(0x09);
        complete_command();
        int paused_lba = msf_to_lba(read_min, read_sec, read_sect);
        exec_command(commands[n]);
        MOTOR_CHECK(stat_reg & CDSTAT_MOTOR);
        MOTOR_CHECK(msf_to_lba(read_min, read_sec, read_sect) == paused_lba);

        /* Rejected commands must not spin an absent disc or retarget a
         * stopped drive. These controls pass on the unchanged baseline. */
        setup();
        exec_command(0x08); complete_command();
        param_fifo[0] = 0; param_fifo[1] = 0x60; param_fifo[2] = 0;
        param_count = 3;
        exec_command(0x02);
        MOTOR_CHECK(irq_flag == CDIRQ_ERROR && !(stat_reg & CDSTAT_MOTOR));
        MOTOR_CHECK(msf_to_lba(read_min, read_sec, read_sect) == 100);
        param_count = 0;

        setup();
        stop_read_stream();
        iso_handle = NULL;
        stat_reg = CDSTAT_SHELL;
        exec_command(commands[n]);
        MOTOR_CHECK(irq_flag == CDIRQ_ERROR && !reading);
        MOTOR_CHECK(!(stat_reg & CDSTAT_MOTOR));
    }
    if (motor_failures) return 1;
    puts("PASS: ReadN/ReadS start the motor; Pause retains it and Stop clears it");
    return 0;
}
