#ifndef PSX_STATIC_CALLS_H
#define PSX_STATIC_CALLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;
typedef struct PsxStaticCodeValidity {
    const uint32_t *ranges;
    uint32_t range_count;
    uint64_t revision;
    uint64_t page_sum;
    uint32_t ram_mask;
    uint32_t matches;
} PsxStaticCodeValidity;

typedef struct PsxStaticCallStats {
    uint64_t validity_queries, validity_hits, validity_verifies;
    uint64_t fast_entries, fast_misses;
} PsxStaticCallStats;

int psx_static_code_valid(PsxStaticCodeValidity *state,
                          const uint32_t *ranges, uint32_t count, uint32_t pc);
void psx_static_call_get_stats(PsxStaticCallStats *out);
void psx_static_call_note_entry(int handled);
int dirty_ram_dispatch_game_direct(struct CPUState *cpu, uint32_t addr);
/* Guaranteed tail chains retain the flat owner as the sole stop/return owner.
 * The per-image hook callback is the exact sequence used by that owner. */
typedef int (*PsxNativeTailHooks)(struct CPUState *, uint32_t);
typedef void (*PsxNativeTailBody)(struct CPUState *);
int dirty_ram_dispatch_game_chain(struct CPUState *, uint32_t, int, PsxNativeTailHooks);
int psx_native_tail_prepare(struct CPUState *, uint32_t, PsxNativeTailBody, PsxNativeTailBody);
void psx_native_tail_note_body(struct CPUState *, PsxNativeTailBody);
int psx_game_native_tail_enter(struct CPUState *, uint32_t, PsxNativeTailBody);

/* Never substitute an optional tail-call optimization. The build selection is
 * enabled only after compiling this exact statement/ABI with attribute errors
 * fatal; unsupported compiler/targets retain the original CPS return. */
#if defined(PSX_NATIVE_TAIL_CHAINS) && PSX_NATIVE_TAIL_CHAINS && !defined(PSX_OVERLAY_DLL_BUILD)
# if defined(__clang__)
#  define PSX_GUARANTEED_TAIL [[clang::musttail]]
# elif defined(__GNUC__) && __GNUC__ >= 15
#  define PSX_GUARANTEED_TAIL __attribute__((musttail))
# else
#  error PSX_NATIVE_TAIL_CHAINS requires a guaranteed musttail compiler/ABI
# endif
# define PSX_TRY_NATIVE_TAIL(cpu, pc, caller, body) do { \
    const int psx_tail_result_ = psx_native_tail_prepare((cpu), (pc), (caller), (body)); \
    if (psx_tail_result_ > 0) { PSX_GUARANTEED_TAIL return (body)(cpu); } \
    if (psx_tail_result_ < 0) return; \
} while (0)
#else
# define PSX_TRY_NATIVE_TAIL(cpu, pc, caller, body) do { } while (0)
#endif

/* OFF/REFERENCE retains the original call itself, with no extra cache wrapper
 * or watched pages on its hot path. Generated state tokens vanish with it. */
#if defined(PSX_NATIVE_STATIC_CALLS) && PSX_NATIVE_STATIC_CALLS
#define PSX_STATIC_CODE_VALID(state, ranges, count, pc) \
    psx_static_code_valid((state), (ranges), (count), (pc))
#else
#define PSX_STATIC_CODE_VALID(state, ranges, count, pc) \
    dirty_ram_text_native_ok_ranges_from((ranges), (count), (pc))
#endif

#ifdef __cplusplus
}
#endif
#endif
