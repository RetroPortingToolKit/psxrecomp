/* Derived upload/checkpoint storage is private scratch, not source input. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include "../src/gpu.c"
#include "gpu_exec_stubs.inc"
int main(void) {
    /* Publish half-way through a transfer. Completed upload reconstructs all
     * staging pixels, including the prefix received by the source before it
     * published. Neither old staging contents nor later source writes leak. */
    gpu_write_gp0(0xA0000000u); gpu_write_gp0(0); gpu_write_gp0(0x00010004u);
    gpu_write_gp0(0x22221111u);
    memset(vram_write_pixels, 0xCD, sizeof vram_write_pixels);
    GpuExecutionSnapshot *image = gpu_execution_snapshot_create();
    assert(image && gpu_execution_snapshot_capture(image));
    vram[0] = 0x5555;
    assert(gpu_execution_snapshot_bind(image));
    gpu_write_gp0(0x44443333u);
    assert((*gpu_context_gp0_state()) == GP0_IDLE);
    const uint16_t expected[] = {0x1111,0x2222,0x3333,0x4444};
    assert(!memcmp(*gpu_context_vram_write_pixels(), expected, sizeof expected));
    /* Checkpoint save initializes the private provenance scratch before use. */
    (*gpu_context_texture_window_value()) = 17;
    assert(gpu_pass_checkpoint_save());
    (*gpu_context_texture_window_value()) = 29;
    gpu_pass_checkpoint_restore();
    assert((*gpu_context_texture_window_value()) == 17);
    gpu_execution_snapshot_unbind();
    assert(vram[0] == 0x5555 && gp0_state == GP0_VRAM_WRITE);
    assert(gpu_pass_checkpoint_save());
    assert(!gpu_execution_snapshot_capture(image));
    gpu_pass_checkpoint_restore();
    assert(gpu_execution_snapshot_capture(image));
    gpu_execution_snapshot_destroy(image);
    return 0;
}
