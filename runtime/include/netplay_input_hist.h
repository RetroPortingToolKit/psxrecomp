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

/* PsxNetPad ↔ RNetRbFrame (LX/LY only; RX/RY stay on the pad blob path). */
void netplay_ih_pad_to_frame(const PsxNetPad *pad, uint32_t tick, uint8_t predicted,
                             RNetRbFrame *out);
void netplay_ih_frame_to_pad(const RNetRbFrame *frame, PsxNetPad *pad);

#ifdef __cplusplus
}
#endif

#endif /* PSX_HAS_RECOMP_NET */

#endif /* PSX_NETPLAY_INPUT_HIST_H */
