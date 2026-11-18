#ifndef PSX_LOCAL_PAIR_PREPARE_H
#define PSX_LOCAL_PAIR_PREPARE_H
#include "mod_plugins.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Opt-in split of the existing local-pair operation. Preparation executes the
 * two already-required draw-only callbacks once, through the same reversible
 * sandbox, and parks their complete owned images without publishing them.
 * Commit happens at the caller's original native boundary, executes NO guest
 * code and consumes the lease once. A title must audit any render-input change
 * between preparation and that boundary; this API does not assert equivalence.
 * One pending pair maximum. Reset/load/cancel never revives an old lease. */
uint64_t psx_mod_prepare_local_pair(struct CPUState *cpu,const PSXModStereoFrame *frame,
    PSXModStereoFn fn,void *user);
int psx_mod_commit_prepared_local_pair(uint64_t lease);
void psx_mod_cancel_prepared_local_pair(uint64_t lease);
#ifdef __cplusplus
}
#endif
#endif
