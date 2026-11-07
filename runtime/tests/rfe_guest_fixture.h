/* Synthetic guest RAM, TCB and flat-dispatch fixture for exception returns. */
#ifndef RFE_GUEST_FIXTURE_H
#define RFE_GUEST_FIXTURE_H

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_state.h"
#include "interrupts.h"
#include "psx_bios_backend.h"
#include "psx_scheduler.h"
#include "mod_runtime.h"

/* A guest file may define FX_FAIL before it includes this header; the kernel
 * and dispatch code below then report through the guest's own macro. */
#ifndef FX_FAIL
#define FX_FAIL(...) do { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); \
    exit(1); \
} while (0)
#endif
#define FX_CHECK(cond) do { if (!(cond)) FX_FAIL("%s", #cond); } while (0)

/* ---- guest memory: 2 MB of RAM, every mirror ---- */
static uint32_t fx_ram[0x200000u / 4u];
static uint32_t fx_read(uint32_t a) { return fx_ram[(a & 0x1FFFFCu) / 4u]; }
static void fx_write(uint32_t a, uint32_t v) { fx_ram[(a & 0x1FFFFCu) / 4u] = v; }
uint32_t psx_read_word(uint32_t a) { return fx_read(a); }
uint8_t *memory_get_ram_ptr(void) { return (uint8_t *)fx_ram; }
int psx_peek_word(uint32_t addr, uint32_t *out) {
    uint32_t phys=addr&0x1FFFFFFFu;
    if (!out || (addr&3u) || phys>=sizeof fx_ram) return 0;
    *out=fx_ram[phys/4u]; return 1;
}
int memory_peek_instruction_word(uint32_t a, uint32_t *value) {
    *value = fx_read(a);
    return 1;
}

/* ---- kernel tables ---- */
#define FX_PCB        0x0000E000u /* [0x108]: process control block */
#define FX_TCB        0x0000E1F4u /* [0x110]: the one thread */
#define FX_TCB_BYTES  0x000000C0u /* [0x114] */
#define FX_TCB_REG(i) (FX_TCB + 0x08u + 4u * (uint32_t)(i))
#define FX_TCB_EPC    (FX_TCB + 0x88u)
#define FX_TCB_HI     (FX_TCB + 0x8Cu)
#define FX_TCB_LO     (FX_TCB + 0x90u)
#define FX_TCB_SR     (FX_TCB + 0x94u)
#define FX_TCB_CAUSE  (FX_TCB + 0x98u)

#define FX_VECTOR     0x80000080u /* exception vector */
#define FX_K_RET      0x80000E10u /* kernel: after the handler call */
#define FX_K_RFE      0x80000F40u /* kernel: B0(17h) ReturnFromException */
#define FX_K_STACK    0x000085D8u /* kernel exception stack */

/* ---- machine state the production modules read ---- */
uint32_t i_stat, i_mask;
uint64_t psx_cycle_count;
uint64_t psx_next_service_cycle;
uint64_t g_psx_cycle_fast_limit;
uint64_t g_psx_device_gen;
uint64_t s_frame_count;
uint64_t g_async_rfe_fire_count;
uint64_t g_sentinel_reach_traps;
uint32_t g_async_rfe_resume_pc;
uint32_t g_sentinel_reach_async;
uint32_t g_dirty_safe_resume_pc;
uint32_t g_psx_cyc_batch;
uint32_t g_psx_cyc_batch_limit;
uint32_t g_psx_gcs_recip_q16 = 65536u, g_psx_gcs_frac;
uint32_t g_psx_mod_instruction_hooks, g_psx_mod_function_entry_hooks;
int g_psx_mod_immediate_live;
int (*g_psx_func_override_hook)(CPUState *, uint32_t);
uint64_t g_vblank_ack_count;
uint32_t g_debug_current_func_addr;
int g_psx_guest_time_frozen, g_psx_render_pass_active;
int g_psx_savestate_pending;
uint32_t *g_psx_cyc_local_acc;
int g_psx_cyc_bb_defer;
int g_call_unit_depth;
int g_cosim_dirty_pump_site;
int g_dirty_interp_active;
int g_dma_exec_depth;
int g_event_step_conservative;
int g_exec_phase;
int g_idle_skip_enabled = 0;
int g_ls_replay_active;
int g_ls_suppress_record;
int g_precise_mode;
int psx_in_device_service;
volatile int g_sio_timing_active;
CPUState *debug_cpu_ptr;

uint64_t psx_get_cycle_count(void) { return psx_cycle_count; }
static int fx_fiber_token;
void *psx_fiber_current(void) { return &fx_fiber_token; }

