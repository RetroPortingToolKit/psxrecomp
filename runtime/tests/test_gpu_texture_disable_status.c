/* GP0(E1h) bit 11 (texture disable) must not reach GPUSTAT bit 15 unless
 * GP1(09h) allowed it. The runtime does not model GP1(09h) (it halts), so the
 * allow switch is always in its reset state and bit 15 must read 0.
 * GCC -O2 -fwhole-program removes unrelated renderer dependencies. */
#include "../src/gpu.c"

uint32_t vblank_cycles = 263000u;
uint32_t interrupts_get_cycles_since_vblank(void) { return 0u; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
int interrupts_raster_gpu_status(uint32_t *bits) { (void)bits; return 0; }

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; \
} } while (0)

static void draw_mode(uint32_t word) {
    gp0_cmd_buf[0] = word;
    gp0_exec_draw_mode();
}

int main(void) {
    /* Synthetic draw mode: texpage X=Ah Y=1, dither on, bit 11 set. */
    draw_mode(0xE1000A1Au);
    uint32_t stat = gpu_read_gpustat();
    CHECK((stat & 0x07FFu) == 0x021Au);   /* bits 0-10 copy E1 bits 0-10 */
    CHECK(((stat >> 15) & 1u) == 0u);     /* bit 15 stays clear */
    CHECK(texture_disable == 0u);

    /* All of bits 0-11 set: bits 0-10 follow, bit 15 still clear. */
    draw_mode(0xE1000FFFu);
    stat = gpu_read_gpustat();
    CHECK((stat & 0x07FFu) == 0x07FFu);
    CHECK(((stat >> 15) & 1u) == 0u);

    /* Bit 11 clear again: no change to bit 15. */
    draw_mode(0xE1000000u);
    stat = gpu_read_gpustat();
    CHECK((stat & 0x87FFu) == 0u);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: GP0(E1h) bit 11 does not set GPUSTAT bit 15 without GP1(09h)");
    return 0;
}

void gr_set_dither(int enabled) { (void)enabled; }
