#ifndef PSX_HOST_SAMPLER_H
#define PSX_HOST_SAMPLER_H
/* Always-on host CPU sampler for the emulation thread, including Release.
 * See host_sampler.c; TCP host_profile; tools/host_profile.py. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 Mi samples: ~17 minutes at 1 kHz, enough to keep a whole load-transition route (24 MB) so a hitch found afterwards still has its host call sites. */
#define HOST_SAMPLER_CAP 1048576u

enum { HOST_SAMPLE_EXCEPTION = 1u };

typedef struct HostSample {
    uint64_t rva;      /* instruction pointer minus the executable's base */
    uint32_t frame;    /* guest frame (s_frame_count) */
    uint32_t in_pass;  /* 1 while a render pass ran */
    uint32_t guest_pc; /* last entered guest function: leaf-biased, not an instruction PC */
    uint32_t flags;    /* HOST_SAMPLE_* context */
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
