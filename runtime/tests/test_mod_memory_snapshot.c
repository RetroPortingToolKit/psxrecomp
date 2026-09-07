#include "mod_memory.h"
#include "boot_state.h"
#include "cpu_state.h"
#include "dirty_ram_interp.h"
#include "psx_bios_image.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static int failures;

#define CHECK(name, cond) do { \
    if (cond) { \
        printf("PASS  %s\n", name); \
    } else { \
        fprintf(stderr, "FAIL  %s\n", name); \
        failures++; \
    } \
} while (0)

uint64_t psx_cycle_count;
int g_ls_mode;
int g_ls_suppress_record;
int g_ls_replay_active;
int g_event_step_conservative;
int psx_in_device_service;
volatile int g_ds_recording;
int g_dma_exec_depth;
int g_dma_cur_ch;
uint32_t g_dma_cur_madr;
uint32_t g_dma_cur_bcr;
uint32_t g_debug_last_store_pc;
uint64_t s_frame_count;
uint32_t *sr_ptr;
int g_ram_read_watch_active;
void (*g_overlay_flush_pending_cycles)(void);
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
uint32_t *g_psx_cyc_local_acc;
uint64_t psx_next_service_cycle;

int fntrace_is_game_started(void) { return 1; }
int psx_get_in_exception(void) { return 0; }
uint32_t interrupts_get_cycles_since_vblank(void) { return 0; }
void interrupts_set_cycles_since_vblank(uint32_t cycles) { (void)cycles; }
void timers_get_snapshot(uint16_t counter[3], uint32_t mode[3],
                         uint16_t target[3], int32_t irq_line[3],
                         uint32_t frac[3]) {
    memset(counter, 0, 3u * sizeof(counter[0]));
    memset(mode, 0, 3u * sizeof(mode[0]));
    memset(target, 0, 3u * sizeof(target[0]));
    memset(irq_line, 0, 3u * sizeof(irq_line[0]));
    memset(frac, 0, 3u * sizeof(frac[0]));
}
void timers_set_snapshot(const uint16_t counter[3], const uint32_t mode[3],
                         const uint16_t target[3], const int32_t irq_line[3],
                         const uint32_t frac[3]) {
    (void)counter; (void)mode; (void)target; (void)irq_line; (void)frac;
}
void psx_devices_service_to_now(void) {}
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; }
uint32_t mmio_read32(uint32_t phys) { (void)phys; return 0; }
uint16_t mmio_read16(uint32_t phys) { (void)phys; return 0; }
uint8_t mmio_read8(uint32_t phys) { (void)phys; return 0; }
void mmio_write32(uint32_t phys, uint32_t val) { (void)phys; (void)val; }
void mmio_write16(uint32_t phys, uint16_t val) { (void)phys; (void)val; }
void mmio_write8(uint32_t phys, uint8_t val) { (void)phys; (void)val; }
void unmapped_fatal(uint32_t addr, uint32_t phys, const char *op) {
    fprintf(stderr, "unexpected unmapped %s addr=%08x phys=%08x\n",
            op, addr, phys);
    exit(2);
}
uint32_t read_ram_word(uint32_t phys) { (void)phys; return 0; }
uint16_t read_ram_half(uint32_t phys) { (void)phys; return 0; }
void debug_server_trace_ram_read_watch(uint32_t phys, uint32_t value) { (void)phys; (void)value; }
void debug_server_trace_write_check(uint32_t phys, uint32_t old_value, uint32_t new_value, uint32_t size) { (void)phys; (void)old_value; (void)new_value; (void)size; }
void parity_trace_note_write(uint32_t phys, uint32_t size, uint32_t pc) { (void)phys; (void)size; (void)pc; }
void card_data_writes_check(uint32_t phys, uint32_t value, uint32_t size) { (void)phys; (void)value; (void)size; }
void dirty_ram_mark_kernel_write(uint32_t phys) { (void)phys; }
void text_guard_note_write(uint32_t phys, uint32_t value, uint32_t size) { (void)phys; (void)value; (void)size; }
void overlay_watch_note_write(uint32_t phys, uint32_t size) { (void)phys; (void)size; }
uint32_t effective_store_pc(void) { return 0; }
void ls_write_hook(uint32_t addr, uint32_t size, uint32_t val) { (void)addr; (void)size; (void)val; }
uint32_t ls_read_hook(uint32_t addr, uint32_t size, uint32_t val) { (void)addr; (void)size; return val; }
void ds_note_dma_write(void) {}
void ds_note_write(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
void ds_note_read(uint32_t addr, uint32_t size) { (void)addr; (void)size; }
void overlay_loader_note_code_write(void) {}
void overlay_loader_resync_validation_after_restore(void) {}
void overlay_loader_active_write_check(uint32_t phys, uint32_t size) { (void)phys; (void)size; }
void psx_devices_mmio_sync(void) {}
void debug_server_trace_mmio_read(uint32_t addr, uint32_t val, uint8_t width) { (void)addr; (void)val; (void)width; }
void debug_server_trace_mmio_write(uint32_t addr, uint32_t val, uint8_t width) { (void)addr; (void)val; (void)width; }
uint32_t sio_read(uint32_t addr) { (void)addr; return 0; }
void sio_write(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
void sio_tick(int cycles) { (void)cycles; }
uint32_t dma_read(uint32_t addr) { (void)addr; return 0; }
void dma_write(uint32_t addr, uint32_t val) { (void)addr; (void)val; }
void dma_write_masked(uint32_t addr, uint32_t val, uint32_t mask) { (void)addr; (void)val; (void)mask; }
uint32_t timers_read(uint32_t addr) { (void)addr; return 0; }
void timers_write(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
uint32_t cdrom_read(uint32_t addr) { (void)addr; return 0; }
void cdrom_write(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
uint32_t gpu_read_gpuread(void) { return 0; }
uint32_t gpu_read_gpustat(void) { return 0; }
void gpu_set_gp0_source(uint32_t addr) { (void)addr; }
void gpu_write_gp0(uint32_t val) { (void)val; }
void gpu_write_gp1(uint32_t val) { (void)val; }
uint32_t mdec_read(uint32_t addr) { (void)addr; return 0; }
void mdec_write(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
uint32_t spu_read(uint32_t addr) { (void)addr; return 0; }
void spu_write(uint32_t addr, uint32_t value) { (void)addr; (void)value; }
void psx_irq_refresh_cause_ip2(void) {}
int sio_card_should_hold_imask_bit7(void) { return 0; }
void sio_card_handoff_on_imask(uint32_t old_mask, uint32_t new_mask) { (void)old_mask; (void)new_mask; }

CPUState *debug_cpu_ptr;
uint32_t g_debug_current_func_addr;
uint32_t g_dma_initiator_pc;
uint32_t g_overlay_region_floor;
uint32_t g_text_image_lo;
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS];
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS];
PsxBiosImageInfo psx_bios_image;
const PsxKernelBody *psx_bios_kernel_bodies;
uint32_t psx_bios_kernel_body_count;

uint32_t psx_read_word(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);
uint8_t psx_read_byte(uint32_t addr);
void psx_write_word(uint32_t addr, uint32_t val);
void psx_write_half(uint32_t addr, uint16_t val);
void psx_write_byte(uint32_t addr, uint8_t val);

uint32_t g_psx_icache_tv[1024];
static uint16_t vram[1024u * 512u];
static uint8_t spu_ram[16];
static uint32_t g_dma_snapshot_madr;

void gte_canonicalize_cpu_state(CPUState *cpu) { (void)cpu; }
uint32_t gpu_snapshot_bytes(void) { return 4u; }
void gpu_snapshot_write(uint8_t *p) { memset(p, 0xA5, gpu_snapshot_bytes()); }
int gpu_snapshot_read(const uint8_t *p, uint32_t len) { (void)p; return len == gpu_snapshot_bytes(); }
uint32_t spu_snapshot_bytes(void) { return 4u; }
void spu_snapshot_write(uint8_t *p) { memset(p, 0x5A, spu_snapshot_bytes()); }
int spu_snapshot_read(const uint8_t *p, uint32_t len) { (void)p; return len == spu_snapshot_bytes(); }
uint8_t *spu_get_ram_ptr(void) { return spu_ram; }
uint32_t spu_get_ram_bytes(void) { return (uint32_t)sizeof(spu_ram); }
uint32_t cdrom_snapshot_bytes(void) { return 4u; }
void cdrom_snapshot_write(uint8_t *p) { memset(p, 0xC0, cdrom_snapshot_bytes()); }
int cdrom_snapshot_read(const uint8_t *p, uint32_t len) { (void)p; return len == cdrom_snapshot_bytes(); }
uint32_t dma_snapshot_bytes(void) { return 4u; }
void dma_snapshot_write(uint8_t *p) {
    p[0] = (uint8_t)g_dma_snapshot_madr;
    p[1] = (uint8_t)(g_dma_snapshot_madr >> 8);
    p[2] = (uint8_t)(g_dma_snapshot_madr >> 16);
    p[3] = (uint8_t)(g_dma_snapshot_madr >> 24);
}
int dma_snapshot_read(const uint8_t *p, uint32_t len) {
    if (len != dma_snapshot_bytes())
        return 0;
    g_dma_snapshot_madr = (uint32_t)p[0] |
                          ((uint32_t)p[1] << 8) |
                          ((uint32_t)p[2] << 16) |
                          ((uint32_t)p[3] << 24);
    return 1;
}
uint32_t sio_snapshot_bytes(void) { return 4u; }
void sio_snapshot_write(uint8_t *p) { memset(p, 0x51, sio_snapshot_bytes()); }
int sio_snapshot_read(const uint8_t *p, uint32_t len) { (void)p; return len == sio_snapshot_bytes(); }
uint32_t mdec_snapshot_bytes(void) { return 4u; }
void mdec_snapshot_write(uint8_t *p) { memset(p, 0x4D, mdec_snapshot_bytes()); }
int mdec_snapshot_read(const uint8_t *p, uint32_t len) { (void)p; return len == mdec_snapshot_bytes(); }
const uint16_t *gpu_get_vram(void) { return vram; }
void gr_vram_transfer_in(int x, int y, int w, int h, const uint16_t *data) {
    (void)x; (void)y;
    memcpy(vram, data, (size_t)w * (size_t)h * sizeof(uint16_t));
}
void gr_vram_transfer_out(int x, int y, int w, int h, uint16_t *data) {
    (void)x; (void)y;
    memcpy(data, vram, (size_t)w * (size_t)h * sizeof(uint16_t));
}
int gpu_vram_dirty_tracking(void) { return 0; }
uint32_t gpu_vram_dirty_row_count(void) { return 512u; }
const uint64_t *gpu_vram_dirty_mask(void) { static uint64_t mask[8]; return mask; }
int gpu_vram_dirty_verify_enabled(void) { return 0; }
void gpu_vram_dirty_clear(void) {}

uLong compressBound(uLong sourceLen) { return sourceLen + 16u; }
int compress2(Bytef *dest, uLongf *destLen, const Bytef *source,
              uLong sourceLen, int level) {
    (void)dest; (void)destLen; (void)source; (void)sourceLen; (void)level;
    return Z_MEM_ERROR;
}
int uncompress(Bytef *dest, uLongf *destLen, const Bytef *source,
               uLong sourceLen) {
    (void)dest; (void)destLen; (void)source; (void)sourceLen;
    return Z_DATA_ERROR;
}

static void put_u32le(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64le(const uint8_t *p) {
    uint64_t lo = get_u32le(p);
    uint64_t hi = get_u32le(p + 4);
    return lo | (hi << 32);
}

static int strip_modmem_section(uint8_t *state, size_t *state_len) {
    uint32_t sections = get_u32le(state + 28u);
    uint8_t *cur = state + BOOT_STATE_HEADER_WIRE_BYTES;
    uint8_t *end = state + *state_len;

    for (uint32_t i = 0; i < sections; i++) {
        uint8_t *section = cur;
        uint32_t tag;
        uint64_t len;
        size_t section_bytes;

        if ((size_t)(end - cur) < 16u)
            return 0;
        tag = get_u32le(cur);
        len = get_u64le(cur + 8u);
        cur += 16u;
        if ((uint64_t)(end - cur) < len)
            return 0;
        cur += (size_t)len;
        section_bytes = (size_t)(cur - section);
        if (tag == BS_SEC_MODMEM) {
            memmove(section, cur, (size_t)(end - cur));
            *state_len -= section_bytes;
            put_u32le(state + 4u, 7u);
            put_u32le(state + 28u, sections - 1u);
            return 1;
        }
    }
    return 0;
}

static void mutate_allocated_memory(uint32_t guest, uint32_t gpu) {
    psx_write_word(guest, 0x11223344u);
    psx_write_half(guest + 4u, 0x5566u);
    psx_write_byte(guest + 6u, 0x77u);
    psx_write_word(gpu, 0x89ABCDEFu);
    psx_write_half(gpu + 4u, 0xCAFEu);
    psx_write_byte(gpu + 6u, 0x42u);
}

static void test_boot_state_restores_mod_dma_payload(void) {
    CPUState cpu;
    CPUState loaded;
    uint32_t guest = psx_mod_memory_alloc(64u, 16u);
    uint32_t gpu = psx_mod_gpu_dma_memory_alloc(128u, 32u);
    uint8_t *state = NULL;
    size_t state_len = 0;
    uint32_t resolved;

    memset(&cpu, 0, sizeof(cpu));
    memset(&loaded, 0, sizeof(loaded));
    cpu.pc = 0x80010000u;
    cpu.gpr[4] = guest;
    g_dma_snapshot_madr = gpu;
    mutate_allocated_memory(guest, gpu);

    CHECK("boot state raw save with active mod allocations succeeds",
          boot_state_save_buffer_raw(&cpu, 0x12345678u, 0x80010000u,
                                     &state, &state_len));
    CHECK("boot state buffer allocated", state != NULL && state_len > 0u);
    if (!state)
        return;

    psx_write_word(guest, 0u);
    psx_write_word(gpu, 0u);
    g_dma_snapshot_madr = 0u;
    CHECK("boot state load restores active mod allocations",
          boot_state_load_buffer(state, state_len, 0x12345678u, 0x80010000u,
                                 &loaded));
    resolved = psx_mod_gpu_dma_resolve_address(g_dma_snapshot_madr);
    CHECK("boot state restored DMA pointer into mod GPU aperture",
          g_dma_snapshot_madr == gpu && resolved == (gpu & 0x00FFFFFCu));
    CHECK("boot state restored guest mod payload",
          psx_read_word(guest) == 0x11223344u);
    CHECK("boot state restored GPU DMA command payload",
          psx_read_word(gpu) == 0x89ABCDEFu);
    free(state);
}

static void test_legacy_missing_modmem_rejects_before_mutation(void) {
    CPUState cpu;
    CPUState loaded;
    uint32_t guest = psx_mod_memory_alloc(64u, 16u);
    uint32_t gpu = psx_mod_gpu_dma_memory_alloc(128u, 32u);
    uint8_t *state = NULL;
    size_t state_len = 0;

    memset(&cpu, 0, sizeof(cpu));
    memset(&loaded, 0, sizeof(loaded));
    cpu.pc = 0x80010000u;
    g_dma_snapshot_madr = gpu;
    mutate_allocated_memory(guest, gpu);
    CHECK("boot state raw save for legacy strip succeeds",
          boot_state_save_buffer_raw(&cpu, 0x12345678u, 0x80010000u,
                                     &state, &state_len));
    if (!state)
        return;
    CHECK("test stripped mod memory section",
          strip_modmem_section(state, &state_len));

    psx_write_word(guest, 0xAABBCCDDu);
    psx_write_word(gpu, 0x55667788u);
    CHECK("legacy state missing mod memory rejects",
          !boot_state_load_buffer(state, state_len, 0x12345678u, 0x80010000u,
                                  &loaded));
    CHECK("legacy reject happened before guest mod memory mutation",
          psx_read_word(guest) == 0xAABBCCDDu);
    CHECK("legacy reject happened before gpu mod memory mutation",
          psx_read_word(gpu) == 0x55667788u);
    free(state);
}

int main(void) {
    uint32_t guest;
    uint32_t gpu;
    uint32_t next_guest;
    uint32_t next_gpu;
    uint32_t bytes;
    uint8_t *snapshot;

    CHECK("fresh mod snapshot is not required", !memory_mod_snapshot_required());

    guest = psx_mod_memory_alloc(32u, 16u);
    gpu = psx_mod_gpu_dma_memory_alloc(64u, 32u);
    CHECK("guest allocation succeeds", guest == 0x9F000000u);
    CHECK("gpu dma allocation succeeds", gpu == PSX_MOD_GPU_DMA_GUEST_BASE);
    CHECK("allocated mod snapshot is required", memory_mod_snapshot_required());

    mutate_allocated_memory(guest, gpu);
    bytes = memory_mod_snapshot_bytes();
    snapshot = (uint8_t *)malloc(bytes);
    CHECK("snapshot buffer allocated", snapshot != NULL);
    if (!snapshot)
        return 1;
    memory_mod_snapshot_write(snapshot);

    psx_write_word(guest, 0u);
    psx_write_half(guest + 4u, 0u);
    psx_write_byte(guest + 6u, 0u);
    psx_write_word(gpu, 0u);
    psx_write_half(gpu + 4u, 0u);
    psx_write_byte(gpu + 6u, 0u);

    CHECK("snapshot read accepts exact wire size",
          memory_mod_snapshot_read(snapshot, bytes));
    CHECK("guest word restored", psx_read_word(guest) == 0x11223344u);
    CHECK("guest half restored", psx_read_half(guest + 4u) == 0x5566u);
    CHECK("guest byte restored", psx_read_byte(guest + 6u) == 0x77u);
    CHECK("gpu dma word restored", psx_read_word(gpu) == 0x89ABCDEFu);
    CHECK("gpu dma half restored", psx_read_half(gpu + 4u) == 0xCAFEu);
    CHECK("gpu dma byte restored", psx_read_byte(gpu + 6u) == 0x42u);

    next_guest = psx_mod_memory_alloc(16u, 16u);
    next_gpu = psx_mod_gpu_dma_memory_alloc(32u, 32u);
    CHECK("guest allocator cursor restored",
          next_guest == 0x9F000000u + 32u);
    CHECK("gpu dma allocator cursor restored",
          next_gpu == PSX_MOD_GPU_DMA_GUEST_BASE + 64u);

    CHECK("truncated snapshot rejects",
          !memory_mod_snapshot_read(snapshot, bytes - 1u));
    CHECK("older smaller snapshot rejects when current allocations are active",
          !memory_mod_snapshot_read(snapshot, bytes));

    free(snapshot);
    test_boot_state_restores_mod_dma_payload();
    test_legacy_missing_modmem_rejects_before_mutation();
    return failures ? 1 : 0;
}
