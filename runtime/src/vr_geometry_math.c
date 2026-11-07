#include "vr_geometry_math.h"

#include <math.h>
#include <string.h>

static int finite9(const double values[9]) {
    for (int i = 0; i < 9; ++i) {
        if (!isfinite(values[i])) {
            return 0;
        }
    }
    return 1;
}

static int finite3(const double values[3]) {
    for (int i = 0; i < 3; ++i) {
        if (!isfinite(values[i])) {
            return 0;
        }
    }
    return 1;
}

int psx_vr_inverse3x3(const double matrix[9],
                      const PSXVRMatrixInversePolicy* policy,
                      double inverse[9]) {
    if (!matrix || !policy || !inverse || !finite9(matrix) ||
        !isfinite(policy->min_abs_determinant) ||
        policy->min_abs_determinant < 0.0 ||
        isnan(policy->max_abs_determinant) ||
        policy->max_abs_determinant <= 0.0 ||
        isnan(policy->max_abs_coefficient) ||
        policy->max_abs_coefficient <= 0.0) {
        return 0;
    }

    const double cofactor[9] = {
        matrix[4] * matrix[8] - matrix[5] * matrix[7],
        matrix[2] * matrix[7] - matrix[1] * matrix[8],
        matrix[1] * matrix[5] - matrix[2] * matrix[4],
        matrix[5] * matrix[6] - matrix[3] * matrix[8],
        matrix[0] * matrix[8] - matrix[2] * matrix[6],
        matrix[2] * matrix[3] - matrix[0] * matrix[5],
        matrix[3] * matrix[7] - matrix[4] * matrix[6],
        matrix[1] * matrix[6] - matrix[0] * matrix[7],
        matrix[0] * matrix[4] - matrix[1] * matrix[3],
    };
    const double determinant =
        matrix[0] * cofactor[0] + matrix[1] * cofactor[3] +
        matrix[2] * cofactor[6];
    if (!isfinite(determinant) ||
        fabs(determinant) < policy->min_abs_determinant ||
        fabs(determinant) > policy->max_abs_determinant) {
        return 0;
    }

    double result[9];
    for (int i = 0; i < 9; ++i) {
        result[i] = cofactor[i] / determinant;
        if (!isfinite(result[i]) ||
            fabs(result[i]) > policy->max_abs_coefficient) {
            return 0;
        }
    }
    memcpy(inverse, result, sizeof result);
    return 1;
}

int psx_vr_box_visible(const int16_t bounds[6],
                       const double model_rotation[9],
                       const double model_translation[3],
                       const double eye_rotation[9],
                       const double eye_translation[3],
                       double fx, double fy, double cx, double cy,
                       int width, int height) {
    if (!bounds || !model_rotation || !model_translation ||
        !eye_rotation || !eye_translation || width <= 0 || height <= 0 ||
        !finite9(model_rotation) || !finite3(model_translation) ||
        !finite9(eye_rotation) || !finite3(eye_translation) ||
        !isfinite(fx) || !isfinite(fy) || !isfinite(cx) || !isfinite(cy) ||
        fx <= 0.0 || fy <= 0.0) {
        return PSX_VR_GEOMETRY_UNKNOWN;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (bounds[axis] > bounds[axis + 3]) {
            return PSX_VR_GEOMETRY_UNKNOWN;
        }
    }

    unsigned inside = 0;
    for (int corner = 0; corner < 8; ++corner) {
        double world[3];
        double eye[3];
        for (int row = 0; row < 3; ++row) {
            world[row] = model_translation[row];
            for (int col = 0; col < 3; ++col) {
                world[row] += model_rotation[row * 3 + col] *
                    bounds[col + ((corner >> col) & 1) * 3];
            }
        }
        for (int row = 0; row < 3; ++row) {
            eye[row] = eye_translation[row];
            for (int col = 0; col < 3; ++col) {
                eye[row] += eye_rotation[row * 3 + col] * world[col];
            }
            if (!isfinite(eye[row])) {
                return PSX_VR_GEOMETRY_UNKNOWN;
            }
        }

        /* Saturation is not evidence that the whole node is outside. */
        if (fabs(eye[0]) >= 32760.0 || fabs(eye[1]) >= 32760.0 ||
            fabs(eye[2]) >= 65530.0) {
            return PSX_VR_GEOMETRY_UNKNOWN;
        }
        if (eye[2] >= -1.0) {
            inside |= 1u;
        }
        if (fx * eye[0] + (cx + 2.0) * eye[2] >= -2.0) {
            inside |= 2u;
        }
        if ((width - cx + 2.0) * eye[2] - fx * eye[0] >= -2.0) {
            inside |= 4u;
        }
        if (fy * eye[1] + (cy + 2.0) * eye[2] >= -2.0) {
            inside |= 8u;
        }
        if ((height - cy + 2.0) * eye[2] - fy * eye[1] >= -2.0) {
            inside |= 16u;
        }
    }
    return inside == 31u ? PSX_VR_GEOMETRY_VISIBLE
                         : PSX_VR_GEOMETRY_OUTSIDE;
}

int psx_vr_ray_box(const double origin[3], const double direction[3],
                   const int16_t bounds[6], double tmin, double tmax,
                   double* t_enter) {
    if (!origin || !direction || !bounds || !t_enter ||
        !finite3(origin) || !finite3(direction) ||
        !isfinite(tmin) || !isfinite(tmax) || tmin > tmax) {
        return 0;
    }
    double lo_t = tmin;
    double hi_t = tmax;
    for (int axis = 0; axis < 3; ++axis) {
        const double lo = bounds[axis];
        const double hi = bounds[axis + 3];
        if (lo > hi) {
            return 0;
        }
        if (fabs(direction[axis]) < 1e-9) {
            if (origin[axis] < lo || origin[axis] > hi) {
                return 0;
            }
            continue;
        }
        const double inverse = 1.0 / direction[axis];
        double a = (lo - origin[axis]) * inverse;
        double b = (hi - origin[axis]) * inverse;
        if (a > b) {
            const double swap = a;
            a = b;
            b = swap;
        }
        if (a > lo_t) {
            lo_t = a;
        }
        if (b < hi_t) {
            hi_t = b;
        }
        if (lo_t > hi_t) {
            return 0;
        }
    }
    *t_enter = lo_t;
    return 1;
}
