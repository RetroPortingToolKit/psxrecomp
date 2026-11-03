# Capture both HUD mattes with one checkpoint and readback

Source: [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp).
- [a876bae3](https://github.com/FractalEngineer/psxrecomp/commit/a876bae3385a8b972d8a5cf083ad5e30cf05c4bd)
- [d2554481](https://github.com/FractalEngineer/psxrecomp/commit/d2554481fac39409cebef7602e59bd155e000fe6)

Original author: Yves <calibratedbeats@gmail.com>; retained as commit author
and Co-authored-by on this reconstructed port.

Adapted: this feature and its tests. Excluded: other VR features, game policy,
assets and chronological investigation notes.
Base: `4419d234ab66c839da94dba597794ac47a09eb00` (current upstream master or the preceding feature).

Validation: Windows/GCC 13.2 Release: sandbox and abort tests pass (2/2), including both matte buffers and capture-only checkpoint reuse.

Consumer: [Medal of Honor v0.1.6](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/1957e28a5dc7f5daa79e2b9b6ed867eb06b41ace).
Requires a framework pin bump and rebuild; no game/BIOS regeneration.
