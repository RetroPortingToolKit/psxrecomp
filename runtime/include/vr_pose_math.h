#pragma once
#include "mod_plugins.h"
/* XR right-handed X right/Y up/Z back; PSX X right/Y down/Z forward. */
int vr_pose_to_view(const double q[4], const double p[3],
                    const double origin_q[4], const double origin_p[3],
                    const double fov[4], double units_per_meter,
                    int width, int height, PSXModRenderView *out);
/* Pose-to-recentered-PSX-camera transform, inverse to the view transform.
 * Row-major rotation maps PSX local axes; translation uses game camera units.
 * This does not apply any game's body/world matrix or aiming policy. */
int vr_pose_to_transform(const double q[4],const double p[3],
                         const double origin_q[4],const double origin_p[3],
                         double units_per_meter,double rotation[9],double translation[3]);
