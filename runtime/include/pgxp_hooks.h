#ifndef PGXP_HOOKS_H
#define PGXP_HOOKS_H

/* PGXP dataflow-shadowing hook surface (docs/ENHANCEMENTS.md G1.2/G1.3).
 *
 * The runtime keeps a host-only "shadow" of sub-pixel GTE projection results
 * and follows them BY PROVENANCE as the game moves them through registers and
 * RAM into a GP0 packet (pgxp.cpp). Generated code participates by calling
 * these hooks after the guest operation completes.
 *
 * Gating model (the G1.3 decision): the emitter writes PGXP_*() macro
 * invocations unconditionally; this header expands them to real calls only
 * when the translation unit is compiled with -DPSX_PGXP=1 (the pgxp build
 * variant). In the base variant the macros expand to ((void)0), so base
 * objects compile to exactly the pre-feature code — the feature costs nothing
 * unless a title links the pgxp target.
 *
 * RULES for emit sites:
 *   - Macro arguments MUST be side-effect free (they vanish in base builds).
 *   - Hooks run AFTER the guest architectural effect (value = what actually
 *     loaded/stored/resulted), so the shadow can validate against reality.
 *   - `cpu` must be in scope at every emit site (it always is in generated C).
 *
 * The interpreters (dirty_ram_interp.c, psx_interpreter.c) call the
 * psx_pgxp_* functions directly — they are runtime code compiled once, and
 * every hook early-outs on a single global when the feature is off.
 *
 * Overlay DLLs cannot link runtime symbols directly; they forward through the
 * PGXPHooks table below (OverlayCallbacks.pgxp, overlay_api.h). A NULL table
 * no-ops every hook — precision shadowing is a visual enhancement, never
 * load-bearing.
 *
 * LICENSING: this is a clean-room implementation of the publicly documented
 * PGXP technique (psx-spx, public design write-ups). Do NOT port code from
 * the vendored duckstation/ (CC BY-NC-ND) or beetle-psx/ (GPL) trees; they
 * are black-box behavioral oracles only. See docs/ENHANCEMENTS.md G1.
 */

#include <stdint.h>
#include "pgxp_deprecation.h"

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

/* Funnel entry points (implemented in runtime/src/pgxp.cpp). The raw
 * instruction word carries the op/rs/rt/rd/imm decode so the emitted call is
 * uniform per class; `value` is the architectural result of the operation.
 *
 *   load:   rt <- [addr]; value = loaded word/half/byte (post-extension)
 *   store:  [addr] <- rt; value = the stored word/half/byte
 *   alu:    rd/rt <- op(s1, s2); result = the written value (also LUI, HILO
 *           moves, MOVE idioms and MFC0/CFC0 — the body decodes the class
 *           from instr)
 *   muldiv: HI/LO <- op(s1, s2)
 *   cop2:   MFC2/CFC2/MTC2/CTC2 (addr = 0) and LWC2/SWC2 (addr = guest addr);
 *           value = the transferred word
 */
