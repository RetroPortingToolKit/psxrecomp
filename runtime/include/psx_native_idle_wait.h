#ifndef PSX_NATIVE_IDLE_WAIT_H
#define PSX_NATIVE_IDLE_WAIT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed build policy, configured before guest execution. Limit the detector
 * and last-store tracking to declared wait edges; ordinary config-owned idle
 * detection keeps its existing behavior when this policy is not selected. */
void psx_idle_skip_set_audited_only(int enabled);
/* Native kernel: audited SDK timeout loops wait against hardware time even
 * while active game CPU work uses an enhanced guest cycle scale. */
void psx_idle_skip_set_hw_countdown_timing(int enabled);

#ifdef __cplusplus
}
#endif
#endif
