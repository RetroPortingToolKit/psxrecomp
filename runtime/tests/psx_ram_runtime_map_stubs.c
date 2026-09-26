/* Link doubles for psx_ram_runtime_map_test: everything the REAL memory.c,
 * dma.c and boot_state.c call outside themselves. RAM, the DMA engine and the
 * savestate format are the code under test and are never stubbed. Device
 * snapshot readers count their calls in g_stub_device_restores, which is how
 * the test proves a refused load mutated no device. Section GC (the target is
 * built with -ffunction-sections/--gc-sections like mod_memory_snapshot_test)
 * drops whatever the test never reaches. */
#include "cpu_state.h"

#include <stdint.h>
#include <string.h>

int g_stub_device_restores;

/* ---- data ---------------------------------------------------------------- */
CPUState *debug_cpu_ptr;
uint32_t g_debug_current_func_addr;
uint32_t g_debug_last_store_pc;
volatile int g_ds_recording;
int g_event_step_conservative;
int g_ls_mode;
int g_ls_replay_active;
int g_ls_suppress_record;
int g_ram_read_watch_active;
void (*g_overlay_flush_pending_cycles)(void);
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
uint32_t g_psx_icache_tv[1024];
uint64_t psx_cycle_count;
uint64_t s_frame_count;
uint64_t psx_next_service_cycle;
int psx_in_device_service;
uint32_t g_dirty_ram_dispatch_pc_bitmap[(0x00800000u / 4u + 31u) / 32u];
uint32_t g_dirty_ram_exec_pc_bitmap[(0x00800000u / 4u + 31u) / 32u];
uint32_t g_dirty_ram_exec_page_bitmap[((0x00800000u / 4096u) + 31u) / 32u];

/* ---- hooks the RAM paths notify (no behaviour under test) ------------------ */
void debug_server_trace_mmio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_trace_ram_read_watch(uint32_t p, uint32_t v) { (void)p; (void)v; }
void debug_server_trace_write_check(uint32_t p, uint32_t o, uint32_t n, int w) { (void)p; (void)o; (void)n; (void)w; }
int  card_data_writes_check(uint32_t p, uint32_t v, uint8_t w) { (void)p; (void)v; (void)w; return 0; }
void ds_note_dma_write(void) {}
void ds_note_read(uint32_t a, uint32_t s) { (void)a; (void)s; }
void ds_note_write(uint32_t a, uint32_t s) { (void)a; (void)s; }
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) { (void)k; (void)d; (void)a; }
void audio_trace_event(uint16_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }
void parity_trace_note_write(uint32_t a, uint32_t w, uint32_t pc) { (void)a; (void)w; (void)pc; }
uint32_t ls_read_hook(uint32_t a, int s, uint32_t v) { (void)a; (void)s; return v; }
void ls_write_hook(uint32_t a, int s, uint32_t v) { (void)a; (void)s; (void)v; }
int  fntrace_is_game_started(void) { return 0; }
void overlay_loader_note_code_write(void) {}
void overlay_loader_resync_validation_after_restore(void) {}
void gte_canonicalize_cpu_state(CPUState *cpu) { (void)cpu; }

