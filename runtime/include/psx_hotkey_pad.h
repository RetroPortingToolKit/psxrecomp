#pragma once
/* Host controller shortcut bindings (launcher "assist" binds), pure logic.
 *
 * Encodings (shared with recomp-ui): 0 unbound; 1..99 one button (code+1),
 * always chorded with Select (legacy); 100..999 an axis direction, chorded
 * with Select; >= 1000 a button combination (1000 + mask of SDL
 * GameController button codes), every button held.
 *
 * A ONE-button combination is a direct shortcut only while the running title
 * allows it for that shortcut (psx_mod_allow_direct_shortcut); otherwise it
 * means Select + that button, like the legacy one-button encoding. A direct
 * shortcut claims its host button: the button is removed from the guest pad
 * while claimed and, once claimed while held, until it is released. */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_HOTKEY_PAD_COMBO_BASE 1000

typedef int (*PsxHotkeyButtonDown)(void *ctx, int sdl_button);
typedef int (*PsxHotkeyAxisValue)(void *ctx, int sdl_axis);

/* SDL button code of a one-button combination, else -1. */
static inline int psx_hotkey_pad_single_button(int binding) {
    if (binding < PSX_HOTKEY_PAD_COMBO_BASE) return -1;
    const uint32_t mask = (uint32_t)(binding - PSX_HOTKEY_PAD_COMBO_BASE);
    if (!mask || (mask & (mask - 1u))) return -1;
    int code = 0;
    while (!(mask & (1u << code))) code++;
    return code;
}

/* Whether `binding` is held. `back` is the Select (Back) button code. */
static inline int psx_hotkey_pad_down(int binding, int direct_allowed, int back,
                                      PsxHotkeyButtonDown button_down,
                                      PsxHotkeyAxisValue axis_value, void *ctx) {
    if (binding <= 0) return 0;
    if (binding >= PSX_HOTKEY_PAD_COMBO_BASE) {
        const uint32_t mask = (uint32_t)(binding - PSX_HOTKEY_PAD_COMBO_BASE);
        if (!mask) return 0;
        if (psx_hotkey_pad_single_button(binding) >= 0 && !direct_allowed &&
            !button_down(ctx, back))
            return 0;
        for (int code = 0; code < 32; ++code)
            if ((mask & (1u << code)) && !button_down(ctx, code)) return 0;
        return 1;
    }
    if (!button_down(ctx, back)) return 0;
    if (binding < 100) return button_down(ctx, binding - 1);
    {
        const int axis = (binding - 100) / 2;
        const int v = axis_value ? axis_value(ctx, axis) : 0;
        return ((binding - 100) & 1) ? (v > 16000) : (v < -16000);
    }
}

/* Whether the direct allowance for `shortcut` is in force: allowed by the
 * title and, for Rewind (`is_rewind`), Rewind enabled and not blocked by the
 * title (psx_mod_set_rewind_blocked). When not in force the button is not
 * claimed and a one-button binding falls back to Select + button. */
static inline int psx_hotkey_direct_active(uint32_t allowed_mask, int shortcut,
                                           int is_rewind, int rewind_enabled,
                                           int rewind_blocked) {
    if (shortcut < 0 || shortcut >= 32 || !(allowed_mask & (1u << shortcut)))
        return 0;
    if (is_rewind && (!rewind_enabled || rewind_blocked)) return 0;
    return 1;
}

/* Per-frame claim of direct-shortcut host buttons. `claimed` is the mask of
 * buttons claimed this frame, `held` the buttons physically held. Returns
 * the mask to remove from the guest pad; *latched carries a claim through
 * release after the shortcut stops claiming the button. */
static inline uint32_t psx_hotkey_claim_update(uint32_t *latched,
                                               uint32_t claimed, uint32_t held) {
    *latched = (*latched | claimed) & held;
    return claimed | *latched;
}

#ifdef __cplusplus
}
#endif
