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
    }
    if (motor_failures) return 1;
    puts("PASS: ReadN/ReadS start the motor; Pause retains it and Stop clears it");
    return 0;
}
