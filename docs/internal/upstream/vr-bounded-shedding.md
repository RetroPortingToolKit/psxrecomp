# Bound stereo shedding and recover at the display deadline

Source: [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp).
- [cae62891](https://github.com/FractalEngineer/psxrecomp/commit/cae62891e9236e9252e43b80aa9dd47f7554aea4)
- [b6a671c1](https://github.com/FractalEngineer/psxrecomp/commit/b6a671c1db4ba15d02369ce5561754d485881a55)

Original author: Yves <calibratedbeats@gmail.com>; retained as commit author
and Co-authored-by on this reconstructed port.

Adapted: this feature and its tests. Excluded: other VR features, game policy,
assets and chronological investigation notes.
Base: `8400b55dc02d3fa787df1280a9fb474901adfcf4` (current upstream master or the preceding feature).

Validation: Windows/GCC 13.2 Release: policy, sandbox and abort tests pass (3/3). Sustained overload remains expensive; the MoH launcher disables shedding by default.

Consumer: [Medal of Honor v0.1.6](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/1957e28a5dc7f5daa79e2b9b6ed867eb06b41ace).
Requires a framework pin bump and rebuild; no game/BIOS regeneration.