/* ---- scheduler / interrupts ------------------------------------------------ */
void psx_advance_cycles_slow(uint32_t c) { psx_cycle_count += c; }
void psx_devices_mmio_sync(void) {}
void psx_devices_service_to_now(void) {}
int  psx_get_in_exception(void) { return 0; }
void psx_irq_raise(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void psx_irq_refresh_cause_ip2(void) {}
void psx_fatal_halt(const char *reason) { (void)reason; __builtin_trap(); }
void sio_card_handoff_on_imask(uint32_t o, uint32_t n) { (void)o; (void)n; }
static uint32_t s_csv;
uint32_t interrupts_get_cycles_since_vblank(void) { return s_csv; }
void interrupts_set_cycles_since_vblank(uint32_t v) { s_csv = v; g_stub_device_restores++; }
void timers_get_snapshot(uint16_t c[3], uint32_t m[3], uint16_t t[3], int32_t l[3], uint32_t f[3]) {
    for (int i = 0; i < 3; i++) { c[i] = (uint16_t)i; m[i] = 0; t[i] = 0; l[i] = 0; f[i] = 0; }
}
void timers_set_snapshot(const uint16_t c[3], const uint32_t m[3], const uint16_t t[3],
                         const int32_t l[3], const uint32_t f[3]) {
    (void)c; (void)m; (void)t; (void)l; (void)f; g_stub_device_restores++;
}

/* ---- MMIO devices the routing reaches ------------------------------------- */
void cdrom_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void timers_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void spu_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void sio_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void mdec_write(uint32_t a, uint32_t v) { (void)a; (void)v; }
void gpu_write_gp0(uint32_t v) { (void)v; }
void gpu_write_gp1(uint32_t v) { (void)v; }
void gpu_set_gp0_source(uint32_t a) { (void)a; }
uint32_t gpu_read_gpuread(void) { return 0; }
uint32_t spu_dma_read(void) { return 0; }
void spu_dma_write(uint32_t w) { (void)w; }
void mdec_debug_dma_in_start(uint32_t a, uint32_t w) { (void)a; (void)w; }
void mdec_debug_dma_out_start(uint32_t a, uint32_t w) { (void)a; (void)w; }
void cdrom_debug_snapshot(void *out) { (void)out; }
uint32_t cdrom_dma_sector_word_count(void) { return 0; }
void dma_gpu_ll_start(void *s, uint32_t a, uint32_t m) { (void)s; (void)a; (void)m; }
void dma_gpu_ll_cancel(void *s) { (void)s; }
void gpu_ws_begin_linked_list(void) {}
void gpu_ws_end_linked_list(void) {}
void gpu_ws_prepass_linked_list(uint32_t a) { (void)a; }
void gpu_ws_restore_linked_list_rank(uint32_t r) { (void)r; }

/* ---- device snapshot sections (fixed-size doubles; readers count) --------- */
#define STUB_SECTION(name, size)                                             \
    uint32_t name##_snapshot_bytes(void) { return (size); }                  \
    void name##_snapshot_write(uint8_t *p) { memset(p, 0xA5, (size)); }      \
    int name##_snapshot_read(const uint8_t *p, uint32_t len) {               \
        (void)p; g_stub_device_restores++; return len == (size);             \
    }
STUB_SECTION(gpu, 96u)
STUB_SECTION(spu, 80u)
STUB_SECTION(cdrom, 64u)
STUB_SECTION(sio, 48u)
STUB_SECTION(mdec, 400u)
int sio_snapshot_validate(const uint8_t *p, uint32_t len) { return p && len == 48u; }
int mdec_snapshot_validate(const uint8_t *p, uint32_t len) { return p && len == 400u; }

static uint8_t s_spu_ram[512u * 1024u];
uint8_t *spu_get_ram_ptr(void) { return s_spu_ram; }
uint32_t spu_get_ram_bytes(void) { return (uint32_t)sizeof s_spu_ram; }

static uint16_t s_vram[1024u * 512u];
const uint16_t *gpu_get_vram(void) { return s_vram; }
int gpu_vram_dirty_tracking(void) { return 0; }
int gpu_vram_dirty_verify_enabled(void) { return 0; }
uint32_t gpu_vram_dirty_row_count(void) { return 512u; }
const uint64_t *gpu_vram_dirty_mask(void) { static uint64_t m[8]; return m; }
void gpu_vram_dirty_clear(void) {}
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *d) {
    (void)x; (void)y; memcpy(d, s_vram, (size_t)w * (size_t)h * 2u);
}
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *d) {
    (void)x; (void)y; memcpy(s_vram, d, (size_t)w * (size_t)h * 2u);
    g_stub_device_restores++;
}
