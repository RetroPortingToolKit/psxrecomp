#include "vr_geometry_math.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static int same(double a, double b, double tolerance) {
    return isfinite(a) && isfinite(b) && fabs(a - b) <= tolerance;
}

static int multiply_is_identity(const double matrix[9],
                                const double inverse[9]) {
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            double value = 0.0;
            for (int k = 0; k < 3; ++k) {
                value += matrix[row * 3 + k] * inverse[k * 3 + col];
            }
            if (!same(value, row == col ? 1.0 : 0.0, 1e-9)) {
                return 0;
            }
        }
    }
    return 1;
}

int main(void) {
    const double matrix[9] = {
        1.0, 0.25, -0.5,
        0.0, 1.5, 0.75,
        0.5, -0.25, 2.0,
    };
    const PSXVRMatrixInversePolicy title_policy = {0.01, 64.0, 8.0};
    double inverse[9];
    CHECK(psx_vr_inverse3x3(matrix, &title_policy, inverse));
    CHECK(multiply_is_identity(matrix, inverse));

    const PSXVRMatrixInversePolicy no_upper_determinant = {
        0.01, INFINITY, 64.0,
    };
    const double high_determinant[9] = {5, 0, 0, 0, 5, 0, 0, 0, 5};
    CHECK(!psx_vr_inverse3x3(
        high_determinant, &title_policy, inverse));
    CHECK(psx_vr_inverse3x3(
        high_determinant, &no_upper_determinant, inverse));
    CHECK(multiply_is_identity(high_determinant, inverse));

    const double coefficient_reject[9] = {
        0.01, 0, 0, 0, 10, 0, 0, 0, 1,
    };
    double untouched[9];
    for (int i = 0; i < 9; ++i) {
        untouched[i] = 17.0;
    }
    CHECK(!psx_vr_inverse3x3(
        coefficient_reject, &no_upper_determinant, untouched));
    for (int i = 0; i < 9; ++i) {
        CHECK(untouched[i] == 17.0);
    }
    const double singular[9] = {0};
    CHECK(!psx_vr_inverse3x3(singular, &title_policy, untouched));
    const double nonfinite[9] = {1, 0, 0, 0, NAN, 0, 0, 0, 1};
    CHECK(!psx_vr_inverse3x3(nonfinite, &title_policy, untouched));

    const double identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    const double zero3[3] = {0, 0, 0};
    const int16_t forward[6] = {-10, -10, 90, 10, 10, 110};
    const int16_t behind[6] = {-10, -10, -110, 10, 10, -90};
    const int16_t side[6] = {390, -10, 90, 410, 10, 110};
    const int16_t crossing[6] = {-1000, -1000, -10, 1000, 1000, 1000};
    CHECK(psx_vr_box_visible(
              forward, identity, zero3, identity, zero3,
              256, 256, 256, 120, 512, 240) ==
          PSX_VR_GEOMETRY_VISIBLE);
    CHECK(psx_vr_box_visible(
              behind, identity, zero3, identity, zero3,
              256, 256, 256, 120, 512, 240) ==
          PSX_VR_GEOMETRY_OUTSIDE);
    CHECK(psx_vr_box_visible(
              side, identity, zero3, identity, zero3,
              256, 256, 256, 120, 512, 240) ==
          PSX_VR_GEOMETRY_OUTSIDE);
    CHECK(psx_vr_box_visible(
              crossing, identity, zero3, identity, zero3,
              256, 256, 256, 120, 512, 240) ==
          PSX_VR_GEOMETRY_VISIBLE);
    double rotated[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    rotated[0] = rotated[8] = -1.0;
    CHECK(psx_vr_box_visible(
              behind, identity, zero3, rotated, zero3,
              256, 256, 256, 120, 512, 240) ==
          PSX_VR_GEOMETRY_VISIBLE);
    const double shifted[3] = {-400, 0, 0};
    CHECK(psx_vr_box_visible(
              side, identity, zero3, identity, shifted,
              256, 256, 256, 120, 512, 240) ==
          PSX_VR_GEOMETRY_VISIBLE);
    const double saturated[3] = {40000, 0, 0};
    CHECK(psx_vr_box_visible(
              forward, identity, saturated, identity, zero3,
              256, 256, 256, 120, 512, 240) ==
          PSX_VR_GEOMETRY_UNKNOWN);
    CHECK(psx_vr_box_visible(
              NULL, identity, zero3, identity, zero3,
              256, 256, 256, 120, 512, 240) ==
          PSX_VR_GEOMETRY_UNKNOWN);

    const int16_t box[6] = {-10, -10, 100, 10, 10, 120};
    const double origin[3] = {0, 0, 0};
    const double direction[3] = {0, 0, 1};
    double entry = 999.0;
    CHECK(psx_vr_ray_box(origin, direction, box, 0, 1000, &entry));
    CHECK(entry == 100.0);
    const double inside[3] = {0, 0, 110};
    CHECK(psx_vr_ray_box(inside, direction, box, 5, 20, &entry));
    CHECK(entry == 5.0);
    const double parallel[3] = {1, 0, 0};
    CHECK(psx_vr_ray_box(inside, parallel, box, 0, 1000, &entry));
    CHECK(entry == 0.0);
    CHECK(!psx_vr_ray_box(origin, direction, box, 50, 10, &entry));

    const int16_t reversed[6] = {10, -10, 100, -10, 10, 120};
    CHECK(!psx_vr_ray_box(origin, direction, reversed, 0, 1000, &entry));
    const double nonfinite_origin[3] = {NAN, 0, 0};
    CHECK(!psx_vr_ray_box(
        nonfinite_origin, direction, box, 0, 1000, &entry));
    entry = 17.0;
    CHECK(!psx_vr_ray_box(origin, direction, box, 0, 1000, NULL));

    puts("PASS: shared VR inverse, conservative visibility and slab geometry");
    return 0;
}
