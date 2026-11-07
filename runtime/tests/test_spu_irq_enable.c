/* Authored IRQ-enable controls using the existing SPU fidelity fixture.
 * Its unrelated volume-sweep expectations are outside this regression. */
#define main spu_fidelity_original_main
#include "test_spu_fidelity.c"
#undef main

/* IRQ enable alone must not raise IRQ9 while the SPU unit is disabled.
 * RAM transfers still happen; only the interrupt request is gated. */
static void test_irq_requires_spu_enable(void) {
    spu_init();
    g_irq_raises = 0;
    wr(R_IRQADDR, 0x0600);
    wr(R_XFERADDR, 0x0600);
    wr(R_SPUCNT, 0x0040); /* IRQ on, SPU off; compare at the current address */
    CHECK(g_irq_raises == 0);
    CHECK((rd(R_SPUSTAT) & 0x40u) == 0);
    wr(R_FIFO, 0xBEEF);
    CHECK(spu_ram[0x3000] == 0xEF && spu_ram[0x3001] == 0xBE);
    wr(R_XFERADDR, 0x0600);
    spu_dma_write(0x12345678u);
    wr(R_XFERADDR, 0x0600);
    CHECK(spu_dma_read() == 0x12345678u);
    CHECK(g_irq_raises == 0);
    CHECK((rd(R_SPUSTAT) & 0x40u) == 0);

    wr(R_XFERADDR, 0x0600);
    wr(R_SPUCNT, 0x8040); /* both enable bits: the same address now raises */
    CHECK(g_irq_raises == 1);
    CHECK((rd(R_SPUSTAT) & 0x40u) != 0);
    wr(R_SPUCNT, 0x8000); /* existing bit-6 acknowledgement */
    CHECK((rd(R_SPUSTAT) & 0x40u) == 0);
    wr(R_SPUCNT, 0x0040); /* disabling the unit must not re-arm a request */
    wr(R_XFERADDR, 0x0600);
    CHECK(g_irq_raises == 1);
    CHECK((rd(R_SPUSTAT) & 0x40u) == 0);
    wr(R_SPUCNT, 0x8040);
    CHECK(g_irq_raises == 2);
}

int main(void) {
    test_irq_requires_spu_enable();
    test_irq_transfer_paths();
    test_irq_disabled_never_fires();
    test_irq_on_block_fetch();
    test_irq_on_capture_write();
    test_irq_on_reverb_access();
    printf("spu_irq_enable: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
