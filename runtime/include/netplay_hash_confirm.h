#ifndef PSX_NETPLAY_HASH_CONFIRM_H
#define PSX_NETPLAY_HASH_CONFIRM_H

/*
 * MotK compatibility shim → recomp-net hash_confirm.
 * Prefer rnet_hc_* in new code.
 */

#include "recomp_net/hash_confirm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_HC_RING RNET_HC_RING
typedef RNetHashConfirm NetplayHashConfirm;

#define netplay_hc_reset            rnet_hc_reset
#define netplay_hc_prime_after      rnet_hc_prime_after
#define netplay_hc_note_local       rnet_hc_note_local
#define netplay_hc_note_peer        rnet_hc_note_peer
#define netplay_hc_resolved_through rnet_hc_resolved_through
#define netplay_hc_confirm_through  rnet_hc_confirm_through
#define netplay_hc_local_digest     rnet_hc_local_digest
#define netplay_hc_peer_digest      rnet_hc_peer_digest
#define netplay_hc_peek_mismatch    rnet_hc_peek_mismatch
#define netplay_hc_heal_stale_gap   rnet_hc_heal_stale_gap

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_HASH_CONFIRM_H */
