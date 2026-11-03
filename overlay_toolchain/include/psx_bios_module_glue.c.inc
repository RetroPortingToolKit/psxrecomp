/* psx_bios_module_glue.c.inc — the module side of a loadable BIOS backend.
 *
 * Compiled ONCE per module as its own translation unit by
 * tools/bios_module_build.py, after overlay_dispatch_preamble.c.inc (which
 * defines g_cbs, overlay_flush_cycles and every forwarder a BIOS shares with
 * overlay shards). This file adds what only a BIOS dispatcher needs — see
 * psx_bios_module.h — and the four exported entry points.
 *
 * Contract: nothing here may reach a runtime symbol directly. Every call and
 * every global goes through g_bcbs / g_cbs, or through the pointers the
 * generated C's macro-rewritten externs declare. PSX_BIOS_MODULE_STEM is the
 * image stem (-DPSX_BIOS_MODULE_STEM=SCPH1001) and names the descriptor the
 * generated dispatch exports.
 */
#include "psx_bios_backend.h"
#include "psx_bios_module.h"

#ifndef PSX_BIOS_MODULE_STEM
#error "PSX_BIOS_MODULE_STEM must name the image stem (e.g. -DPSX_BIOS_MODULE_STEM=SCPH1001)"
#endif
#define PSX_BM_CAT2(a, b) a##b
#define PSX_BM_CAT(a, b)  PSX_BM_CAT2(a, b)
#define PSX_BM_DESCRIPTOR PSX_BM_CAT(PSX_BIOS_MODULE_STEM, _psx_bios_backend)

extern const PsxBiosBackend PSX_BM_DESCRIPTOR;

static PsxBiosModuleCallbacks g_bcbs;

/* Local dummies so a call before init cannot fault; init repoints them. */
static int      (*s_hle_hook_dummy)(CPUState *cpu, uint32_t phys);
static int      s_depth_dummy;
static uint64_t s_hits_dummy;
static uint32_t s_func_addr_dummy;
static uint64_t s_flattened_dummy;
static uint32_t s_ram_mask_dummy = 0x1FFFFFu;
static uint64_t s_frame_count_dummy;
static uint32_t s_mod_guest_count_dummy;

/* The pointers the generated C declares (via the psx_bios_module.h name
 * mappings): `extern int g_psx_dispatch_depth;` became
 * `extern int (*g_psx_bm_dispatch_depth_p);`, and so on. Defined here. */
int      (**g_psx_bm_hle_hook_p)(CPUState *cpu, uint32_t phys) = &s_hle_hook_dummy;
int       *g_psx_bm_dispatch_depth_p    = &s_depth_dummy;
uint64_t  *g_psx_bm_static_hits_p       = &s_hits_dummy;
uint32_t  *g_psx_bm_current_func_addr_p = &s_func_addr_dummy;
uint64_t  *g_psx_bm_bail_flattened_p    = &s_flattened_dummy;
uint32_t  *g_psx_bm_ram_mask_p          = &s_ram_mask_dummy;
uint64_t  *g_psx_bm_frame_count_p       = &s_frame_count_dummy;
uint32_t  *g_psx_bm_mod_guest_functions_p = &s_mod_guest_count_dummy;

