# TCP guest instruction disassembly: source and validation

Reconstructed from [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp), developed on
`vr-dev` at `9976567eb8e95510ca7eeeeccd9af3a260885de5`.
Exact source commits:

- [85c47e10](https://github.com/FractalEngineer/psxrecomp/commit/85c47e10a2fa6e112205e0e953a89ba120c4b139)
- [d58db909](https://github.com/FractalEngineer/psxrecomp/commit/d58db9092a57cc692d0c9d936f5003fb4b3e0977)
- [c2ed6e55](https://github.com/FractalEngineer/psxrecomp/commit/c2ed6e558f37212c36aedfdfa2c90863883f0aff)

Original author: Yves <calibratedbeats@gmail.com>. The feature commit retains
that exact human author identity. The focused port consolidates the selected
source hunks and corrections. Its commit also cites these sources and carries
the original human contributor's `Co-authored-by: Yves <calibratedbeats@gmail.com>` trailer.

Adapted: The C-compatible bridge to the existing MIPS decoder, bounded TCP handler, build linkage and command docs. Formatting was corrected and covered during reconstruction.

Deliberately excluded: Guest instruction execution/codegen changes, new decoders, game-specific addresses and runtime printf debugging.

The feature was reconstructed on upstream master
`db62eec15f81c88aef3d6dab51d4e566a7167d23`, retaining current upstream APIs,
renderer/input policy and build integration. Validation after reconstruction:
Disassembly operand/target/coprocessor/buffer regressions, debug-tool ON/OFF server compilation and regenerated TCP index.

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

Release disassembly tests passed for arithmetic, multiply/divide, shifts, branch/jump targets, immediates, COP0/COP2, unknown words and bounded buffers. Checks execute with NDEBUG enabled. Full runtime builds passed with debug tools enabled and disabled; the generated command index passes.

Integration: [Medal of Honor, SLUS-00974 NTSC-U](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/e323885958c2ae0b0df847bed6175f9e76524442), built against the corrected framework stack with `PSXRECOMP_ROOT`. Bundled OpenBIOS LLE cold boot, menu navigation, mission gameplay, fresh savestate creation/loading and post-load input/frame advancement were checked with screenshots. An older alpha slot-5 savestate was refused; compatibility with historical states is not established. Memory-card round trips, attract-demo completion, other games and fresh headset visuals were not tested.

The registered recompiler suite reports 132 passes, four failures and three disabled tests. The four failures reproduce on unchanged upstream master in this Windows toolchain: `portable_settings_paths_test`, `optional_netplay_auth`, `compile_overlays_static_tail` and `aot_overlay_discovery`. The introduced BIOS-fingerprint and GL-guard failures are fixed.