/* Observers and device tiers on the delivery path; inert here. */
static ModFunctionEntryContext fx_mod_context;
void mod_runtime_function_entry_context_save(ModFunctionEntryContext *out) {
    *out = fx_mod_context;
}
void mod_runtime_function_entry_context_restore(const ModFunctionEntryContext *in) {
    fx_mod_context = *in;
}
void event_ring_record(uint16_t kind, uint8_t detail) { (void)kind; (void)detail; }
void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
    (void)kind; (void)detail; (void)aux;
}
void device_trace_note(uint32_t bit, uint32_t detail) { (void)bit; (void)detail; }
void debug_server_poll(void) {}
void debug_server_log_restore_event(uint32_t kind, uint32_t pc, uint32_t jmp) {
    (void)kind; (void)pc; (void)jmp;
}
void debug_server_log_thread_event(uint32_t kind, CPUState *cpu, uint32_t cur,
                                   uint32_t target, uint32_t pc) {
    (void)kind; (void)cpu; (void)cur; (void)target; (void)pc;
}
void psx_publish_note(uint32_t site, uint32_t target, uint32_t origin) {
    (void)site; (void)target; (void)origin;
}
void ls_note_exception_entry(void) {}
void psx_idle_note_check(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
int psx_idle_skip_is_enabled(void) { return 0; }
void psx_icache_fetch(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
void dirty_ram_checkpoint_enter(uint32_t pc, int slot, uint32_t target, int taken) {
    (void)pc; (void)slot; (void)target; (void)taken;
}
void dirty_ram_checkpoint_leave(void) {}
int dirty_ram_checkpoint_resume_pending(void) { return 0; }
int dirty_ram_is_dirty(uint32_t phys) { (void)phys; return 0; }
int dma_cdrom_transfer_active(void) { return 0; }
int source_gpu_runtime_active(void) { return 0; }
int psx_netplay_active(void) { return 0; }
int psx_selfcheck_enabled(void) { return 0; }
int psx_netplay_rb_recover_null_pc(CPUState *cpu, uint32_t *pc) {
    (void)cpu; (void)pc;
    return 0;
}
int parity_trace_is_armed(void) { return 0; }
void savestate_poll(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
void psx_netplay_poll_snap(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
void psx_selfcheck_poll(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
void psx_rewind_poll(CPUState *cpu, uint32_t pc) { (void)cpu; (void)pc; }
void overlay_loader_shadow_escape_fixup(uint64_t epoch) { (void)epoch; }
void overlay_loader_shadow_scheduler_escape_fixup(void) {}
int overlay_loader_shadow_native_thread_switch_bail(void) { return 0; }
void psx_bios_hle_configure(int call_hle, int boot_skip) { (void)call_hle; (void)boot_skip; }
uint32_t sio_get_seq(void) { return 0; }
int sio_card_protocol_active(void) { return 0; }
uint32_t timers_cycles_to_irq(uint32_t mask) { (void)mask; return 0xFFFFFFFFu; }
uint32_t cdrom_cycles_to_irq(uint32_t mask) { (void)mask; return 0xFFFFFFFFu; }
uint32_t dma_cycles_to_irq(uint32_t mask) { (void)mask; return 0xFFFFFFFFu; }
uint32_t sio_cycles_to_irq(uint32_t mask) { (void)mask; return 0xFFFFFFFFu; }
int gpu_video_standard_is_pal(void) { return 0; }
void psx_fatal_halt(const char *reason) { FX_FAIL("fatal halt: %s", reason); }

/* ---- dispatch: the generated trampoline's contract ---- */
typedef void (*FxGuestFn)(CPUState *);
typedef struct { uint32_t pc; FxGuestFn fn; } FxEntry;
static const FxEntry *fx_entries;
static unsigned fx_entry_count;
static int fx_max_dispatch_depth;
/* A depth past this is the defect under test, so stop before the host stack does. */
#define FX_DEPTH_LIMIT 64

static FxGuestFn fx_lookup(uint32_t pc) {
    for (unsigned i = 0; i < fx_entry_count; i++)
        if (((fx_entries[i].pc ^ pc) & 0x1FFFFFFFu) == 0) return fx_entries[i].fn;
    return NULL;
}

static void fx_dispatch_impl(CPUState *cpu, uint32_t addr, uint32_t stop_addr) {
    extern int g_psx_dispatch_depth;
    uint32_t sp_at_call = cpu->gpr[29];
    if (++g_psx_dispatch_depth > fx_max_dispatch_depth)
        fx_max_dispatch_depth = g_psx_dispatch_depth;
    if (g_psx_dispatch_depth > FX_DEPTH_LIMIT)
        FX_FAIL("host dispatch depth %d at guest PC 0x%08X: an exception return "
                "left host frames behind", g_psx_dispatch_depth, (unsigned)addr);
    for (;;) {
        FxGuestFn fn = fx_lookup(addr);
        if (!fn) FX_FAIL("dispatch to unmapped guest PC 0x%08X; vector=%08X/%08X "
                         "depth=%d EPC=%08X current_tcb=%08X saved_epc=%08X",
                         (unsigned)addr, fx_read(0x80u), fx_read(0x84u),
                         g_psx_dispatch_depth, cpu->cop0[14],
                         fx_read(fx_read(0x108u)),
                         fx_read(fx_read(fx_read(0x108u)) + 0x88u));
        cpu->pc = addr; /* the function's entry switch consumes it */
        fn(cpu);
        psx_rfe_escape_check(cpu);
        if (cpu->pc == 0) break;
        if (stop_addr != 0 && cpu->pc == stop_addr && cpu->gpr[29] == sp_at_call) {
            cpu->pc = 0;
            break;
        }
        addr = cpu->pc; /* tail call: re-dispatch */
    }
    --g_psx_dispatch_depth;
}
static void fx_dispatch(CPUState *cpu, uint32_t addr) { fx_dispatch_impl(cpu, addr, 0); }
static void fx_dispatch_call(CPUState *cpu, uint32_t addr, uint32_t ret) {
    fx_dispatch_impl(cpu, addr, ret);
}

static const PsxBiosImageInfo fx_image;
static const PsxBiosBackend fx_backend = {
    .image = &fx_image,
    .dispatch = fx_dispatch,
    .dispatch_call = fx_dispatch_call,
};
const PsxBiosBackend *const psx_bios_builtin_registry[] = { &fx_backend };
const uint32_t psx_bios_builtin_registry_count = 1;

/* ---- the kernel ---- */
void psx_irq_set_cause_ptr(uint32_t *cause); /* interrupts.c; main.cpp wires it */
static uint32_t fx_handler_pc;      /* the installed exception handler */
static uint64_t fx_exception_entries;

/* Exception vector: save the interrupted context in the current TCB, then call
 * the installed handler on the kernel stack. The call nests on the host, as a
 * kernel `jalr` to a native handler does. A handler that returns gets the
 * default exit, ReturnFromException. */
static void fx_k_exception(CPUState *cpu) {
    cpu->pc = 0;
    fx_exception_entries++;
    for (int i = 1; i < 32; i++) fx_write(FX_TCB_REG(i), cpu->gpr[i]);
    fx_write(FX_TCB_EPC, cpu->cop0[14]);
    fx_write(FX_TCB_HI, cpu->hi);
    fx_write(FX_TCB_LO, cpu->lo);
    fx_write(FX_TCB_SR, cpu->cop0[12]);
    fx_write(FX_TCB_CAUSE, cpu->cop0[13]);
    cpu->gpr[29] = FX_K_STACK;
    cpu->gpr[31] = FX_K_RET;
    psx_dispatch_call(cpu, fx_handler_pc, FX_K_RET);
    if (cpu->pc == 0) cpu->pc = FX_K_RFE;
}

/* B0(17h): reload every register from the current TCB, then `jr k0` with
 * `rfe` in the delay slot. */
static void fx_k_return_from_exception(CPUState *cpu) {
    cpu->pc = 0;
    for (int i = 1; i < 32; i++) cpu->gpr[i] = fx_read(FX_TCB_REG(i));
    cpu->hi = fx_read(FX_TCB_HI);
    cpu->lo = fx_read(FX_TCB_LO);
    cpu->cop0[12] = fx_read(FX_TCB_SR);
    cpu->gpr[26] = fx_read(FX_TCB_EPC);
    { uint32_t sr = cpu->cop0[12];
      cpu->cop0[12] = (sr & 0xFFFFFFC0u) | ((sr >> 2) & 0x0Fu); } /* rfe */
    psx_rfe_mark_escape();
    cpu->pc = cpu->gpr[26];
}

static void fx_k_ret(CPUState *cpu) { cpu->pc = FX_K_RFE; }

/* Boot: kernel tables, the one runnable thread and its first context. The
 * scheduler starts the thread from its TCB, as it resumes any thread. */
static void fx_boot(CPUState *cpu, const FxEntry *entries, unsigned count,
                    uint32_t handler_pc, uint32_t entry_pc, uint32_t sp, uint32_t ra) {
    fx_entries = entries;
    fx_entry_count = count;
    fx_handler_pc = handler_pc;
    FX_CHECK(psx_bios_activate(&fx_backend));
    memset(cpu, 0, sizeof(*cpu));
    cpu->read_word = fx_read;
    cpu->write_word = fx_write;
    debug_cpu_ptr = cpu;
    interrupts_init();
    psx_irq_set_cause_ptr(&cpu->cop0[13]);
    /* The current delivery decodes the vector's LUI/ADDIU pair. */
    fx_write(0x80u, 0x3C1A8000u);
    fx_write(0x84u, 0x275A0080u);
    fx_write(0x108u, FX_PCB);
    fx_write(0x110u, FX_TCB);
    fx_write(0x114u, FX_TCB_BYTES);
    fx_write(FX_PCB, FX_TCB);
    fx_write(FX_TCB, 0x4000u);
    fx_write(FX_TCB_REG(29), sp);
    fx_write(FX_TCB_REG(31), ra);
    fx_write(FX_TCB_EPC, entry_pc);
    fx_write(FX_TCB_SR, 0x00000404u); /* IEp + IM2: interrupts on after the pop */
    cpu->pc = entry_pc;
}

#endif /* RFE_GUEST_FIXTURE_H */
