# Preserve precise eye poses and clip near-camera geometry

Source: [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp).
- [efa10eee](https://github.com/FractalEngineer/psxrecomp/commit/efa10eee331bf9d6c5ab4c7ea987bc80e434fa9a)

Original author: Yves <calibratedbeats@gmail.com>; retained as commit author
and Co-authored-by on this reconstructed port.

Adapted: this feature and its tests. Projective PGXP mode opens after the
precision checkpoint and closes before rollback, preserving the real PGXP
generation and prior enabled/CPU-mode state through success and abort.
Excluded: other VR features, game policy, assets and chronological
investigation notes.
Base: `36f1ff3e905b4437cb2933c4f107f7dbe1c825e2` (current upstream master or the preceding feature).

Validation: Windows/GCC 13.2 Release: precise pose, GTE projective/facing, GPU
projection/provenance, real PGXP scope rollback, sandbox and abort tests pass;
production GPU-history guard passes. Prior Quest 3/VDXR acceptance applies to
the source fork, not this reconstructed branch.

Consumer: [Medal of Honor v0.1.6](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/1957e28a5dc7f5daa79e2b9b6ed867eb06b41ace).
Requires a framework pin bump and rebuild; no game/BIOS regeneration.
