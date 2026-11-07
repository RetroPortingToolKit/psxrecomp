/* Store policy and stereo-pair freshness for reversible view transactions.
 * Build/run: ctest -R render_pass_plan_test */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "render_pass.h"
#include "render_pass_plan.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } \
} while (0)


static void test_store_policy(void) {
    CHECK(render_pass_mmio_class(0x1F801810u, 0x28000000u, 4) == -1, "GP0 reaches the GPU");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x04000002u, 4) == -1, "GP1 DMA mode allowed");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x10000007u, 4) == -1, "GP1 info query allowed");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x05000000u, 4) == RENDER_PASS_DROP_GPU,
          "GP1 display start (a flip) is dropped");
    CHECK(render_pass_mmio_class(0x1F801814u, 0x00000000u, 4) == RENDER_PASS_DROP_GPU,
          "GP1 reset is dropped");
    CHECK(render_pass_mmio_class(0x1F8010A8u, 0x01000401u, 4) == -1, "GPU DMA CHCR allowed");
    CHECK(render_pass_mmio_class(0x1F8010E8u, 0x11000002u, 4) == -1, "OTC DMA allowed");
    CHECK(render_pass_mmio_class(0x1F8010F4u, 0, 4) == -1, "DICR allowed (restored)");
    CHECK(render_pass_mmio_class(0x1F8010C8u, 0x01000201u, 4) == RENDER_PASS_DROP_DMA,
          "SPU DMA is dropped");
    CHECK(render_pass_mmio_class(0x1F801074u, 0, 4) == -1, "I_MASK allowed (restored)");
    CHECK(render_pass_mmio_class(0x1F801D88u, 0x00FFu, 2) == RENDER_PASS_DROP_SPU,
          "SPU key-on is dropped (cannot be undone)");
    CHECK(render_pass_mmio_class(0x1F801C00u, 0, 2) == RENDER_PASS_DROP_SPU, "SPU voice regs dropped");
    CHECK(render_pass_mmio_class(0x1F801801u, 0x1Bu, 1) == RENDER_PASS_DROP_CD, "CD command dropped");
    CHECK(render_pass_mmio_class(0x1F801104u, 0, 4) == RENDER_PASS_DROP_TIMER, "timer mode dropped");
    CHECK(render_pass_mmio_class(0x1F801040u, 0, 1) == RENDER_PASS_DROP_OTHER, "SIO dropped");
    CHECK(render_pass_mmio_class(0x1F801820u, 0, 4) == RENDER_PASS_DROP_OTHER, "MDEC dropped");
}

static void test_stereo_pair_fresh(void) {
    const uint32_t vb = 564480u;
    const uint64_t c = 100000000ull;
    CHECK(render_pass_stereo_pair_fresh(c, c, vb), "pair fresh on its own cycle");
    CHECK(render_pass_stereo_pair_fresh(c, c + 8ull * vb, vb), "pair fresh across the slowest cadence");
    CHECK(render_pass_stereo_pair_fresh(c, c + 24ull * vb, vb), "pair fresh at the age limit");
    CHECK(!render_pass_stereo_pair_fresh(c, c + 24ull * vb + 1, vb),
          "pair stale once the plugin stops submitting");
    CHECK(!render_pass_stereo_pair_fresh(c, c - 1, vb), "clock behind pair (state load) is stale");
}

int main(void) {
    test_stereo_pair_fresh();
    test_store_policy();
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
