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
    /* Derived display caches follow register writes, state restore and private
     * snapshot ownership. Compare each cached result with the uncached view. */
    gpu_init();
    GpuDisplayInfo actual, display_expected, source;
    const uint32_t modes[] = {0, 1, 2, 3, 0x40, 0x0C, 0x18};
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; ++i) {
        gpu_write_gp1(0x08000000u | modes[i]);
        gpu_write_gp1(0x0500180Cu);
        gpu_write_gp1(0x060C0200u);
        gpu_write_gp1(0x07040010u);
        gpu_build_display_info(&display_expected);
        gpu_get_display_info(&actual);
        assert(!memcmp(&actual, &display_expected, sizeof actual));
        gpu_get_display_info(&actual);
        assert(!memcmp(&actual, &display_expected, sizeof actual));
    }
    source = actual;
    image = gpu_execution_snapshot_create();
    assert(image && gpu_execution_snapshot_capture(image));
    assert(gpu_execution_snapshot_bind(image));
    gpu_get_display_info(&actual);
    assert(!memcmp(&actual, &source, sizeof actual));
    gpu_write_gp1(0x08000003u);
    gpu_write_gp1(0x05000029u);
    gpu_build_display_info(&display_expected);
    gpu_get_display_info(&actual);
    assert(!memcmp(&actual, &display_expected, sizeof actual));
    assert(actual.display_x == 41);
    assert(gpu_pass_checkpoint_save());
    gpu_write_gp1(0);
    gpu_get_display_info(&actual);
    assert(actual.display_x == 0);
    gpu_pass_checkpoint_restore();
    gpu_get_display_info(&actual);
    assert(!memcmp(&actual, &display_expected, sizeof actual));
    gpu_execution_snapshot_unbind();
    gpu_get_display_info(&actual);
    assert(!memcmp(&actual, &source, sizeof actual));
    gpu_write_gp1(0x08000001u);
    assert(gpu_execution_snapshot_capture(image));
    gpu_get_display_info(&source);
    assert(gpu_execution_snapshot_bind(image));
    gpu_get_display_info(&actual);
    assert(!memcmp(&actual, &source, sizeof actual));
    gpu_execution_snapshot_unbind();
    gpu_execution_snapshot_destroy(image);
    uint32_t bytes = gpu_snapshot_bytes();
    uint8_t *saved = malloc(bytes);
    assert(saved);
    gpu_snapshot_write(saved);
    gpu_write_gp1(0);
    gpu_get_display_info(&actual);
    assert(gpu_snapshot_read(saved, bytes));
    gpu_get_display_info(&actual);
    assert(!memcmp(&actual, &source, sizeof actual));
    free(saved);
    gpu_set_power_on_video_mode(1);
    gpu_build_display_info(&display_expected);
    gpu_get_display_info(&actual);
    assert(!memcmp(&actual, &display_expected, sizeof actual));
    return 0;
}
