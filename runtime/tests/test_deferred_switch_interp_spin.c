/* An interpreted thread that spins in one local loop still lets a deferred
 * in-exception thread switch be taken.
 *
 * When an interrupt handler switches threads while the switch cannot be taken
 * at once, interrupts.c defers it to the next site-0 poll: an interrupt check
 * at a fully materialized boundary, which in the dirty-RAM interpreter is its
 * entry poll (psx_check_interrupts_at). A thread interpreted from dirty RAM
 * that spins in one local loop (a disc-loaded module polling a stub that never
 * changes, as Metal Gear Solid PAL does at boot) never leaves the interpreter,
 * so that poll never comes: the switch stays deferred and every other thread
 * starves.
 *
 * The guest here is `b .` at 0x80040000 in a dirty page. A switch to a thread
 * at 0x80060000 is pending from the start. The fixture's site-0 poll takes it;
 * the pump-site check (psx_check_interrupts) does not, as in the runtime. The
 * driver re-enters dirty_ram_dispatch the way the flat dispatcher does, at
 * most four times. The switch must be taken within two entries and a handful
 * of interpreted instructions.
 *
 * Production dirty_ram_interp.c (test_deferred_switch_interp_spin.py links
 * it); any other seam it reaches fails the run. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"

uint32_t g_psx_mod_instruction_hooks = 0;
uint32_t g_psx_mod_function_entry_hooks = 0;
int g_psx_draw_distance_clamp_live = 0;
/* No mod immediates or function overrides are installed. */
int g_psx_mod_immediate_live = 0;
int (*g_psx_func_override_hook)(CPUState *cpu, uint32_t addr) = 0;
/* Runtime state the interpreter reads: idle, nothing raised or pending. */
uint32_t g_debug_current_func_addr, g_debug_last_store_pc;
uint32_t g_dirty_ram_code_gen = 1;
uint64_t g_dispatch_static_hits, g_psx_bail_first, g_psx_bail_resolved;
int g_exc_escape_reason, g_psx_call_bail, g_psx_dispatch_depth, g_rfe_escape_pending;
int g_psx_ws_sxy_cull_n, g_ws_bd_from_interp;
uint32_t g_ws_backdrop_lo, g_ws_backdrop_hi;
uint32_t i_stat, i_mask;
uint64_t psx_cycle_count;

int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr);
extern uint64_t g_dirty_ram_insns_run;

#define SPIN_PC 0x80040000u
#define OTHER_THREAD_PC 0x80060000u
#define MAX_ENTRIES 4

uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;
static uint8_t ram[0x00200000u];
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_read_word(uint32_t a) {
    uint32_t v;
    memcpy(&v, ram + (a & 0x1FFFFCu), 4);
    return v;
}

int dirty_ram_is_dirty(uint32_t phys) { return ((phys & 0x1FFFFFFFu) >> 12) == 0x40u; }
int psx_game_address_in_text(uint32_t addr) { (void)addr; return 0; }
int psx_game_text_native_ok(uint32_t addr) { (void)addr; return 0; }
int psx_game_text_native_ok_full(uint32_t addr) { (void)addr; return 0; }
int psx_is_dispatchable(uint32_t pc) { (void)pc; return 1; }
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }

