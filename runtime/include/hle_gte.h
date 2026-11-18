#ifndef PSX_HLE_GTE_H
#define PSX_HLE_GTE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;
/* For a build-selected native service which owns operation-level timing.
 * Retains GTE outputs, PGXP and projection capture/replay. ENHANCED omits
 * per-command latency; REFERENCE uses the ordinary timed entry. The caller
 * resolves earlier pending work and publishes its batch completion itself.
 * No mutable mode, scope nesting, or state survives this call. */
void psx_hle_gte_execute(struct CPUState* cpu, uint32_t command);
#ifdef __cplusplus
}
namespace PSXRecomp { namespace GTE { struct GTEState; }}
/* A whole packet-producing job keeps the maintained value state resident.
 * No second register representation or per-command CPU marshaling is used.
 * Only non-projecting packet commands are accepted; input/output precision
 * belongs to the native operation's explicit boundary. The job owns timing,
 * resolves earlier work, and publishes CPU/GTE state before returning. */
void psx_hle_gte_packet_load(PSXRecomp::GTE::GTEState& state, const CPUState& cpu);
bool psx_hle_gte_packet_command(PSXRecomp::GTE::GTEState& state, uint32_t command);
void psx_hle_gte_packet_store(CPUState& cpu, const PSXRecomp::GTE::GTEState& state);
#endif
#endif
