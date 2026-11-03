/* psx_host_rumble.h - title-supplied host rumble (psx_mod_set_host_rumble).
 *
 * Some titles compute vibration for every pad but only send it over SIO to a
 * DualShock (R4 with a presented NeGcon keeps running its vibration patterns
 * and writes them nowhere useful). A trusted mod can hand the runtime the
 * DualShock-equivalent motor values instead; the runtime forwards the louder
 * of that and the guest's own SIO motors to the host pad. A value lapses
 * PSX_HOST_RUMBLE_TTL VBlanks after the last call, so a mod that stops
 * calling (menus, unloaded) can never leave a pad vibrating. Pure, so tests
 * link it directly. Nothing changes for a title that never calls it. */
#ifndef PSX_HOST_RUMBLE_H
#define PSX_HOST_RUMBLE_H

#include <stdint.h>

#define PSX_HOST_RUMBLE_TTL 8u

typedef struct PsxHostRumbleSlot {
    uint8_t  small;    /* DualShock small motor: 0 off, nonzero on */
    uint8_t  large;    /* DualShock large motor strength 0..255 */
    uint8_t  valid;
    uint32_t stamp;    /* VBlank of the last call */
} PsxHostRumbleSlot;

static inline void psx_host_rumble_set(PsxHostRumbleSlot *slot, uint32_t now,
                                       uint32_t small, uint32_t large) {
    if (!slot) return;
    slot->small = small ? 1u : 0u;
    slot->large = (uint8_t)(large > 255u ? 255u : large);
    slot->valid = 1u;
    slot->stamp = now;
}

/* Merge into the guest's SIO motor values (in/out). */
static inline void psx_host_rumble_merge(const PsxHostRumbleSlot *slot,
                                         uint32_t now, uint8_t *small,
                                         uint8_t *large) {
    if (!slot || !slot->valid || !small || !large) return;
    if ((uint32_t)(now - slot->stamp) >= PSX_HOST_RUMBLE_TTL) return;
    if (slot->small && !*small) *small = 0xFFu;
    if (slot->large > *large) *large = slot->large;
}

#endif /* PSX_HOST_RUMBLE_H */
