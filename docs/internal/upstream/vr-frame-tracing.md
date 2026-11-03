# VR frame tracing: source and validation

Adapted from [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp):
- [022eb1ed](https://github.com/FractalEngineer/psxrecomp/commit/022eb1edb2d6b2d9d505641e198bd9aeb5eb8ce1)
- [0e0d2de8](https://github.com/FractalEngineer/psxrecomp/commit/0e0d2de82777857248ae6c1bf7e9b7cba05e3d0a)
- [5c67dced](https://github.com/FractalEngineer/psxrecomp/commit/5c67dced708a67e905b77be455263009f566d4bb)
- [63271ce7](https://github.com/FractalEngineer/psxrecomp/commit/63271ce7b4e2fb5f616050e7d59a2e4642a33b55)
- [571a8f10](https://github.com/FractalEngineer/psxrecomp/commit/571a8f1082dbbc95af19e90ce0226a0cb40f302d)
- [87a3e822](https://github.com/FractalEngineer/psxrecomp/commit/87a3e8223cf7c05761326bfce06e80ffc527e132)

Original author: Yves <calibratedbeats@gmail.com>; retained as commit author
and Co-authored-by on this reconstructed port.

Adapted: opt-in trace ring, XR cadence, HUD timing, nested replay attribution,
exports and tests. Excluded: shedding policy, checkpoint reuse, game hooks and
chronological research. Rebuilt on upstream master `9caf4c22`.

Validation: Windows/GCC 13.2 Release: trace, source guard, sandbox, abort and
XR-off input tests pass (5/5); command index is current. XR-on integration is
checked separately after assembling the feature branches.
Consumer: [Medal of Honor v0.1.6](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/1957e28a5dc7f5daa79e2b9b6ed867eb06b41ace); pin bump and rebuild, no regeneration.
