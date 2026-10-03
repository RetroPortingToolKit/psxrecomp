# Runtime projection-distance scaling: source and validation

Reconstructed from [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp), developed on
`vr-dev` at `9976567eb8e95510ca7eeeeccd9af3a260885de5`.
Exact source commits:

- [82695b75](https://github.com/FractalEngineer/psxrecomp/commit/82695b75d79610ca7a99783adf9dd5195edfcdd0)
- [5633e868](https://github.com/FractalEngineer/psxrecomp/commit/5633e86878611a14d3a78e8f9708452a07bc7238)
- [bbd01ccf](https://github.com/FractalEngineer/psxrecomp/commit/bbd01ccfc91c1d9ebe199dad071df64e5b13d06d)
- [39d478db](https://github.com/FractalEngineer/psxrecomp/commit/39d478db83132815c2c8c593fc719c4bf67301b0)

Original author: Yves <calibratedbeats@gmail.com>. The feature commit retains
that exact human author identity. The focused port consolidates the selected
source hunks and corrections. Its commit also cites these sources and carries
the original human contributor's `Co-authored-by: Yves <calibratedbeats@gmail.com>` trailer.

Adapted: Optional effective-H scaling and environment/config precedence. Unlike the source fork, [video] fov_scale is read by a runtime-only parser extension, leaving all emitter inputs unchanged. Environment/range/quantization validation and PGXP shadows were corrected.

Deliberately excluded: Shared recompiler-parser fields, BIOS stamp refreshes, stereo views, XR code and game camera/culling policy.

The feature was reconstructed on upstream master
`db62eec15f81c88aef3d6dab51d4e566a7167d23`, retaining current upstream APIs,
renderer/input policy and build integration. Validation after reconstruction:
GTE identity/numerical/environment/quantization regressions and both committed BIOS fingerprint checks. Runtime config-parser regression is registered with CTest.

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

Release GTE tests pass for identity, numerical projection, invalid ratios, finite/range/full-string environment parsing and quantization. A registered runtime-only TOML regression covers defaults, valid values and rejected values. PGXP shadows use effective H while guest H stays canonical. Both committed BIOS fingerprint checks pass: the shared recompiler parser and generated BIOS files are unchanged from master.

Integration: [Medal of Honor, SLUS-00974 NTSC-U](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/e323885958c2ae0b0df847bed6175f9e76524442), built against the corrected framework stack with `PSXRECOMP_ROOT`. Bundled OpenBIOS LLE cold boot, menu navigation, mission gameplay, fresh savestate creation/loading and post-load input/frame advancement were checked with screenshots. An older alpha slot-5 savestate was refused; compatibility with historical states is not established. Memory-card round trips, attract-demo completion, other games and fresh headset visuals were not tested.

The registered recompiler suite reports 132 passes, four failures and three disabled tests. The four failures reproduce on unchanged upstream master in this Windows toolchain: `portable_settings_paths_test`, `optional_netplay_auth`, `compile_overlays_static_tail` and `aot_overlay_discovery`. The introduced BIOS-fingerprint and GL-guard failures are fixed.
