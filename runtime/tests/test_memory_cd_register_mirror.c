/* The CD-ROM register block answers at 1F801804, 08 and 0C as well as at
 * 1F801800-1F801803: the four byte registers repeat across 1F801800-180F,
 * through the production memory map. The word reads of 1F801804, 08 and 0C
 * come from authored test programs run on a reference emulator, where each
 * returns what 1F801800 returns; byte accesses and writes are checked for the
 * same address decode.
 * GCC -O2 -fwhole-program removes unrelated device dependencies. */
#include "../src/memory.c"

void psx_devices_mmio_sync(void) {}
void debug_server_trace_mmio_read(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
void debug_server_trace_mmio_write(uint32_t a, uint32_t v, uint8_t w) { (void)a; (void)v; (void)w; }
static uint32_t cd_last_read, cd_last_write, cd_last_value;
uint32_t cdrom_read(uint32_t addr) { cd_last_read = addr; return addr == 0x1F801800u ? 0x18u : 0x5Au; }
void cdrom_write(uint32_t addr, uint32_t val) { cd_last_write = addr; cd_last_value = val; }

/* Devices and tracing the memory map reaches but these cases do not use. */
uint32_t g_debug_current_func_addr, g_debug_last_store_pc;
int g_dma_exec_depth, g_ls_mode, g_ls_suppress_record, g_ram_read_watch_active;
volatile int g_ds_recording;
uint64_t g_psx_device_gen;
void (*g_overlay_flush_pending_cycles)(void);
void debug_server_trace_ram_read_watch(uint32_t p, uint32_t v) { (void)p; (void)v; }
void ds_note_dma_write(void) {}
void ds_note_read(uint32_t a, uint32_t s) { (void)a; (void)s; }
void ds_note_write(uint32_t a, uint32_t s) { (void)a; (void)s; }
int fntrace_is_game_started(void) { return 0; }
uint32_t ls_read_hook(uint32_t a, int s, uint32_t v) { (void)a; (void)s; return v; }
void ls_write_hook(uint32_t a, int s, uint32_t v) { (void)a; (void)s; (void)v; }
int psx_get_in_exception(void) { return 0; }
void psx_irq_refresh_cause_ip2(void) {}
void sio_card_handoff_on_imask(uint32_t o, uint32_t n) { (void)o; (void)n; }
void sio_tick(int c) { (void)c; }
uint32_t dma_read(uint32_t a) { (void)a; abort(); }
uint32_t sio_read(uint32_t a) { (void)a; abort(); }
uint32_t spu_read(uint32_t a) { (void)a; abort(); }
uint32_t timers_read(uint32_t a) { (void)a; abort(); }
uint32_t mdec_read(uint32_t a) { (void)a; abort(); }
uint32_t gpu_read_gpuread(void) { abort(); }
uint32_t gpu_read_gpustat(void) { abort(); }
void dma_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void dma_write_masked(uint32_t a, uint32_t v, uint32_t m) { (void)a; (void)v; (void)m; abort(); }
void mdec_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void spu_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void timers_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void sio_write(uint32_t a, uint32_t v) { (void)a; (void)v; abort(); }
void gpu_write_gp0(uint32_t v) { (void)v; abort(); }
void gpu_write_gp1(uint32_t v) { (void)v; abort(); }
void gpu_set_gp0_source(uint32_t a) { (void)a; abort(); }

/* Word reads of the mirrors in the authored programs: each returns the
 * status register (18h here) that 1F801800 returns. */
static const uint32_t mirror_words[] = { 0x1F801804u, 0x1F801808u, 0x1F80180Cu };

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

int main(void) {
    for (size_t i = 0; i < sizeof mirror_words / sizeof mirror_words[0]; ++i) {
        uint32_t a = mirror_words[i];
        uint32_t got = psx_read_word(0xA0000000u | a);
        CHECK(got == 0x18u, "word read %08X: %08X != 00000018", a, got);
    }
    for (uint32_t m = 0; m < 16; ++m) {
        uint32_t a = 0x1F801800u + m;
        cd_last_read = 0;
        (void)psx_read_byte(0xA0000000u | a);
        CHECK(cd_last_read == (0x1F801800u | (m & 3u)), "byte read %08X reached %08X", a, cd_last_read);
        cd_last_write = 0;
        psx_write_byte(0xA0000000u | a, (uint8_t)(0x40u + m));
        CHECK(cd_last_write == (0x1F801800u | (m & 3u)) && cd_last_value == 0x40u + m,
              "byte write %08X reached %08X", a, cd_last_write);
        if (m & 3u) continue;
        cd_last_write = 0;
        psx_write_word(0xA0000000u | a, 0x11u);
        CHECK(cd_last_write == 0x1F801800u, "word write %08X reached %08X", a, cd_last_write);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: the CD-ROM register block mirrors across 1F801800-180F");
    return 0;
}

/* No render pass is active; an isolated-cache store is outside these cases. */
int g_psx_render_pass_active;
int render_pass_mmio_read_allowed(uint32_t phys, uint32_t width) { (void)phys; (void)width; return 1; }
#include "render_pass_plan.h"
int render_pass_store_to(const RenderPassStoreTarget *target, uint32_t address,
                        uint32_t value, uint32_t width) {
    (void)target; (void)address; (void)value; (void)width; abort();
}
void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value) { (void)biu; (void)addr; (void)value; abort(); }

#include "../src/psx_ram_geometry.c"
