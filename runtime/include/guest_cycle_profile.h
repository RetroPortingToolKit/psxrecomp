#ifndef PSXRECOMP_GUEST_CYCLE_PROFILE_H
#define PSXRECOMP_GUEST_CYCLE_PROFILE_H

/* Always-on guest-cycle profile ring; see guest_cycle_profile.c. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GCP_FLAG_EXCEPTION 0x01u

typedef struct GcpSample {
    uint64_t cycle;   /* guest cycle at the sample */
    uint32_t frame;   /* guest frame (s_frame_count) */
    uint32_t block;   /* last basic-block leader, phys (debug-tools builds) */
    uint32_t fn;      /* last guest function entered */
    uint32_t ra;      /* guest $ra */
    uint32_t native;  /* native overlay shard in progress, 0 = none */
    uint32_t disp;    /* static-dispatch stamp */
    uint32_t weight;  /* sample periods this sample stands for */
    uint8_t  phase;   /* psx_exec_phase() */
    uint8_t  flags;   /* GCP_FLAG_* */
} GcpSample;

/* Install the sampler (period from PSX_GCP_PERIOD, default 4096; 0 = off). */
void     guest_cycle_profile_install(void);
uint32_t guest_cycle_profile_period(void);
uint64_t guest_cycle_profile_total(void);
uint32_t guest_cycle_profile_capacity(void);
uint64_t guest_cycle_profile_oldest(void);
int      guest_cycle_profile_get(uint64_t seq, GcpSample *out);
/* Write every held sample whose frame is in [frame_lo, frame_hi] as CSV. */
int      guest_cycle_profile_write_csv(const char *path, uint64_t frame_lo,
                                       uint64_t frame_hi, uint64_t *rows_out,
                                       uint64_t *weight_out);

#ifdef __cplusplus
}
#endif

#endif
