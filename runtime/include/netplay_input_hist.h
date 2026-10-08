#ifndef PSX_NETPLAY_INPUT_HIST_H
#define PSX_NETPLAY_INPUT_HIST_H

/*
 * MotK input history: portable ring/invent in recomp-net; PSX pad
 * conversion stays in netplay_input_hist.c.
 */

#if defined(PSX_HAS_RECOMP_NET)

#include <stdint.h>

#include "psx_netplay.h"
#include "recomp_net/input_hist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETPLAY_INPUT_HIST_DEPTH     RNET_INPUT_HIST_DEPTH
#define NETPLAY_INPUT_HIST_MAX_SLOTS RNET_INPUT_HIST_MAX_SLOTS
typedef RNetInputHist NetplayInputHist;

#define netplay_ih_reset             rnet_ih_reset
#define netplay_ih_frame_to_contract rnet_ih_frame_to_contract
#define netplay_ih_put               rnet_ih_put
#define netplay_ih_get               rnet_ih_get
#define netplay_ih_invent_hold_last  rnet_ih_invent_hold_last
#define netplay_ih_invent_idle       rnet_ih_invent_idle
#define netplay_ih_promote           rnet_ih_promote

/* The portable stick contract deliberately omits controller type and RX/RY.
 * Those fields still affect the guest pad protocol and must not be silently
 * promoted on a completed tick or mistaken for a digital-only release. */
static inline int netplay_ih_extra_pad_differ(const RNetRbFrame *a,
                                             const RNetRbFrame *b)
{
    return a->analog != b->analog || a->rx != b->rx || a->ry != b->ry;
}

static inline int netplay_ih_pad_payload_equal(const RNetRbFrame *a,
                                               const RNetRbFrame *b)
{
    return a->buttons == b->buttons && a->stick_x == b->stick_x &&
           a->stick_y == b->stick_y && !netplay_ih_extra_pad_differ(a, b);
}

static inline RNetInputContractDecision netplay_ih_pad_correction_decide(
    const RNetRbFrame *published, const RNetRbFrame *wire, uint8_t completed,
    const RNetInputContractParams *params, const RNetInputContractHostGates *gates)
{
    RNetInputContractFrame pub_c, wire_c;
    if (completed && netplay_ih_extra_pad_differ(published, wire))
        return nRNetInputContractRewind;
    netplay_ih_frame_to_contract(published, &pub_c);
    netplay_ih_frame_to_contract(wire, &wire_c);
    return rnet_input_contract_stick_replace_decide(&pub_c, &wire_c,
                                                   completed, params, gates);
}

/* PsxNetPad ↔ RNetRbFrame: both sticks, controller type and NeGcon pressures. */
void netplay_ih_pad_to_frame(const PsxNetPad *pad, uint32_t tick, uint8_t predicted,
                             RNetRbFrame *out);
void netplay_ih_frame_to_pad(const RNetRbFrame *frame, PsxNetPad *pad);

#ifdef __cplusplus
}
#endif

#endif /* PSX_HAS_RECOMP_NET */

#endif /* PSX_NETPLAY_INPUT_HIST_H */
