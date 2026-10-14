/* Link fixture for restore_edge_irq_replay_test: the device, debug, mod and
 * host collaborators of the REAL traps.c (scheduler) and interrupts.c (IRQ
 * delivery). Everything the scheduler/IRQ decision reads that matters to the
 * test (cycle clock, I_STAT/I_MASK, guest RAM, the null-PC recovery hook) is
 * owned by the harness through the hooks declared at the bottom; the rest is
 * inert diagnostics or devices with no pending work. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CPUState;

/* ---- Guest state owned by the fixture (memory.c / psx_cycles.c in the runtime) ---- */
uint8_t  g_test_ram[0x200000];
uint64_t psx_cycle_count = 0;
uint64_t psx_next_service_cycle = 0;
uint32_t i_stat = 0;
uint32_t i_mask = 0;
uint32_t g_psx_ram_size = 0x200000u;
uint32_t g_psx_vblank_cycles = 564480u;

uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
void psx_advance_cycles_slow(uint32_t cycles) { psx_cycle_count += cycles; }

uint32_t psx_read_word(uint32_t addr)
{
    uint32_t v;
    memcpy(&v, &g_test_ram[addr & 0x1FFFFCu], 4);
    return v;
}
void psx_host_write_word(uint32_t addr, uint32_t value)
{
    memcpy(&g_test_ram[addr & 0x1FFFFCu], &value, 4);
}
uint8_t *memory_get_ram_ptr(void) { return g_test_ram; }

/* Null-PC recovery (psx_netplay_rb.c): the harness decides. */
int (*g_test_recover_null_pc)(struct CPUState *, uint32_t *) = NULL;
int psx_netplay_rb_recover_null_pc(struct CPUState *c, uint32_t *out_pc)
{
    return g_test_recover_null_pc ? g_test_recover_null_pc(c, out_pc) : 0;
}

/* ---- Runtime globals the two units reference ---- */
uint64_t g_async_rfe_fire_count = 0;
uint32_t g_async_rfe_resume_pc = 0;
int      g_call_unit_depth = 0;
int      g_cosim_dirty_pump_site = 0;
int      g_dirty_interp_active = 0;
uint32_t g_dirty_safe_resume_pc = 0;
int      g_dma_exec_depth = 0;
int      g_event_step_conservative = 0;
int      g_exec_phase = 0;
int      g_idle_skip_enabled = 0;
int      g_ls_replay_active = 0;
int      g_ls_suppress_record = 0;
uint32_t g_psx_cyc_batch = 0;
uint32_t g_psx_cyc_batch_limit = 0;
int      g_psx_cyc_bb_defer = 0;
uint32_t *g_psx_cyc_local_acc = NULL;
int      g_psx_dispatch_depth = 0;
int      g_psx_guest_time_frozen = 0;
uint32_t g_sentinel_reach_async = 0;
uint64_t g_sentinel_reach_traps = 0;
volatile int g_sio_timing_active = 0;
int      psx_in_device_service = 0;
uint64_t s_frame_count = 0;
struct CPUState *debug_cpu_ptr = NULL;

/* ---- Devices: nothing scheduled, nothing pending ---- */
uint32_t cdrom_cycles_to_irq(uint32_t m) { (void)m; return 0x7FFFFFFFu; }
uint32_t dma_cycles_to_irq(uint32_t m)   { (void)m; return 0x7FFFFFFFu; }
uint32_t sio_cycles_to_irq(uint32_t m)   { (void)m; return 0x7FFFFFFFu; }
uint32_t timers_cycles_to_irq(uint32_t m){ (void)m; return 0x7FFFFFFFu; }
void cdrom_tick(void) {}
void timers_tick(int c) { (void)c; }
void sio_tick_quantum(void) {}
uint32_t sio_get_seq(void) { return 0; }
int  sio_card_protocol_active(void) { return 0; }
void gpu_vblank_tick(void) {}
void gpu_vblank_flush_present(void) {}
void gpu_vblank_clear_deferred_present(void) {}
void psx_guest_cycle_scale_vblank(void) {}
void psx_devices_service_to_now(void) {}
int  dma_cdrom_transfer_active(void) { return 0; }
uint16_t spu_ctrl_read(void) { return 0; }
int  psx_idle_skip_is_enabled(void) { return 0; }
void psx_idle_note_check(struct CPUState *c, uint32_t pc) { (void)c; (void)pc; }

/* ---- Host features that are off in this fixture ---- */
int psx_netplay_active(void) { return 0; }
int psx_selfcheck_enabled(void) { return 0; }
int psx_mod_function_entry_active(void) { return 0; }
void savestate_poll(struct CPUState *c, uint32_t pc) { (void)c; (void)pc; }
void psx_netplay_poll_snap(struct CPUState *c, uint32_t pc) { (void)c; (void)pc; }
void psx_selfcheck_poll(struct CPUState *c, uint32_t pc) { (void)c; (void)pc; }
void psx_rewind_poll(struct CPUState *c, uint32_t pc) { (void)c; (void)pc; }
void mod_runtime_function_entry_context_save(void *ctx) { (void)ctx; }
void mod_runtime_function_entry_context_restore(const void *ctx) { (void)ctx; }
int  overlay_loader_shadow_native_thread_switch_bail(void) { return 0; }
void overlay_loader_shadow_escape_fixup(uint64_t epoch) { (void)epoch; }
void overlay_loader_shadow_scheduler_escape_fixup(void) {}
void ls_note_exception_entry(void) {}

/* Host fibers: only the LLE scheduler uses them; the HLE scheduler is default. */
void *psx_fiber_current(void) { return NULL; }
void *psx_fiber_convert_thread(void) { return NULL; }
void *psx_fiber_create(void *fn, void *arg) { (void)fn; (void)arg; return NULL; }
void psx_fiber_destroy(void *f) { (void)f; }
void psx_fiber_switch(void *f) { (void)f; }

/* ---- Diagnostics ---- */
void debug_server_poll(void) {}
void debug_server_log_restore_event(uint32_t k, uint32_t a, uint32_t b) { (void)k; (void)a; (void)b; }
void debug_server_log_thread_event(int k, struct CPUState *c, uint32_t a, uint32_t b, uint32_t d)
{ (void)k; (void)c; (void)a; (void)b; (void)d; }
void ds_note_thread_switch(void) {}
void device_trace_note(uint32_t s, uint32_t d) { (void)s; (void)d; }
void event_ring_record(uint16_t k, uint8_t d) { (void)k; (void)d; }
void event_ring_record_aux(uint16_t k, uint8_t d, uint32_t a) { (void)k; (void)d; (void)a; }
int  parity_trace_is_armed(void) { return 0; }
void parity_trace_record(void) {}
uint32_t crash_trace_dispatch_ring_get(int i) { (void)i; return 0; }
uint64_t crash_trace_dispatch_seq_get(void) { return 0; }
void psx_unknown_dispatch_record(uint32_t a, uint32_t p) { (void)a; (void)p; }
void psx_crash_trace_dump(const char *r, void *i) { (void)i; fprintf(stderr, "crash: %s\n", r); }
void psx_fatal_halt(const char *r) { fprintf(stderr, "FATAL: %s\n", r); exit(2); }
