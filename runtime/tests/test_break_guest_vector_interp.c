/* A BREAK that the interpreter executes passes through the guest's own
 * exception vector and resumes at EPC+4 with the handler's result. Under the
 * BIOS's vector, and in a branch delay slot, it reaches psx_break as before.
 * Production dirty_ram_interp.c and psx_break_vector.h; memory, dispatch and
 * the fatal exit are seams. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cpu_state.h"
#include "psx_break_vector.h"

int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr);

#define MAIN   0x80010100u   /* ori t1 ; break 0xFB42 ; move v0,t0 ; jr ra ; nop */
#define SLOT   0x80010140u   /* jr ra ; break 1 (in the delay slot) */
#define HANDLER 0x80010200u
#define STOP   0x80012000u   /* return address: a page the interpreter does not own */

static uint8_t ram[0x200000];
static void poke(uint32_t addr, uint32_t word) { memcpy(ram + (addr & 0x1ffffcu), &word, 4); }
static uint32_t peek(uint32_t addr) { uint32_t w; memcpy(&w, ram + (addr & 0x1ffffcu), 4); return w; }

/* The exit that psx_break makes in production, observed instead of taken. */
static jmp_buf fatal;
static unsigned fatal_calls, rfe_calls;
static uint32_t fatal_code, fatal_pc;
void psx_break(CPUState *cpu, uint32_t code, uint32_t pc) {
    (void)cpu;
    ++fatal_calls; fatal_code = code; fatal_pc = pc;
    longjmp(fatal, 1);
}
int psx_break_enter_guest_vector(CPUState *cpu, uint32_t pc) {
    return psx_break_vector_enter(cpu, pc, 0);
}
void psx_rfe_mark_escape(void) { ++rfe_calls; }

uint8_t *memory_get_ram_ptr(void) { return ram; }
uint32_t psx_read_word(uint32_t a) { return peek(a); }
/* The vector page and the page of the fixture run in the interpreter. */
int dirty_ram_is_dirty(uint32_t phys) { return (phys >> 12) == 0u || (phys >> 12) == 0x10u; }
int psx_is_dispatchable(uint32_t pc) { (void)pc; return 1; }
int psx_game_address_in_text(uint32_t addr) { (void)addr; return 0; }
int psx_game_text_native_ok(uint32_t addr) { (void)addr; return 0; }
int psx_game_text_native_ok_full(uint32_t addr) { (void)addr; return 0; }
int psx_dispatch_game_compiled(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin) { (void)site; (void)target; (void)origin; }
/* Observers and overlay tiers on the path under test; inert here. */
void fntrace_maybe_mark_game_started(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; }
int overlay_fp_enabled(void) { return 0; }
int overlay_loader_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int overlay_loader_is_candidate(uint32_t phys) { (void)phys; return 0; }
int psx_overlay_dispatch(CPUState *cpu, uint32_t addr) { (void)cpu; (void)addr; return 0; }
int psx_mod_function_entry(CPUState *cpu, uint32_t address) { (void)cpu; (void)address; return 0; }
int psx_mod_try_function_replacement(CPUState *cpu, uint32_t address) { (void)cpu; (void)address; return 0; }
/* No mod hook or draw-distance clamp is live. The fixture linker would stub
 * these data symbols as functions, whose code bytes read as non-zero. */
