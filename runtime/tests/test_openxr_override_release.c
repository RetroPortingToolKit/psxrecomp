#include "psx_openxr.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); \
        return 1; \
    } \
} while (0)

int main(void) {
    PSXModOpenXRInput input = {0};
    input.struct_size = sizeof input;
    input.focused = 1;
    input.buttons[0] = PSX_MOD_XR_MENU;
    input.buttons_active[0] = PSX_MOD_XR_MENU;
    CHECK(psx_openxr_input_override(&input));

    PSXModOpenXRInput got_input = {0};
    got_input.struct_size = sizeof got_input;
    CHECK(psx_openxr_input(&got_input));
    CHECK(got_input.synthetic && got_input.focused);
    CHECK(got_input.buttons[0] == PSX_MOD_XR_MENU);

    PSXModOpenXRHands hands = {0};
    hands.struct_size = sizeof hands;
    hands.focused = 1;
    hands.origin_valid = 1;
    hands.origin_orientation_xyzw[3] = 1;
    hands.pose[1][PSX_MOD_XR_GRIP_POSE].active = 1;
    hands.pose[1][PSX_MOD_XR_GRIP_POSE].flags = 15;
    hands.pose[1][PSX_MOD_XR_GRIP_POSE].position_m[0] = .18f;
    hands.pose[1][PSX_MOD_XR_GRIP_POSE].orientation_xyzw[3] = 1;
    hands.pose[1][PSX_MOD_XR_AIM_POSE].active = 1;
    hands.pose[1][PSX_MOD_XR_AIM_POSE].flags = 15;
    hands.pose[1][PSX_MOD_XR_AIM_POSE].position_m[0] = .18f;
    hands.pose[1][PSX_MOD_XR_AIM_POSE].orientation_xyzw[3] = 1;
    CHECK(psx_openxr_hands_override(&hands));

    PSXModOpenXRHands got_hands = {0};
    got_hands.struct_size = sizeof got_hands;
    CHECK(psx_openxr_hands(&got_hands));
    CHECK(got_hands.synthetic && got_hands.focused && got_hands.origin_valid);
    CHECK(got_hands.pose[1][PSX_MOD_XR_GRIP_POSE].active);
    CHECK(got_hands.pose[1][PSX_MOD_XR_AIM_POSE].active);

    psx_openxr_shutdown();
    memset(&got_input, 0, sizeof got_input);
    got_input.struct_size = sizeof got_input;
    CHECK(psx_openxr_input(&got_input) && !got_input.synthetic);
    memset(&got_hands, 0, sizeof got_hands);
    got_hands.struct_size = sizeof got_hands;
    CHECK(psx_openxr_hands(&got_hands) && !got_hands.synthetic);
    return 0;
}
