# Experimental Win32/OpenGL OpenXR: source and validation

Reconstructed from [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp), developed on
`vr-dev` at `9976567eb8e95510ca7eeeeccd9af3a260885de5`.
Exact source commits:

- [22bdc9e9](https://github.com/FractalEngineer/psxrecomp/commit/22bdc9e90d14f7a04b79c83b77611c4eb980fef4)
- [5a1264b7](https://github.com/FractalEngineer/psxrecomp/commit/5a1264b79b69b5d75f7b8507e305b1e13a911a50)
- [ac6f84ba](https://github.com/FractalEngineer/psxrecomp/commit/ac6f84ba039b1d27bb1e93d3b6d05390dfa42ff9)
- [734977c9](https://github.com/FractalEngineer/psxrecomp/commit/734977c93e80789e9210a262daa35a88f41c40d4)
- [04714837](https://github.com/FractalEngineer/psxrecomp/commit/04714837245ebe68abbc462ac6822a3818eedf67)
- [5bafeebf](https://github.com/FractalEngineer/psxrecomp/commit/5bafeebf3f0c2eaaa38212f960b9f6f88351cb74)

Original author: Yves <calibratedbeats@gmail.com>. The feature commit retains
that exact human author identity. The focused port consolidates the selected
source hunks and corrections. Its commit also cites these sources and carries
the original human contributor's `Co-authored-by: Yves <calibratedbeats@gmail.com>` trailer.

Adapted: Only SDK dependency/build integration, headset frame lifecycle, Touch actions and tracked hands, synthetic debug controls, menu quads and native startup presentation. Debug-less guards, complete dependency notices and focused docs were corrected during the port.

Deliberately excluded: Producer-PC filtering, title-specific transforms/hook PCs/input policy, private saves/discs, unrelated chronology, Android support and headset-rate simulation.

The feature was reconstructed on upstream master
`db62eec15f81c88aef3d6dab51d4e566a7167d23`, retaining current upstream APIs,
renderer/input policy and build integration. Validation after reconstruction:
XR ON/OFF crossed with debug-tool ON/OFF C compilation, compiled-out action/hand lifecycle tests and the SDK loader native link. Historical Quest 3/Virtual Desktop VDXR acceptance is not fresh rebased headset evidence.

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

Full Release runtime builds and nine focused CTest cases pass for XR ON/OFF crossed with debug tools ON/OFF. The rebuilt Medal of Honor game links with XR ON, launcher UI OFF, rewind ON and netplay OFF. OpenBIOS LLE reaches flat gameplay after a fresh save/load. A live initialization attempt found VirtualDesktopXR, then xrGetSystem returned XR_ERROR_FORM_FACTOR_UNAVAILABLE (-35); the game continued in flat mode with neutral XR input. No headset layers were submitted. Quest 3/VDXR alpha acceptance remains historical; fresh visual headset, controlled focus/reconnection and other-runtime tests remain pending.

Integration: [Medal of Honor, SLUS-00974 NTSC-U](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/e323885958c2ae0b0df847bed6175f9e76524442), built against the corrected framework stack with `PSXRECOMP_ROOT`. Bundled OpenBIOS LLE cold boot, menu navigation, mission gameplay, fresh savestate creation/loading and post-load input/frame advancement were checked with screenshots. An older alpha slot-5 savestate was refused; compatibility with historical states is not established. Memory-card round trips, attract-demo completion, other games and fresh headset visuals were not tested.

The registered recompiler suite reports 132 passes, four failures and three disabled tests. The four failures reproduce on unchanged upstream master in this Windows toolchain: `portable_settings_paths_test`, `optional_netplay_auth`, `compile_overlays_static_tail` and `aot_overlay_discovery`. The introduced BIOS-fingerprint and GL-guard failures are fixed.
