#ifndef PSX_NETPLAY_HOST_GATE_H
#define PSX_NETPLAY_HOST_GATE_H
#include "netplay_host_profile.h"
#define NP_HOST_CAPS_MAGIC 0x50484331u
#define NP_HOST_SELECT_MAGIC 0x50485331u
#define NP_HOST_ERROR_MAGIC 0x50484531u
enum { NP_HOST_FRAGMENTS=(PSX_HOST_CAPS_MAX_BYTES+20+11)/12,NP_HOST_WIRE_BYTES=NP_HOST_FRAGMENTS*12 };
enum { NP_HOST_PENDING,NP_HOST_BAD_DATA,NP_HOST_CONFLICT,NP_HOST_NO_COMMON,
       NP_HOST_ARTIFACT,NP_HOST_CONFIRM,NP_HOST_REMOTE,NP_HOST_TIMEOUT,
       NP_HOST_STARTUP_MISSING,NP_HOST_SPECTATOR,NP_HOST_EARLY_CONTROL };
typedef struct {uint32_t epoch,a,b,seen;uint8_t slot,flags;} NpHostPacket;
typedef struct {uint32_t epoch,a,b,seen;uint8_t slot,op,flags;int from;} NpHostControl;
typedef struct {
    uint8_t wire[NP_HOST_WIRE_BYTES],selection[32];
    uint64_t fragments;
    uint32_t bytes,chunks,selection_chunks;
} NpHostPeer;
typedef struct {
    uint32_t enabled,error,local,occupied,known,confirmed,acknowledged,cursor;
    uint32_t observer;
    PSXHostCapabilities observer_capabilities;
    uint32_t clock_started,start_ms;
    uint8_t session[32];
    PSXHostProfileRule rules[PSX_HOST_MAX_OFFERS];
    PSXHostCapabilities capabilities[9];
    NpHostPeer peers[9];
    PSXHostMatch match;
    NpHostControl deferred[64];
    uint32_t deferred_head,deferred_count;
} NpHostGate;
int np_host_gate_init(NpHostGate *,const PSXHostCapabilities *,const PSXHostProfileCatalog *,
                      uint32_t occupied,uint32_t local,const uint8_t session[32],const uint8_t nonce[16]);
/* Receive-only: validate the seated agreement against a genuinely offered
 * observer-capable local implementation. Never publishes a seat or changes
 * the player selection; immutable offers may be replayed for late observers. */
int np_host_gate_observer_init(NpHostGate *,const PSXHostCapabilities *,const PSXHostProfileCatalog *,
                             uint32_t occupied,const uint8_t session[32]);
int np_host_gate_note(NpHostGate *,const NpHostPacket *,int authoritative_from);
int np_host_gate_packet(NpHostGate *,NpHostPacket *);
int np_host_gate_ready(const NpHostGate *);
void np_host_gate_deadline(NpHostGate *,uint32_t now_ms);
const char *np_host_gate_error(const NpHostGate *);
int np_host_gate_defer(NpHostGate *,const NpHostControl *);
int np_host_gate_take_deferred(NpHostGate *,NpHostControl *);
#endif
