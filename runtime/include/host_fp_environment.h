#ifndef PSX_HOST_FP_ENVIRONMENT_H
#define PSX_HOST_FP_ENVIRONMENT_H
#ifdef __cplusplus
extern "C" {
#endif
/* Call on the emulation thread before it creates simulation workers. Resets
 * the C floating environment; x64 additionally fixes SSE rounding, exception
 * masks and gradual underflow. No CPU capability polling or live selection.
 * A successful local reset is not a heterogeneous qualification. */
int psx_host_fp_environment_set_default(void);
int psx_host_fp_environment_is_default(void);
/* 2=x64 SSE controls checked, 1=portable C default only, 0=unavailable. */
int psx_host_fp_environment_contract(void);
#ifdef __cplusplus
}
#endif
#endif
