/* Straight-line interpretation into game text that dispatch refuses keeps
 * interpreting; it hands back at the first PC dispatch accepts.
 *
 * Layout (all NOPs): 0x40000-0x40FFF is a dirty page, where interpretation
 * starts; 0x60000-0x60FFF is a second dirty page, for the control.
 * 0x40000-0x4FFFF is game text. 0x40000-0x41FFF holds live bytes that differ
 * from the boot EXE, so dispatch refuses it (psx_game_text_native_ok is
 * false); from 0x42000 on the text matches again and runs compiled.
 *
 * Before the change the interpreter handed back at 0x41000, the first clean
 * page. Dispatch refused that PC and re-entered the interpreter, which handed
 * back again one instruction later: about 1,000 hand-backs to cross one page.
 * Now it interprets through the refused text, hands back once, and dispatch
 * runs the compiled code at 0x42000.
 *
 * Production dirty_ram_interp.c (test_interp_refused_game_text.py links it);
 * memory and dispatch answers are seams, everything else aborts. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"

/* No mod hooks are active in this interpreter fixture. */
uint32_t g_psx_mod_instruction_hooks = 0;
uint32_t g_psx_mod_function_entry_hooks = 0;
int g_psx_mod_immediate_live = 0;

int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr);
extern uint64_t g_dirty_ram_blocks_run;   /* one per interpreter hand-back */

uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;
static uint8_t ram[0x00200000u];                      /* zero = NOP */
uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_read_word(uint32_t a) { (void)a; return 0; }

int dirty_ram_is_dirty(uint32_t phys) {
    const uint32_t page = (phys & 0x1FFFFFFFu) >> 12;
    return page == 0x40u || page == 0x60u;
}
int psx_game_address_in_text(uint32_t addr) {
    const uint32_t p = addr & 0x1FFFFFFFu;
    return p >= 0x40000u && p < 0x50000u;
}
int psx_game_text_native_ok(uint32_t addr) {
    return psx_game_address_in_text(addr) && ((addr & 0x1FFFFFFFu) >> 12) >= 0x42u;
}
int psx_game_text_native_ok_full(uint32_t addr) { return psx_game_text_native_ok(addr); }
int psx_is_dispatchable(uint32_t pc) { (void)pc; return 1; }

static unsigned s_compiled_calls;
static uint32_t s_compiled_addr;
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr) {
    (void)cpu;
    ++s_compiled_calls;
    s_compiled_addr = addr;
    return 1;                                         /* handled */
}

/* No interrupt is pending; no kernel slot, overlay, mod hook or thread switch
 * applies on this path. */
int psx_get_in_exception(void) { return 0; }
void psx_check_interrupts(CPUState *cpu) { (void)cpu; }
void psx_check_interrupts_at(CPUState *cpu, uint32_t resume_pc) { (void)cpu; (void)resume_pc; }
int psx_interrupts_checked_at_current_cycle(uint32_t pc) { (void)pc; return 1; }
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
/* [[draw_distance.clamp]] is off: fixture_link would stub this data symbol as a
 * function, whose code bytes read as non-zero and run the clamp lookup. */
int g_psx_draw_distance_clamp_live = 0;
/* The segment-miss recorder sees every clean game-text miss; nothing to record. */
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

static int failures;

/* Interprets from `start`; checks the hand-back count and what dispatch ran. */
static void run_from(uint32_t start, unsigned expect_handbacks,
                     uint32_t expect_compiled, uint32_t expect_pc) {
    CPUState cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.read_word = psx_read_word;
    cpu.pc = start;
    s_compiled_calls = 0;
    s_compiled_addr = 0;
    const uint64_t blocks = g_dirty_ram_blocks_run;
    dirty_ram_dispatch(&cpu, start, 0);
    const unsigned handbacks = (unsigned)(g_dirty_ram_blocks_run - blocks);
    printf("start=%08X hand-backs=%u compiled=%u@%08X pc=%08X\n", (unsigned)start,
           handbacks, s_compiled_calls, (unsigned)s_compiled_addr, (unsigned)cpu.pc);
    if (handbacks != expect_handbacks) {
        fprintf(stderr, "FAIL: from %08X: %u hand-backs, expected %u\n",
                (unsigned)start, handbacks, expect_handbacks);
        ++failures;
    }
    if (s_compiled_calls != (expect_compiled ? 1u : 0u) || s_compiled_addr != expect_compiled) {
        fprintf(stderr, "FAIL: from %08X: compiled dispatch %u@%08X, expected %08X\n",
                (unsigned)start, s_compiled_calls, (unsigned)s_compiled_addr,
                (unsigned)expect_compiled);
        ++failures;
    }
    if (expect_pc && cpu.pc != expect_pc) {
        fprintf(stderr, "FAIL: from %08X: pc %08X, expected %08X\n",
                (unsigned)start, (unsigned)cpu.pc, (unsigned)expect_pc);
        ++failures;
    }
}

int main(void) {
    /* Through refused text: one hand-back, then compiled code at 0x42000. */
    run_from(0x80040FF0u, 1u, 0x80042000u, 0u);
    /* Control: a clean page outside game text is handed back at once. */
    run_from(0x80060FF0u, 1u, 0u, 0x80061000u);
    if (failures) return 1;
    puts("PASS: interpretation runs through refused game text to an accepted page");
    return 0;
}
