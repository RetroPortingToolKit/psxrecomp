/* Device time must advance inside a handler with CPU interrupts disabled.
 * The test includes the owning sources to stage a handler context without a
 * BIOS; GCC's whole-program optimization drops the unrelated machinery. */
#include "../src/psx_video_timing.c"
#include "../src/interrupts.c"

static unsigned field_ticks;
static uint64_t fixture_cycles;
uint32_t i_stat, i_mask;

uint64_t psx_get_cycle_count(void) { return fixture_cycles; }
uint32_t sio_get_seq(void) { return 0; }
int sio_card_protocol_active(void) { return 0; }
void gpu_vblank_tick(void) { field_ticks++; }
void event_ring_record(uint16_t kind, uint8_t detail) {
    (void)kind; (void)detail;
}
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
void device_trace_note(uint32_t bit, uint32_t detail) {
    (void)bit; (void)detail;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    return 1; \
} } while (0)

int main(void) {
    CPUState cpu = {0};
    cpu.cop0[COP0_SR] = 0x404u; /* handler: IEc clear, saved IE set */
    i_mask = 1u << IRQ_VBLANK;
    in_exception = 1;
    exception_nest_depth = 1;
    g_psx_vblank_cycles = 100u;

    /* A handler can poll a field change; no early edge or CPU delivery. */
    fixture_cycles += 99u;
    interrupts_advance_cycles(99u);
    CHECK(field_ticks == 0u && i_stat == 0u);
    fixture_cycles++;
    interrupts_advance_cycles(1u);
    CHECK(field_ticks == 1u && i_stat == 1u);
    CHECK(interrupts_get_cycles_since_vblank() == 0u);
    CHECK(!psx_interrupt_delivery_needed(&cpu));
    CHECK(cpu.cop0[COP0_SR] == 0x404u && in_exception == 1);

    /* Multiple elapsed fields retain overshoot even while IRQ stays pending. */
    fixture_cycles += 250u;
    interrupts_advance_cycles(250u);
    CHECK(field_ticks == 3u && i_stat == 1u);
    CHECK(interrupts_get_cycles_since_vblank() == 50u);
    interrupts_service_scheduled_events();
    CHECK(field_ticks == 3u); /* polling adds no invented clock */
    CHECK(!psx_interrupt_delivery_needed(&cpu));

    /* CPU eligibility changes only when the guest restores interrupt enable. */
    in_exception = 0;
    exception_nest_depth = 0;
    cpu.cop0[COP0_SR] = 0x401u;
    CHECK(psx_interrupt_delivery_needed(&cpu));
    i_mask = 0u;
    CHECK(!psx_interrupt_delivery_needed(&cpu));
    puts("PASS: device fields advance in exception; CPU interrupt gates hold");
    return 0;
}

int g_psx_guest_time_frozen;
void psx_guest_cycle_scale_vblank(void) {}