/* The deferred switch: pending until a site-0 poll takes it. */
static int s_switch_pending;
static unsigned s_site0_polls, s_pump_checks;
int psx_defer_switch_pending(void) { return s_switch_pending; }
int psx_get_in_exception(void) { return 0; }
void psx_check_interrupts(CPUState *cpu) { (void)cpu; ++s_pump_checks; }
void psx_check_interrupts_at(CPUState *cpu, uint32_t resume_pc) {
    ++s_site0_polls;
    if (s_switch_pending && (resume_pc & 0x1FFFFFFFu) == (SPIN_PC & 0x1FFFFFFFu)) {
        s_switch_pending = 0;
        cpu->pc = OTHER_THREAD_PC;            /* the scheduler resumes the other thread */
    }
}
int psx_interrupts_checked_at_current_cycle(uint32_t pc) { (void)pc; return 0; }
void psx_rfe_escape_check(CPUState *cpu) { (void)cpu; }
int psx_kernel_bless_dispatchable(uint32_t phys) { (void)phys; return 0; }
int psx_kernel_patch_range_ends_at(uint32_t phys) { (void)phys; return 0; }
void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len) { (void)phys; (void)len; }
void fntrace_maybe_mark_game_started(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
int overlay_fp_enabled(void) { return 0; }
int overlay_loader_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int overlay_loader_is_candidate(uint32_t phys) { (void)phys; return 0; }
int psx_overlay_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int psx_overlay_static_can_dispatch(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_overlay_resident_crc_at(uint32_t phys, int *valid) {
    (void)phys; *valid = 0; return 0;
}
int psx_mod_function_entry(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int psx_mod_try_function_replacement(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
/* No widescreen site is installed: every site predicate says no. */
#define NO_WS_SITE(name) int name(uint32_t pc) { (void)pc; return 0; }
NO_WS_SITE(psx_ws_is_backdrop_site)
NO_WS_SITE(psx_ws_is_cull_bgez_site)
NO_WS_SITE(psx_ws_is_cull_bias_lower_site)
NO_WS_SITE(psx_ws_is_cull_bias_site)
NO_WS_SITE(psx_ws_is_cull_bltz_site)
NO_WS_SITE(psx_ws_is_cull_branch_keep_site)
NO_WS_SITE(psx_ws_is_cull_clip_edge_x_load_site)
NO_WS_SITE(psx_ws_is_cull_depth_site)
NO_WS_SITE(psx_ws_is_cull_negsub_site)
NO_WS_SITE(psx_ws_is_cull_plane_nx_site)
NO_WS_SITE(psx_ws_is_cull_range_site)
NO_WS_SITE(psx_ws_is_cull_screen_x_site)
NO_WS_SITE(psx_ws_is_cull_slti_lower_site)
NO_WS_SITE(psx_ws_is_cull_slti_site)
NO_WS_SITE(psx_ws_is_cull_vxrange_site)
NO_WS_SITE(psx_ws_is_cull_w_imm)
NO_WS_SITE(psx_ws_is_cull_xclip_load_site)
NO_WS_SITE(psx_ws_is_local_viewport_width_site)
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t instr) { (void)pc; (void)instr; return 0; }
void debug_server_poll(void) {}
void debug_server_wait_if_paused(void) {}
void psx_fatal_halt(const char *reason) {
    fprintf(stderr, "FAIL: psx_fatal_halt: %s\n", reason);
    exit(2);
}
int psx_game_is_function_entry(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_segment_miss_note(uint32_t addr, int (*is_entry)(uint32_t),
                               uint32_t ra, uint32_t sp, uint32_t frame) {
    (void)addr; (void)is_entry; (void)ra; (void)sp; (void)frame; return 0;
}
uint64_t s_frame_count;
void psx_ra_tripwire(CPUState *cpu, uint32_t a, uint32_t b, uint32_t c) {
    (void)cpu; (void)a; (void)b; (void)c;
}
void psx_pgxp_alu(CPUState *cpu, uint32_t insn, uint32_t r, uint32_t a, uint32_t b) {
    (void)cpu; (void)insn; (void)r; (void)a; (void)b;
}

static void put_word(uint32_t addr, uint32_t word) { memcpy(ram + (addr & 0x1FFFFCu), &word, 4); }

int main(void) {
    put_word(SPIN_PC, 0x1000FFFFu);           /* b . (beq zero, zero, -1) */
    put_word(SPIN_PC + 4u, 0x00000000u);      /* nop (delay slot) */

    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.read_word = psx_read_word;
    cpu.pc = SPIN_PC;
    s_switch_pending = 1;

    const uint64_t insns0 = g_dirty_ram_insns_run;
    int entries = 0;
    while (entries < MAX_ENTRIES && (cpu.pc & 0x1FFFFFFFu) != (OTHER_THREAD_PC & 0x1FFFFFFFu)) {
        dirty_ram_dispatch(&cpu, cpu.pc, 0);
        ++entries;
    }
    const uint64_t insns = g_dirty_ram_insns_run - insns0;
    printf("entries=%d interpreted=%llu site0_polls=%u pump_checks=%u pc=%08X pending=%d\n",
           entries, (unsigned long long)insns, s_site0_polls, s_pump_checks,
           (unsigned)cpu.pc, s_switch_pending);

    if (s_switch_pending || (cpu.pc & 0x1FFFFFFFu) != (OTHER_THREAD_PC & 0x1FFFFFFFu)) {
        fprintf(stderr, "FAIL: the deferred switch was not taken: the spinning thread ran "
                "%llu instructions in %d entries\n", (unsigned long long)insns, entries);
        return 1;
    }
    if (entries > 2 || insns > 16u) {
        fprintf(stderr, "FAIL: the deferred switch was taken late: %d entries, %llu instructions\n",
                entries, (unsigned long long)insns);
        return 1;
    }
    puts("PASS: a deferred thread switch is taken while an interpreted thread spins");
    return 0;
}