uint32_t g_psx_mod_instruction_hooks = 0;
uint32_t g_psx_mod_function_entry_hooks = 0;
int g_psx_draw_distance_clamp_live = 0;
int g_psx_mod_immediate_live = 0;
int g_psx_ws_sxy_cull_n = 0;
int g_ws_bd_from_interp = 0;
uint32_t g_ws_backdrop_lo, g_ws_backdrop_hi;
int g_rfe_escape_pending = 0, g_exc_escape_reason = 0;
int g_psx_call_bail = 0, g_psx_dispatch_depth = 0;
uint64_t g_psx_bail_first, g_psx_bail_resolved, g_dispatch_static_hits;
uint32_t g_debug_current_func_addr, g_debug_last_store_pc, g_dirty_ram_code_gen;
uint32_t g_psx_ram_size = 0x00200000u;
uint32_t g_psx_ram_mask = 0x001FFFFFu;
uint64_t s_frame_count;
int psx_game_is_function_entry(uint32_t addr) { (void)addr; return 0; }
uint32_t psx_segment_miss_note(uint32_t addr, int (*is_entry)(uint32_t),
                               uint32_t ra, uint32_t sp, uint32_t frame) {
    (void)addr; (void)is_entry; (void)ra; (void)sp; (void)frame; return 0;
}
void dirty_ram_mark_executable_range(uint32_t phys, uint32_t len) { (void)phys; (void)len; }
uint32_t psx_overlay_resident_crc_at(uint32_t phys, int *valid) { (void)phys; *valid = 0; return 0; }
int psx_get_in_exception(void) { return 0; }
/* No interrupt is pending and no kernel slot, overlay or thread switch applies. */
void psx_check_interrupts(CPUState *cpu) { (void)cpu; }
void psx_check_interrupts_at(CPUState *cpu, uint32_t resume_pc) { (void)cpu; (void)resume_pc; }
void psx_rfe_escape_check(CPUState *cpu) { (void)cpu; }
int psx_deferred_switch_is_pending(void) { return 0; }
int psx_kernel_bless_dispatchable(uint32_t phys) { (void)phys; return 0; }
int psx_kernel_patch_range_ends_at(uint32_t phys) { (void)phys; return 0; }
int psx_overlay_static_can_dispatch(uint32_t addr) { (void)addr; return 0; }
void psx_ra_tripwire(CPUState *cpu, uint32_t a, uint32_t b, uint32_t c) { (void)cpu; (void)a; (void)b; (void)c; }
int psx_interrupts_checked_at_current_cycle(uint32_t resume_pc) { (void)resume_pc; return 1; }
void psx_pgxp_alu(CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2) {
    (void)cpu; (void)instr; (void)result; (void)s1; (void)s2;
}
void psx_pgxp_load(CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value) {
    (void)cpu; (void)instr; (void)addr; (void)value;
}
/* No widescreen site is declared. */
int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t instr) { (void)pc; (void)instr; return 0; }
int psx_ws_is_cull_plane_nx_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_xclip_load_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_bias_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_depth_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_angle_site(uint32_t pc, uint32_t instr, uint32_t *out) { (void)pc; (void)instr; (void)out; return 0; }
int psx_ws_backdrop_preload(void) { return 0; }
int psx_ws_auto_cull_on(void) { return 0; }
int psx_ws_is_cull_bias_lower_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_clip_edge_x_load_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_local_viewport_width_site(uint32_t pc) { (void)pc; return 0; }
int psx_ws_is_cull_screen_x_site(uint32_t pc) { (void)pc; return 0; }

static void load_fixture(uint32_t v0, uint32_t v1, uint32_t v2) {
    static const uint32_t main_code[] = {
        0x34096f39u,   /* ori   t1, zero, 0x6f39 */
        0x003ed08du,   /* break 0xfb42 */
        0x01001021u,   /* move  v0, t0          (EPC+4) */
        0x03e00008u,   /* jr    ra */
        0x00000000u,
    };
    static const uint32_t slot_code[] = {
        0x03e00008u,   /* jr    ra */
        0x0000004du,   /* break 1               (delay slot) */
    };
    /* The handler answers code 9 only: t0 = (break code ^ 0x2531) >> 1, then
     * it returns to EPC+4 with rfe in the delay slot. s0 keeps the code. */
    static const uint32_t handler[] = {
        0x401a6800u,   /* mfc0  k0, Cause */
        0x00000000u,
        0x001ad082u,   /* srl   k0, k0, 2 */
        0x335a001fu,   /* andi  k0, k0, 0x1f */
        0x03408021u,   /* move  s0, k0 */
        0x401a7000u,   /* mfc0  k0, EPC */
        0x00000000u,
        0x8f5b0000u,   /* lw    k1, 0(k0) */
        0x00000000u,
        0x001bd980u,   /* sll   k1, k1, 6 */
        0x001bdb02u,   /* srl   k1, k1, 12 */
        0x3b682531u,   /* xori  t0, k1, 0x2531 */
        0x00084042u,   /* srl   t0, t0, 1 */
        0x275a0004u,   /* addiu k0, k0, 4 */
        0x03400008u,   /* jr    k0 */
        0x42000010u,   /* rfe */
    };
    memset(ram, 0, sizeof ram);
    for (unsigned i = 0; i < sizeof main_code / 4; i++) poke(MAIN + 4 * i, main_code[i]);
    for (unsigned i = 0; i < sizeof slot_code / 4; i++) poke(SLOT + 4 * i, slot_code[i]);
    for (unsigned i = 0; i < sizeof handler / 4; i++) poke(HANDLER + 4 * i, handler[i]);
    poke(0x80000080u, v0); poke(0x80000084u, v1); poke(0x80000088u, v2);
    fatal_calls = rfe_calls = 0; fatal_code = fatal_pc = 0;
}