void psx_pgxp_load  (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
void psx_pgxp_store (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
/* Complete compiler-owned GPR receipts at an actual precision/observer boundary. */
/* Existing native output ownership, also used by generated control frames. */
void pgxp_native_clobber(struct CPUState *cpu, uint32_t gpr_mask, uint32_t gte_mask);
void psx_pgxp_region_materialize(struct CPUState *cpu, uint32_t mask,
    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
/* Compiler-authored final affine receipts, not an instruction replay stream.
 * All sources are the incoming canonical registers; snapshot them together
 * before publishing aliases, and recover the original integer from dst-addend.
 * Internal native regions only: portable/reference/DLL entries keep full hooks. */
typedef struct PSXPGXPRegionAdd {
    uint8_t destination, source;
    int16_t addend;
} PSXPGXPRegionAdd;
void psx_pgxp_region_add_immediates(struct CPUState *cpu,
    const PSXPGXPRegionAdd *definitions, uint32_t count);
void psx_pgxp_alu   (struct CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);
void psx_pgxp_muldiv(struct CPUState *cpu, uint32_t instr, uint32_t hi, uint32_t lo, uint32_t s1, uint32_t s2);
void psx_pgxp_cop2  (struct CPUState *cpu, uint32_t instr, uint32_t value, uint32_t addr);

/* Proven metadata-free ALU writes are coalesced until the next full hook.
 * Values are captured at the architectural write, including load-delay pairs.
 * Overlay DLLs retain the existing five-function callback ABI. */
extern int psx_pgxp_plain_enabled;
extern uint32_t psx_pgxp_plain_mask;
extern uint32_t psx_pgxp_plain_values[32];

#if defined(PSX_PGXP) && PSX_PGXP && \
    defined(PSX_NATIVE_PRECISION_SLICES) && PSX_NATIVE_PRECISION_SLICES && \
    !defined(PSX_OVERLAY_DLL_BUILD)
#define PGXP_PLAIN_ALU(dst, instr, res, s1, s2) do { \
    if (psx_pgxp_plain_enabled) { \
        psx_pgxp_plain_values[(dst)] = (res); \
        psx_pgxp_plain_mask |= UINT32_C(1) << (dst); \
    } \
} while (0)
#else
#define PGXP_PLAIN_ALU(dst, instr, res, s1, s2) PGXP_ALU(instr, res, s1, s2)
#endif

/* Function-CFG analysis proved that this shadow result is overwritten before
 * any precision consumer. Still end its old lineage, including equal words:
 * reuse the maintained destructive-write queue, flushed by full hooks,
 * checkpoints, debug inspection and serialization. Never bypass memory/GTE
 * hooks. Reference, overlays and non-native timing retain the full engine. */
#if defined(PSX_PGXP) && PSX_PGXP && defined(PSX_PGXP_DATAFLOW) && PSX_PGXP_DATAFLOW && \
    defined(PSX_NATIVE_CPU_SPANS) && PSX_NATIVE_CPU_SPANS && !defined(PSX_OVERLAY_DLL_BUILD)
#define PGXP_FLOW_DEAD_ALU(dst, instr, res, s1, s2) do { \
    if (psx_pgxp_plain_enabled) { \
        psx_pgxp_plain_values[(dst)] = (res); \
        psx_pgxp_plain_mask |= UINT32_C(1) << (dst); \
    } \
} while (0)
/* A later same-slot ALU definition is guaranteed before every observer, so
 * the earlier dead lifetime has neither a hook nor a deferred write record.
 * The final write still ends old/equal-word lineage through the full engine. */
#define PGXP_FLOW_ELIDED_ALU(dst, instr, res, s1, s2) ((void)0)
#else
#define PGXP_FLOW_DEAD_ALU(dst, instr, res, s1, s2) PGXP_ALU(instr, res, s1, s2)
#define PGXP_FLOW_ELIDED_ALU(dst, instr, res, s1, s2) PGXP_ALU(instr, res, s1, s2)
#endif

/* Forwarder table for overlay DLLs (OverlayCallbacks.pgxp). Appended-last
 * member semantics apply: a NULL pointer (older host) means "no shadowing". */
typedef struct PGXPHooks {
    void (*load)  (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
    void (*store) (struct CPUState *cpu, uint32_t instr, uint32_t addr, uint32_t value);
    void (*alu)   (struct CPUState *cpu, uint32_t instr, uint32_t result, uint32_t s1, uint32_t s2);
    void (*muldiv)(struct CPUState *cpu, uint32_t instr, uint32_t hi, uint32_t lo, uint32_t s1, uint32_t s2);
    void (*cop2)  (struct CPUState *cpu, uint32_t instr, uint32_t value, uint32_t addr);
} PGXPHooks;

#if defined(PSX_PGXP) && PSX_PGXP
#define PGXP_LOAD(instr, addr, val)              psx_pgxp_load(cpu, (instr), (addr), (val))
#define PGXP_STORE(instr, addr, val)             psx_pgxp_store(cpu, (instr), (addr), (val))
#define PGXP_ALU(instr, res, s1, s2)             psx_pgxp_alu(cpu, (instr), (res), (s1), (s2))
#define PGXP_MULDIV(instr, hi, lo, s1, s2)       psx_pgxp_muldiv(cpu, (instr), (hi), (lo), (s1), (s2))
#define PGXP_COP2(instr, val, addr)              psx_pgxp_cop2(cpu, (instr), (val), (addr))
#else
#define PGXP_LOAD(instr, addr, val)              ((void)0)
#define PGXP_STORE(instr, addr, val)             ((void)0)
#define PGXP_ALU(instr, res, s1, s2)             ((void)0)
#define PGXP_MULDIV(instr, hi, lo, s1, s2)       ((void)0)
#define PGXP_COP2(instr, val, addr)              ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* PGXP_HOOKS_H */
