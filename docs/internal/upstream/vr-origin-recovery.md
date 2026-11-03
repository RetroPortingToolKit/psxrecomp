# Report ambiguous XR origin diagnostics without recentering

Source: [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp).
- [96981469](https://github.com/FractalEngineer/psxrecomp/commit/96981469c4a3de0b31c3d2a282efdc7892f81361)

Original author: Yves <calibratedbeats@gmail.com>; retained as commit author
and Co-authored-by on this reconstructed port.

Adapted: diagnostic up-dot and sustained-inversion reporting. Automatic
recentering was deliberately excluded because a valid sustained roll is
indistinguishable from a corrupt origin in the relative quaternion. Explicit
recenter and OpenXR reference-space change events retain ownership of origin
changes.
Base: `18d52c8bfbe5afc380292ef3116af2920225a92e` (current upstream master or the preceding feature).

Validation: Windows/GCC 13.2 Release: the XR input fixture covers upright,
ordinary yaw, ten-frame ambiguous inversion, valid sustained roll, invalid
tracking reset, and reference-change timing. Physical reconnect is not retested.

Consumer: [Medal of Honor v0.1.6](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/1957e28a5dc7f5daa79e2b9b6ed867eb06b41ace).
Requires a framework pin bump and rebuild; no game/BIOS regeneration.
