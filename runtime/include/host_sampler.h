#ifndef PSX_HOST_SAMPLER_H
#define PSX_HOST_SAMPLER_H
/* Always-on host CPU sampler for the emulation thread (debug-tools builds).
 * See host_sampler.c; TCP host_profile; tools/host_profile.py. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 Mi samples: ~17 minutes at 1 kHz, enough to keep a whole load-transition route (16 MB,
 * debug-tools builds only) so a hitch found afterwards still has its host call sites. */
#define HOST_SAMPLER_CAP 1048576u

typedef struct HostSample {
    uint64_t rva;      /* instruction pointer minus the executable's base */
    uint32_t frame;    /* guest frame (s_frame_count) */
    uint32_t in_pass;  /* 1 while a render pass ran */
} HostSample;

/* Start sampling the calling thread (the emulation thread). Idempotent. */
void host_sampler_start(void);
int host_sampler_supported(void);
/* Total samples recorded; the ring holds the newest HOST_SAMPLER_CAP. */
uint64_t host_sampler_seq(void);
uint64_t host_sampler_image_base(void);
/* Sample `seq`, if it is still in the ring. */
int host_sampler_get(uint64_t seq, HostSample *out);

#ifdef __cplusplus
}
#endif
#endif
