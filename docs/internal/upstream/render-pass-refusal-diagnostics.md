# Render-pass refusal diagnostics: source and validation

Reconstructed from [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp), developed on
`vr-dev` at `9976567eb8e95510ca7eeeeccd9af3a260885de5`.
Exact source commits:

- [257a88b0](https://github.com/FractalEngineer/psxrecomp/commit/257a88b06d26bb99a9e6066b89f839af41090118)

Original author: Yves <calibratedbeats@gmail.com>. The feature commit retains
that exact human author identity. The focused port consolidates the selected
source hunks and corrections. Its commit also cites these sources and carries
the original human contributor's `Co-authored-by: Yves <calibratedbeats@gmail.com>` trailer.

Adapted: Refusal counters, latched GL/resource errors, TCP reporting and synthetic abort/sandbox coverage. The readiness source guard was updated for the preserved refusal chain.

Deliberately excluded: Stereo gates, OpenXR submission, game replay hooks, GL allocation-success policy changes and bulk historical captures.

The feature was reconstructed on upstream master
`db62eec15f81c88aef3d6dab51d4e566a7167d23`, retaining current upstream APIs,
renderer/input policy and build integration. Validation after reconstruction:
Abort/sandbox executables, render-pass source guards, GL internal-scale guards and regenerated TCP index.

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

Abort/sandbox regressions and the registered GL readiness guards pass. The updated guard follows the same refusal chain in ordinary and stereo builds. Integrated real-GL readback tests pass at 1x and 4x; injected driver out-of-memory behavior was not exercised.

Integration: [Medal of Honor, SLUS-00974 NTSC-U](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/e323885958c2ae0b0df847bed6175f9e76524442), built against the corrected framework stack with `PSXRECOMP_ROOT`. Bundled OpenBIOS LLE cold boot, menu navigation, mission gameplay, fresh savestate creation/loading and post-load input/frame advancement were checked with screenshots. An older alpha slot-5 savestate was refused; compatibility with historical states is not established. Memory-card round trips, attract-demo completion, other games and fresh headset visuals were not tested.

The registered recompiler suite reports 132 passes, four failures and three disabled tests. The four failures reproduce on unchanged upstream master in this Windows toolchain: `portable_settings_paths_test`, `optional_netplay_auth`, `compile_overlays_static_tail` and `aot_overlay_discovery`. The introduced BIOS-fingerprint and GL-guard failures are fixed.
