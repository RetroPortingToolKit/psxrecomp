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
