#ifndef PSX_NETPLAY_HOST_PROFILE_H
#define PSX_NETPLAY_HOST_PROFILE_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { PSX_HOST_MAX_OFFERS=8,PSX_HOST_MAX_IMPLEMENTATIONS=4,PSX_HOST_MAX_PEERS=9,
       PSX_HOST_CAPS_MAX_BYTES=56+72*PSX_HOST_MAX_OFFERS };
enum { PSX_HOST_ARCH_UNKNOWN,PSX_HOST_ARCH_X86,PSX_HOST_ARCH_X64,PSX_HOST_ARCH_ARM32,
       PSX_HOST_ARCH_ARM64,PSX_HOST_ARCH_PPC,PSX_HOST_ARCH_LOONGARCH,PSX_HOST_ARCH_RISCV,
       PSX_HOST_ARCH_COUNT };
enum { PSX_HOST_SSE2=1u,PSX_HOST_SSE3=2u,PSX_HOST_SSE41=4u,PSX_HOST_SSE42=8u,
       PSX_HOST_AVX=16u,PSX_HOST_AVX2=32u,PSX_HOST_AVX512F=64u,PSX_HOST_NEON=128u,
       PSX_HOST_ALTIVEC=256u,PSX_HOST_LSX=512u,PSX_HOST_LASX=1024u,
       PSX_HOST_FEATURES_ALL=2047u };
enum { PSX_HOST_BACKEND_REFERENCE=1u,PSX_HOST_BACKEND_ENHANCED=2u,
       PSX_HOST_BACKENDS_ALL=3u,PSX_HOST_PROFILE_PORTABLE=1u,
       PSX_HOST_PROFILE_DIRECT_LOOPBACK=2u,PSX_HOST_PROFILE_OBSERVER=4u };
typedef struct { uint32_t architecture,features_known;uint64_t features; } PSXHostCPU;
typedef struct {
    char profile[32];uint32_t revision,backend;uint8_t qualification[32];
} PSXHostOffer;
typedef struct {
    PSXHostCPU cpu;
    uint32_t compiled_backends,available_backends,offer_count;
    uint8_t build[32];
    PSXHostOffer offers[PSX_HOST_MAX_OFFERS];
} PSXHostCapabilities;
typedef struct { uint32_t backend,architectures;uint64_t required_features; } PSXHostImplementation;
/* Trusted local qualification policy, never accepted from a remote peer.
 * A name/feature/build string does not manufacture a qualification. */
typedef struct {
    char profile[32];uint32_t revision,priority,flags,implementation_count;
    uint8_t qualification[32];
    PSXHostImplementation implementations[PSX_HOST_MAX_IMPLEMENTATIONS];
} PSXHostProfileRule;
typedef struct { const PSXHostProfileRule *rules;uint32_t count; } PSXHostProfileCatalog;
typedef struct {
    char profile[32];uint32_t revision,flags;
    uint8_t qualification[32],identity[32];
} PSXHostSelection;
typedef struct {
    PSXHostSelection selected;
    uint32_t sealed,failed,peer_count,confirmed;
} PSXHostMatch;
enum { PSX_HOST_OK=1,PSX_HOST_INVALID=0,PSX_HOST_NO_COMMON=-1,
       PSX_HOST_BUILD_MISMATCH=-2,PSX_HOST_LOCKED=-3 };
/* No optional instructions/dependencies: unknown features, no backend offers. */
void psx_host_cpu_unknown(PSXHostCPU *out);
/* Separate optional SDL3 adapter; runtime CPU queries, no OS-version inference. */
void psx_host_cpu_detect_sdl3(PSXHostCPU *out);
uint32_t psx_host_compiled_backend(void);
/* Available starts at zero. Caller must supply actually initialized/probed
 * backends, the actual executable artifact SHA-256 (not a partial source
 * manifest), and trusted qualification rules. */
int psx_host_caps_init(PSXHostCapabilities *,const PSXHostCPU *,uint32_t compiled,
                       uint32_t available,const uint8_t build[32]);
int psx_host_caps_offer(PSXHostCapabilities *,const PSXHostProfileRule *,uint32_t backend);
int psx_host_caps_validate(const PSXHostCapabilities *,const PSXHostProfileCatalog *);
size_t psx_host_caps_encode(const PSXHostCapabilities *,uint8_t *,size_t);
int psx_host_caps_decode(PSXHostCapabilities *,const uint8_t *,size_t);
void psx_host_match_init(PSXHostMatch *);
/* Exactly once per match. session_identity is the existing complete title/
 * content/config identity, not a replacement for its current admission gate.
 * All build digests must match; this API does not enable cross-build play. */
int psx_host_match_select(PSXHostMatch *,const PSXHostCapabilities *,uint32_t peers,
                          const PSXHostProfileCatalog *,const uint8_t session_identity[32]);
int psx_host_match_confirm(PSXHostMatch *,uint32_t peer,const uint8_t identity[32]);
int psx_host_match_ready(const PSXHostMatch *);
/* Extend the EXISTING PMD1 identity for required sessions, so old peers that
 * ignore the capability protocol also reject before frame0. No CPU flags. */
int psx_host_required_content_identity(const char *prior,const uint8_t artifact[32],char out[65]);
#ifdef __cplusplus
}
#endif
#endif
