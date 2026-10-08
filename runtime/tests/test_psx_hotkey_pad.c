/* Host shortcut binding semantics: legacy Select chords, multi-button combos,
 * one-button combos that are direct only when the title allows it, and the
 * claim-through-release mask for direct shortcut buttons. */
#include <stdio.h>
#include "psx_hotkey_pad.h"

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); return 1; } } while (0)

enum { A = 0, B = 1, BACK = 4, Y = 3, RS = 8, LEFTX = 0 };
static uint32_t held;
static int axis;
static int down(void *c, int code) { (void)c; return (held >> code) & 1u; }
static int axis_value(void *c, int a) { (void)c; return a == LEFTX ? axis : 0; }
static int combo(uint32_t mask) { return PSX_HOTKEY_PAD_COMBO_BASE + (int)mask; }
static int pressed(int binding, int direct) {
    return psx_hotkey_pad_down(binding, direct, BACK, down, axis_value, NULL);
}

int main(void) {
    CHECK(psx_hotkey_pad_single_button(combo(1u << Y)) == Y);
    CHECK(psx_hotkey_pad_single_button(combo((1u << BACK) | (1u << RS))) == -1);
    CHECK(psx_hotkey_pad_single_button(1 + Y) == -1);  /* legacy encoding */
    CHECK(psx_hotkey_pad_single_button(combo(0)) == -1);
    CHECK(psx_hotkey_pad_single_button(0) == -1);

    /* One-button combination: Select + Y unless direct is allowed. */
    held = 1u << Y;
    CHECK(!pressed(combo(1u << Y), 0));
    CHECK(pressed(combo(1u << Y), 1));
    held = (1u << Y) | (1u << BACK);
    CHECK(pressed(combo(1u << Y), 0));

    /* Multi-button combination: all held, direct flag irrelevant. */
    held = (1u << BACK) | (1u << RS);
    CHECK(pressed(combo((1u << BACK) | (1u << RS)), 0));
    held = 1u << RS;
    CHECK(!pressed(combo((1u << BACK) | (1u << RS)), 1));

    /* Legacy one-button and axis encodings always chord with Select. */
    held = 1u << Y;
    CHECK(!pressed(1 + Y, 1));
    held |= 1u << BACK;
    CHECK(pressed(1 + Y, 0));
    axis = -20000;
    CHECK(pressed(100 + LEFTX * 2 + 0, 0));
    CHECK(!pressed(100 + LEFTX * 2 + 1, 0));
    held = 0;
    CHECK(!pressed(100 + LEFTX * 2 + 0, 1));
    CHECK(!pressed(0, 1));

    /* Claims: a claimed button is suppressed; once claimed while held it
     * stays suppressed through release even after the claim ends. */
    {
        uint32_t latched = 0;
        CHECK(psx_hotkey_claim_update(&latched, 1u << Y, 0) == (1u << Y));
        CHECK(psx_hotkey_claim_update(&latched, 1u << Y, 1u << Y) == (1u << Y));
        CHECK(psx_hotkey_claim_update(&latched, 0, 1u << Y) == (1u << Y));
        CHECK(psx_hotkey_claim_update(&latched, 0, 1u << Y) == (1u << Y));
        CHECK(psx_hotkey_claim_update(&latched, 0, 0) == 0);
        CHECK(psx_hotkey_claim_update(&latched, 0, 1u << Y) == 0); /* fresh press */
        CHECK(psx_hotkey_claim_update(&latched, 0, 0) == 0 && latched == 0);
    }
    /* Direct allowance in force: title-allowed, and for Rewind only while
     * Rewind is enabled and the title has not blocked it. */
    {
        const uint32_t allowed = 1u << 0;  /* shortcut 0 (Rewind) */
        CHECK(psx_hotkey_direct_active(allowed, 0, 1, 1, 0));
        CHECK(!psx_hotkey_direct_active(allowed, 0, 1, 0, 0)); /* disabled */
        CHECK(!psx_hotkey_direct_active(allowed, 0, 1, 1, 1)); /* blocked */
        CHECK(!psx_hotkey_direct_active(0, 0, 1, 1, 0));       /* not allowed */
        CHECK(psx_hotkey_direct_active(1u << 2, 2, 0, 0, 1));  /* not Rewind */
        CHECK(!psx_hotkey_direct_active(~0u, -1, 0, 1, 0));
        CHECK(!psx_hotkey_direct_active(~0u, 32, 0, 1, 0));
        /* Blocked mid-press: the claim stops but the held button stays
         * suppressed until release, then reaches the guest. */
        uint32_t latched = 0;
        CHECK(psx_hotkey_claim_update(&latched, 1u << Y, 1u << Y) == (1u << Y));
        CHECK(psx_hotkey_claim_update(&latched, 0, 1u << Y) == (1u << Y));
        CHECK(psx_hotkey_claim_update(&latched, 0, 0) == 0);
        CHECK(psx_hotkey_claim_update(&latched, 0, 1u << Y) == 0);
        /* Not in force: a one-button binding needs Select, like legacy. */
        held = 1u << Y;
        CHECK(!pressed(combo(1u << Y), psx_hotkey_direct_active(allowed, 0, 1, 1, 1)));
    }
    fprintf(stderr, "psx_hotkey_pad: passed\n");
    return 0;
}
