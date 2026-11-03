# Reuse one guest checkpoint per stereo pair

Source: [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp).
- [063b082a](https://github.com/FractalEngineer/psxrecomp/commit/063b082ae959488436596d90e6933d825e22e780)

Original author: Yves <calibratedbeats@gmail.com>; retained as commit author
and Co-authored-by on this reconstructed port.

Adapted: this feature and its tests. Excluded: other VR features, game policy,
assets and chronological investigation notes.
Base: `0cdf95957c3e34916aef5ff5b0b4e2b77fdc8790` (current upstream master or the preceding feature).

Validation: Windows/GCC 13.2 Release: trace, sandbox and abort tests pass (3/3), including shared-baseline state and per-eye precision arms.

Consumer: [Medal of Honor v0.1.6](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/1957e28a5dc7f5daa79e2b9b6ed867eb06b41ace).
Requires a framework pin bump and rebuild; no game/BIOS regeneration.
