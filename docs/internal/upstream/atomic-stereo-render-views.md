# Atomic stereo pairs and scoped views: source and validation

Reconstructed from [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp), developed on
`vr-dev` at `9976567eb8e95510ca7eeeeccd9af3a260885de5`.
Exact source commits:

- [0a955971](https://github.com/FractalEngineer/psxrecomp/commit/0a95597129cc2ee6d70397a8da9511831575eae4)
- [0a4cf21f](https://github.com/FractalEngineer/psxrecomp/commit/0a4cf21fac88ef425ba0edd25871d7dd20ee0903)
- [22bdc9e9](https://github.com/FractalEngineer/psxrecomp/commit/22bdc9e90d14f7a04b79c83b77611c4eb980fef4)
- [734977c9](https://github.com/FractalEngineer/psxrecomp/commit/734977c93e80789e9210a262daa35a88f41c40d4)

Original author: Yves <calibratedbeats@gmail.com>. The feature commit retains
that exact human author identity. The focused port consolidates the selected
source hunks and corrections. Its commit also cites these sources and carries
the original human contributor's `Co-authored-by: Yves <calibratedbeats@gmail.com>` trailer.

Adapted: Frozen-checkpoint paired rendering, private staging/atomic publication, scoped GTE rigid/asymmetric views, full-pose rollback and generic inverse pose math. TCP diagnostics and bounded capture tooling accompany the feature.

Deliberately excluded: OpenXR SDK/session/device code, producer-PC filtering, game replay callbacks/addresses and the reverted vertex-capture experiment.

The feature was reconstructed on upstream master
`db62eec15f81c88aef3d6dab51d4e566a7167d23`, retaining current upstream APIs,
renderer/input policy and build integration. Validation after reconstruction:
Paired abort/sandbox, GL refusal/source guards, pose/inverse-pose and GTE identity/parallax/rigid/asymmetric regressions.

The motivating consumer is [Medal of Honor, SLUS-00974 (NTSC-U)](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/e323885958c2ae0b0df847bed6175f9e76524442).
Game-specific integration remains in that repository. Consuming this change
requires a deliberate framework pin update in the game repo and a rebuild.
This is a host/runtime/tool change; it requires no BIOS or game C regeneration.

Historical alpha acceptance used the older framework checkpoint above. It is
separate from current-master branch verification. Fresh game, renderer and
headset checks, where applicable, are recorded in the PR validation notes;
uncertain hardware/runtime behavior must not be inferred from a clean link.

## Current-master integration validation

Windows, GCC 13.2, Ninja, Release. Runtime test checks remain active with NDEBUG enabled.

Release pair abort/sandbox, pose/inverse-pose and GTE identity/parallax/rigid/asymmetric tests pass. The actual OpenGL fixture passes all 197 checks at both 1x and 4x on an RTX 4070 Ti (NVIDIA OpenGL 4.6, driver 616.92), including eye texture pixels, private staging, atomic publication, VRAM restoration, refusal gates, retention of the preceding pair on failure, PNG capture and TCP metadata. Diagnostic manifests are written by host tooling from bounded TCP metadata. Fresh in-headset stereo acceptance remains pending.

Integration: [Medal of Honor, SLUS-00974 NTSC-U](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/e323885958c2ae0b0df847bed6175f9e76524442), built against the corrected framework stack with `PSXRECOMP_ROOT`. Bundled OpenBIOS LLE cold boot, menu navigation, mission gameplay, fresh savestate creation/loading and post-load input/frame advancement were checked with screenshots. An older alpha slot-5 savestate was refused; compatibility with historical states is not established. Memory-card round trips, attract-demo completion, other games and fresh headset visuals were not tested.

The registered recompiler suite reports 132 passes, four failures and three disabled tests. The four failures reproduce on unchanged upstream master in this Windows toolchain: `portable_settings_paths_test`, `optional_netplay_auth`, `compile_overlays_static_tail` and `aot_overlay_discovery`. The introduced BIOS-fingerprint and GL-guard failures are fixed.
