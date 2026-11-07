#pragma once

#include <stdint.h>

#define PSX_VR_GEOMETRY_UNKNOWN (-1)
#define PSX_VR_GEOMETRY_OUTSIDE 0
#define PSX_VR_GEOMETRY_VISIBLE 1

typedef struct PSXVRMatrixInversePolicy {
    double min_abs_determinant;
    double max_abs_determinant;
    double max_abs_coefficient;
} PSXVRMatrixInversePolicy;

/*
 * Pure row-major 3x3 numeric operations shared by title VR adapters. Policies
 * and camera/view encodings remain in the callers. All output pointers are
 * written only on success.
 */
int psx_vr_inverse3x3(const double matrix[9],
                      const PSXVRMatrixInversePolicy* policy,
                      double inverse[9]);

/*
 * Conservative eye-space AABB test. The function composes a model transform
 * and an eye transform in the same order as the title adapters, then rejects a
 * box only when all eight corners are outside one screen/camera plane.
 * PSX_VR_GEOMETRY_UNKNOWN means malformed, non-finite or saturated input; the
 * caller chooses its native/conservative fallback.
 */
int psx_vr_box_visible(const int16_t bounds[6],
                       const double model_rotation[9],
                       const double model_translation[3],
                       const double eye_rotation[9],
                       const double eye_translation[3],
                       double fx, double fy, double cx, double cy,
                       int width, int height);

/*
 * Slab test for origin + t*direction within [tmin,tmax]. The entry distance is
 * clamped to tmin when the origin is inside. Invalid pointers, non-finite
 * numbers, reversed ranges and reversed bounds are rejected without writing
 * t_enter.
 */
int psx_vr_ray_box(const double origin[3], const double direction[3],
                   const int16_t bounds[6], double tmin, double tmax,
                   double* t_enter);
