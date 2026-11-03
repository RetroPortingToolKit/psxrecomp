#pragma once

/* Observe an inverted relative head pose for diagnostics only. A sustained
 * roll is valid user motion and must never trigger an automatic recenter. */
static inline unsigned xr_origin_observe_inversion(unsigned streak, int valid,
                                                    const double origin[4],
                                                    const double pose[4],
                                                    double *up_dot) {
    if (!valid) {
        if (up_dot) *up_dot = 1.0;
        return 0;
    }
    double rx = origin[3]*pose[0] - origin[0]*pose[3] -
                origin[1]*pose[2] + origin[2]*pose[1];
    double rz = origin[3]*pose[2] - origin[0]*pose[1] +
                origin[1]*pose[0] - origin[2]*pose[3];
    double up = 1.0 - 2.0*(rx*rx + rz*rz);
    if (up_dot) *up_dot = up;
    if (up >= -0.5) return 0;
    return streak < 10u ? streak + 1u : streak;
}

/* Only the runtime's reference-space change event authorizes an origin reset. */
static inline int xr_origin_reference_reset_due(int pending,
                                                 long long change_time,
                                                 long long now) {
    return pending && now >= change_time;
}