/* Runs from `entry` until the code returns to STOP. Returns 1 when the run
 * ended in psx_break instead. */
static int run(CPUState *cpu, uint32_t entry) {
    memset(cpu, 0, sizeof *cpu);
    cpu->read_word = psx_read_word;
    cpu->cop0[12] = 0x40000401u;
    cpu->cop0[14] = 0x11111111u;
    cpu->gpr[8] = 0xdeadbeefu;          /* t0: only the handler makes it 0x6f39 */
    cpu->gpr[31] = STOP;
    cpu->pc = entry;
    if (setjmp(fatal)) return 1;
    for (int hop = 0; hop < 16 && cpu->pc != STOP && cpu->pc != 0; hop++)
        dirty_ram_dispatch(cpu, cpu->pc, STOP);
    return 0;
}

static int fail(const char *what, uint32_t got, uint32_t want) {
    printf("FAIL: %s: got 0x%08X, wanted 0x%08X\n", what, (unsigned)got, (unsigned)want);
    return 1;
}
#define CHECK(what, got, want) do { if ((uint32_t)(got) != (uint32_t)(want)) bad += fail(what, (uint32_t)(got), (uint32_t)(want)); } while (0)

int main(void) {
    CPUState cpu;
    int bad = 0;

    /* 1. The guest owns the vector: lui k0,0x8001 ; ori k0,k0,0x0200 ; jr k0. */
    load_fixture(0x3c1a8001u, 0x375a0200u, 0x03400008u);
    CHECK("guest vector: ended in psx_break", run(&cpu, MAIN), 0);
    CHECK("guest vector: pc", cpu.pc, STOP);
    CHECK("guest vector: v0 at EPC+4 (the handler's t0)", cpu.gpr[2], 0x6f39u);
    CHECK("guest vector: t1", cpu.gpr[9], 0x6f39u);
    CHECK("guest vector: code seen by the handler", cpu.gpr[16], 9u);
    CHECK("guest vector: EPC", cpu.cop0[14], MAIN + 4u);
    CHECK("guest vector: SR after rfe", cpu.cop0[12], 0x40000401u);
    CHECK("guest vector: rfe count", rfe_calls, 1u);
    CHECK("guest vector: psx_break calls", fatal_calls, 0u);

    /* 2. The BIOS's own stub: lui k0,0 ; addiu k0,k0,0xc80 ; jr k0. */
    load_fixture(0x3c1a0000u, 0x275a0c80u, 0x03400008u);
    CHECK("BIOS vector: ended in psx_break", run(&cpu, MAIN), 1);
    CHECK("BIOS vector: psx_break calls", fatal_calls, 1u);
    CHECK("BIOS vector: code", fatal_code, 0xfb42u);
    CHECK("BIOS vector: pc", fatal_pc, MAIN + 4u);
    CHECK("BIOS vector: EPC untouched", cpu.cop0[14], 0x11111111u);
    CHECK("BIOS vector: SR untouched", cpu.cop0[12], 0x40000401u);

    /* 3. A BREAK in a branch delay slot keeps the exit, guest vector or not. */
    load_fixture(0x3c1a8001u, 0x375a0200u, 0x03400008u);
    CHECK("delay slot: ended in psx_break", run(&cpu, SLOT), 1);
    CHECK("delay slot: psx_break calls", fatal_calls, 1u);
    CHECK("delay slot: pc", fatal_pc, SLOT + 4u);
    CHECK("delay slot: EPC untouched", cpu.cop0[14], 0x11111111u);

    if (bad) return 1;
    puts("PASS: an interpreted BREAK passes through the guest's vector and resumes at EPC+4");
    return 0;
}
