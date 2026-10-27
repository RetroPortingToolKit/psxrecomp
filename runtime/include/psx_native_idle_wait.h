#ifndef PSX_NATIVE_IDLE_WAIT_H
#define PSX_NATIVE_IDLE_WAIT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed build policy, configured before guest execution. Limit the detector
 * and last-store tracking to declared wait edges; ordinary config-owned idle
 * detection keeps its existing behavior when this policy is not selected. */
void psx_idle_skip_set_audited_only(int enabled);

/* Some audited title profiles qualify normal pacing only. Presentation-free
 * debugger turbo then keeps the same compiled guest polling path. The build
 * policy is fixed before execution; changing debugger mode invalidates training.
 * This does not change manual fast-forward or another title's default policy. */
void psx_idle_skip_exclude_debug_turbo(int enabled);
void psx_idle_skip_set_debug_turbo(int active);
/* bit0: exclusion selected; bit1: debugger turbo currently active. */
uint32_t psx_idle_skip_debug_turbo_policy(void);

#ifdef __cplusplus
}
#endif
#endif
