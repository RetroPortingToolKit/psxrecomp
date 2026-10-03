# Experimental Win32/OpenGL OpenXR backend

Build with `-DPSX_OPENXR=ON` to opt in. The default is OFF: ordinary builds
do not fetch the SDK or require an active headset runtime. The enabled build
uses official OpenXR-SDK annotated tag object `b76b80adaf65ac3ad6cc1ce61974fb29a5d02352`
and currently supports Win32/OpenGL. This is a PC headset backend; it does not
provide an Android/standalone Quest build.

## Frame ownership and stereo submission

The game plugin enables the backend with `psx_mod_openxr_enable`, then calls
`psx_mod_openxr_begin(width,height,units_per_meter)` at a host frame boundary.
Begin waits/begins the XR frame and locates both eyes at its predicted time.
`psx_mod_openxr_view` returns the scoped rigid transform and asymmetric
projection for each eye. The plugin supplies its own draw-only callbacks to
the [atomic stereo renderer](STEREO_RENDERING.md).

`psx_mod_openxr_end(pair_rendered)` submits only the fresh complete pair from
that frame. A failed, refused or shed redraw submits zero layers, closing the
frame without recycling an old pair. Private swapchains receive eye textures;
guest execution, simulation time and guest TR registers are not owned by XR.
`psx_mod_openxr_recenter` establishes the shared pose origin. World scale,
native camera/body mapping, aiming and gameplay policy belong to the plugin.

## Offline actions and controller snapshots

Touch stick, trigger, squeeze and click actions are sampled at the normal
offline input boundary, never during eye replay. `psx_mod_openxr_input` exposes
activity/focus, sequence and synthetic metadata. Unfocused/inactive actions
are neutral; the game chooses how to map them through the separate trusted
controller-source API. Default flat input registers no XR source.

`psx_mod_openxr_hands` is read-only and safe during replay. It reports grip/aim
poses for both hands, activity, OpenXR validity/tracking flags, common origin,
predicted time, sequence and age. A cached pose is not a promise that tracking
is currently valid. Focus loss, disable and shutdown clear samples. Relative
controller transforms use the same metric pose math and origin as the eyes.

TCP `openxr_stats`, `openxr_views`, `openxr_input` and `openxr_hands` expose the
backend. `openxr_control` enables/recenters it. Synthetic `openxr_input_override`
and `openxr_hands_override` exist only with debug tools enabled and label their
samples. Their handlers and registrations are excluded by PSX_NO_DEBUG_TOOLS.

## Menus and native startup surfaces

`psx_mod_openxr_quad(distance_m,width_m,height_m)` chooses a frame-local menu
quad after pair begin. The request resets each frame; normal gameplay resumes
projection submission. Applications choose when their identical-eye menus
should become a comfortable quad. Distance must be 0.25..20m and dimensions
positive/bounded; values must be finite.

`psx_mod_openxr_native_surface(distance_m,width_m,units_per_meter)` permits
boot/menu/video presentation before any gameplay stereo hook. It persists
until disabled with distance zero. Width is positive and at most 10m, scale
1..65536, distance 0.25..20m; values must be finite. It refuses during replay,
an open XR frame, or compiled-out XR. Disable it before application pair begin.

Only a fresh native present arms submission. The letterboxed GL_BACK rectangle
is copied before host OSD, upright, to a VIEW-space quad preserving its aspect.
This path does not replay guest draws. Unchanged native frames still pump XR.
`openxr_stats.submitted_source` distinguishes stereo (1) and native (2).
Successful-submit counters and latched frame/cycle metadata describe the last
submission; they do not prove the layer currently visible. `video_info` exposes
the actual GL swap interval. The plugin can avoid a second desktop wait while
keeping the runtime's guest-speed deadline.

## Validation and limits

The original MoH alpha was accepted on Quest 3 via Virtual Desktop/VDXR for
startup/menu visibility, head tracking and mapped combat input. Menu quads and
native startup surfaces were exercised. This is historical acceptance of the
alpha checkpoint, not a new headset run of this rebased branch.

Fresh extraction checks cover action lifecycle/neutralization, pose math,
paired watchdog rollback, GTE identity/parallax/rigid/asymmetric projection,
and XR/debug-tool compile combinations. Controlled focus/reconnection tests,
other runtimes/headsets and fresh rebased headset acceptance remain pending.
Guest-frame replay cost does not guarantee headset-rate rendering. No claim
of independent headset-rate simulation, controller/barrel alignment or Android
support is made. The projection-scale prerequisite is runtime-only; no retail BIOS
regeneration is required. Bundled OpenBIOS is suitable for enabled builds.

The pin resolves to SDK commit `c15d38cb4bb10a5b7e075f74493ff13896e2597a`.
See [Third-Party Attribution](../THIRD_PARTY_ATTRIBUTION.md#openxr-sdk-loader---optional-pc-headset-backend)
for the Apache-2.0 SDK, MIT JsonCpp notices, cold-cache requirements and package
distribution. Consuming this feature requires a framework pin update and runtime
rebuild; it does not change guest code generation.
