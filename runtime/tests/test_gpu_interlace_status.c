/* Exercise the production GPUSTAT reader with controlled guest video time.
 * GCC -O2 -fwhole-program removes unrelated renderer dependencies. */
#include "../src/gpu.c"

static uint32_t phase;
uint32_t g_psx_vblank_cycles = 314000u;
uint32_t interrupts_get_cycles_since_vblank(void) { return phase; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    return 1; \
} } while (0)

int main(void) {
    vertical_interlace = vres = video_mode = 1u;
    v_display_y1 = 35u;
    v_display_y2 = 291u; /* 256 active + 58 blank scanlines */
    lcf = 1u;
    phase = 0u;
    CHECK((gpu_read_gpustat() >> 31) == 0u);
    phase = 57999u;
    CHECK((gpu_read_gpustat() >> 31) == 0u);
    phase = 58000u;
    CHECK((gpu_read_gpustat() >> 31) == 1u);
    for (unsigned i = 0; i < 2000u; ++i)
        CHECK((gpu_read_gpustat() >> 31) == 1u);
    CHECK(phase == 58000u && lcf == 1u);
    lcf = 0u;
    CHECK((gpu_read_gpustat() >> 31) == 0u);
    phase = 0u;
    CHECK((gpu_read_gpustat() >> 31) == 0u);

    /* NTSC display range and a disabled/empty range have their own boundary. */
    video_mode = 0u;
    g_psx_vblank_cycles = 263000u;
    v_display_y1 = 16u;
    v_display_y2 = 256u;
    lcf = 1u;
    phase = 22999u;
    CHECK((gpu_read_gpustat() >> 31) == 0u);
    phase = 23000u;
    CHECK((gpu_read_gpustat() >> 31) == 1u);
    v_display_y2 = v_display_y1;
    phase = 262999u;
    CHECK((gpu_read_gpustat() >> 31) == 0u);
    puts("PASS: interlaced GPUSTAT exposes blanking without read-driven time");
    return 0;
}
