#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test whether a camera-relative vector is inside a rectangular view cone.
 *
 * Arguments use semantic X,Z,Y ordering: X/Z span the ground plane and Y is
 * world vertical. Map the title's stored components to those axes before
 * calling. The camera forward vector is Q12.
 * `threshold` is the game's Q10 cosine threshold (the SLTI immediate after its
 * normalized dot product). `extent_pixels` expands only the horizontal
 * half-angle from the vanilla 160-pixel half-width; the vertical half-angle is
 * unchanged.
 */
int psx_ws_aspect_cone_contains(int32_t x, int32_t z, int32_t y,
                                int32_t fx, int32_t fz, int32_t fy,
                                uint32_t threshold, int extent_pixels);

/* Widen a positive PSX 12-bit angular half-extent by the same horizontal
 * projection scale used by a 320-wide 4:3 viewport. `extent_pixels` is the
 * extra per-side reveal (including any configured guard). Identity is exact
 * when extent_pixels is zero. Results stay below a 90-degree half-angle. */
uint32_t psx_ws_widen_angle_q12(uint32_t vanilla, int extent_pixels);

#ifdef __cplusplus
}
#endif
