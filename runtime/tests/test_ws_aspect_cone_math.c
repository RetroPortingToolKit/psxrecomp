#include "ws_aspect_cone_math.h"

#include <stdio.h>

static int failures;

static void check(int condition, const char *name) {
    if (condition) {
        printf("PASS  %s\n", name);
    } else {
        fprintf(stderr, "FAIL  %s\n", name);
        failures++;
    }
}

int main(void) {
    /* Camera looking along +X, with an approximately 30-degree vanilla cone. */
    const int32_t q12 = 4096;
    const uint32_t cos_q10 = 880;

    check(psx_ws_aspect_cone_contains(
              1000, 500, 0, q12, 0, 0, cos_q10, 0),
          "vanilla horizontal interior is accepted");
    check(!psx_ws_aspect_cone_contains(
              1000, 650, 0, q12, 0, 0, cos_q10, 0),
          "vanilla horizontal exterior is rejected");
    check(psx_ws_aspect_cone_contains(
              1000, 650, 0, q12, 0, 0, cos_q10, 80),
          "wide extent expands horizontal acceptance");
    check(!psx_ws_aspect_cone_contains(
              1000, 0, 650, q12, 0, 0, cos_q10, 80),
          "wide extent preserves the vertical boundary");
    check(!psx_ws_aspect_cone_contains(
              -1000, 0, 0, q12, 0, 0, cos_q10, 160),
          "points behind the camera are rejected");

    /* A 45-degree pitched forward vector still produces orthogonal forward,
     * horizontal, and vertical tests. */
    check(psx_ws_aspect_cone_contains(
              1000, 0, 1000, 2896, 0, 2896, cos_q10, 0),
          "pitched camera accepts a point on its forward axis");
    check(!psx_ws_aspect_cone_contains(
              1000, 0, -1000, 2896, 0, 2896, cos_q10, 160),
          "horizontal expansion does not bypass pitched vertical rejection");

    /* Recorded Tomba 2 village camera in stored X,Y,Z order:
     * right=(3568,0,-2009), up=(-342,4037,-607), forward=(1980,696,3516).
     * Model EF788 delta=(4967,502,1916) lies at the wide right edge. Both
     * vectors must be reordered to semantic X,Z,Y before using this helper. */
    check(!psx_ws_aspect_cone_contains(
              4967, 502, 1916, 1980, 696, 3516, 856, 120),
          "legacy storage interpretation reproduces the village rejection");
    check(psx_ws_aspect_cone_contains(
              4967, 1916, 502, 1980, 3516, 696, 856, 120),
          "mapped village camera accepts its horizontal edge actor");
    check(!psx_ws_aspect_cone_contains(
              4967, 1916, 502, 1980, 3516, 696, 856, 0),
          "same actor remains outside the unexpanded cone");
    check(psx_ws_aspect_cone_contains(
              1980, 3516, 696, 1980, 3516, 696, 856, 0),
          "mapped pitched village camera accepts its forward axis");
    check(!psx_ws_aspect_cone_contains(
              1296, 2302, 8770, 1980, 3516, 696, 856, 120),
          "mapped village camera still rejects a point above its vertical cone");

    check(!psx_ws_aspect_cone_contains(
              1000, 0, 0, q12, 0, 0, 0, 0),
          "invalid zero threshold is inert");
    check(!psx_ws_aspect_cone_contains(
              1000, 0, 0, 0, 0, q12, cos_q10, 0),
          "degenerate horizontal camera basis is inert");

    check(psx_ws_widen_angle_q12(341, 0) == 341,
          "terrain angle is exact at 4:3");
    check(psx_ws_widen_angle_q12(341, 136) == 533,
          "terrain angle follows 21:9 plus guard geometry");
    check(psx_ws_widen_angle_q12(455, 136) == 651,
          "wider vanilla terrain angle scales geometrically");
    check(psx_ws_widen_angle_q12(300, 136) == 484,
          "lava terrain angle uses the same projection geometry");
    check(psx_ws_widen_angle_q12(318, 80) <
              psx_ws_widen_angle_q12(318, 136),
          "terrain angle tracks dynamic horizontal extent");
    check(psx_ws_widen_angle_q12(1023, 256) <= 1023,
          "terrain angle remains below a quarter turn");

    return failures ? 1 : 0;
}
