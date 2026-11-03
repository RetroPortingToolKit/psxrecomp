# Report XR readiness without the debug server

Source: [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp).
- [5932092c](https://github.com/FractalEngineer/psxrecomp/commit/5932092cdf20aaa707e286dbd840c63b1f0d0598)

Original author: Yves <calibratedbeats@gmail.com>; retained as commit author
and Co-authored-by on this reconstructed port.

Adapted: this feature and its tests. The readiness receipt uses same-directory
temporary publication, refuses an existing destination, records XR session
generations, and requires a submission from the current session after a
requested load succeeds. Each game-session reboot starts a fresh load lifecycle;
an earlier success cannot authorize the rebooted session. A failed, invalid, or
refused load publishes a non-ready receipt without exposing partial JSON.
Excluded: other VR features, game policy, assets and chronological
investigation notes.
Base: `fef7a1b6e3e1be7511eb82fa3b512258beacba04` (current upstream master or the preceding feature).

Validation: Windows/GCC 13.2 Release/OpenGL/OpenXR/PGXP MoH build passes with
debug tools OFF and existing UI. Atomic replacement is covered by a standalone
test; the readiness state-machine fixture covers success, XR re-init, reboot
with an old success, pending and failed reboot loads, recovery, and refused
requests. Fresh-boot headless readiness and missing-slot refusal pass. The older
slot-2 fixture failed to load on current upstream; no compatibility guard/header
was changed.

Consumer: [Medal of Honor v0.1.6](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/1957e28a5dc7f5daa79e2b9b6ed867eb06b41ace).
Requires a framework pin bump and rebuild; no game/BIOS regeneration.