/* ---- Forwarders for what OverlayCallbacks does not carry ----------------- */
void psx_arith_overflow(CPUState *cpu) {
    overlay_flush_cycles();
    if (g_bcbs.arith_overflow) g_bcbs.arith_overflow(cpu);
}
void psx_break(CPUState *cpu, uint32_t code, uint32_t pc) {
    overlay_flush_cycles();
    if (g_bcbs.brk) g_bcbs.brk(cpu, code, pc);
}
void psx_unaligned_access(CPUState *cpu, uint32_t addr, uint32_t pc) {
    overlay_flush_cycles();
    if (g_bcbs.unaligned_access) g_bcbs.unaligned_access(cpu, addr, pc);
}
void psx_rfe_escape_check(CPUState *cpu) {
    overlay_flush_cycles();
    if (g_bcbs.rfe_escape_check) g_bcbs.rfe_escape_check(cpu);
}
int dirty_ram_dispatch(CPUState *cpu, uint32_t addr, uint32_t stop_addr) {
    overlay_flush_cycles();
    return g_bcbs.dirty_ram_dispatch ? g_bcbs.dirty_ram_dispatch(cpu, addr, stop_addr) : 0;
}
int dirty_ram_is_dirty(uint32_t phys) {
    return g_bcbs.dirty_ram_is_dirty ? g_bcbs.dirty_ram_is_dirty(phys) : 0;
}
int dirty_ram_text_native_ok(uint32_t phys) {
    return g_bcbs.dirty_ram_text_native_ok ? g_bcbs.dirty_ram_text_native_ok(phys) : 0;
}
int psx_kernel_bless_dispatchable(uint32_t phys) {
    return g_bcbs.kernel_bless_dispatchable ? g_bcbs.kernel_bless_dispatchable(phys) : 0;
}
int psx_game_address_in_text(uint32_t addr) {
    return g_bcbs.game_address_in_text ? g_bcbs.game_address_in_text(addr) : 0;
}
int fntrace_is_game_started(void) {
    return g_bcbs.fntrace_is_game_started ? g_bcbs.fntrace_is_game_started() : 0;
}
void fntrace_record(CPUState *cpu, uint32_t target) {
    if (g_bcbs.fntrace_record) g_bcbs.fntrace_record(cpu, target);
}
void debug_server_trace_dispatch(uint32_t func_addr) {
    if (g_bcbs.trace_dispatch) g_bcbs.trace_dispatch(func_addr);
}
void debug_server_log_probe(uint32_t pc, CPUState *cpu) {
    if (g_bcbs.log_probe) g_bcbs.log_probe(pc, cpu);
}
void psx_segment_miss_record_kind(uint32_t addr, uint32_t home, uint32_t ra,
                                  uint32_t sp, uint32_t frame, uint32_t kind) {
    if (g_bcbs.segment_miss_record_kind)
        g_bcbs.segment_miss_record_kind(addr, home, ra, sp, frame, kind);
}

int g_psx_bm_mod_dispatch_guest(CPUState *cpu, uint32_t addr) {
    overlay_flush_cycles();
    return g_bcbs.mod_dispatch_guest_function
               ? g_bcbs.mod_dispatch_guest_function(cpu, addr) : 0;
}

/* ---- Exports ------------------------------------------------------------- */
PSX_OVERLAY_EXPORT int psx_bios_module_abi(void) {
    return PSX_BIOS_MODULE_ABI_TAG;
}
PSX_OVERLAY_EXPORT uint32_t psx_bios_module_codegen_hash(void) {
    return (uint32_t)PSX_OVERLAY_CODEGEN_HASH;
}
PSX_OVERLAY_EXPORT int psx_bios_module_init(const OverlayCallbacks *cbs,
                                            const PsxBiosModuleCallbacks *bcbs) {
    if (!cbs || !bcbs) return 0;
    /* The host's table must be at least as long as ours: a shorter one would
     * leave our tail members reading past its end. The ABI tag already pins
     * the version; this is the belt to that suspender. */
    if (bcbs->size < sizeof(PsxBiosModuleCallbacks)) return 0;
    overlay_init(cbs);
    g_bcbs = *bcbs;
    if (g_bcbs.hle_hook)          g_psx_bm_hle_hook_p          = g_bcbs.hle_hook;
    if (g_bcbs.dispatch_depth)    g_psx_bm_dispatch_depth_p    = g_bcbs.dispatch_depth;
    if (g_bcbs.static_hits)       g_psx_bm_static_hits_p       = g_bcbs.static_hits;
    if (g_bcbs.current_func_addr) g_psx_bm_current_func_addr_p = g_bcbs.current_func_addr;
    if (g_bcbs.bail_flattened)    g_psx_bm_bail_flattened_p    = g_bcbs.bail_flattened;
    if (g_bcbs.ram_mask)          g_psx_bm_ram_mask_p          = g_bcbs.ram_mask;
    if (g_bcbs.frame_count)       g_psx_bm_frame_count_p       = g_bcbs.frame_count;
    if (g_bcbs.mod_guest_functions) g_psx_bm_mod_guest_functions_p = g_bcbs.mod_guest_functions;
    return 1;
}
PSX_OVERLAY_EXPORT const PsxBiosBackend *psx_bios_module_backend(void) {
    return &PSX_BM_DESCRIPTOR;
}
