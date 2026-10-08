/* PsxNetPad normalization: pure, no SIO or netplay state, so every consumer
 * (capture, netplay encode/decode, rollback history) and its tests share the
 * one definition. */
#include "psx_netplay.h"
#include "sio.h"

static uint8_t centre_deadzone(uint8_t v) {
    const int dead = 24; /* ~SDL-ish center deadzone in 0..255 space */
    return (v > (uint8_t)(0x80 - dead) && v < (uint8_t)(0x80 + dead)) ? 0x80 : v;
}

void psx_netplay_normalize_pad(PsxNetPad *pad)
{
    if (!pad) return;
    pad->connected = 1;
    if (pad->analog > PSX_NETPAD_TYPE_MAX) pad->analog = 0u;
    if (!pad->analog) {
        pad->lx = pad->ly = pad->rx = pad->ry = 0x80;
        return;
    }
    pad->lx = centre_deadzone(pad->lx);
    /* NeGcon ly/rx/ry are pressures (0 = released), not centred axes. */
    if (pad->analog == SIO_PAD_NEGCON) return;
    pad->ly = centre_deadzone(pad->ly);
    pad->rx = centre_deadzone(pad->rx);
    pad->ry = centre_deadzone(pad->ry);
}
