# Add debug synthetic head translation and pitch

Source: [FractalEngineer/psxrecomp](https://github.com/FractalEngineer/psxrecomp).
- [2ed12640](https://github.com/FractalEngineer/psxrecomp/commit/2ed12640adab3602acbf3307d42e9c2739b7d0eb)
- [a3714fad](https://github.com/FractalEngineer/psxrecomp/commit/a3714fada4d0582500001b0c22e4dfa00840403e)

Original author: Yves <calibratedbeats@gmail.com>; retained as commit author
and Co-authored-by on this reconstructed port.

Adapted: this feature and its tests. Excluded: other VR features, game policy,
assets and chronological investigation notes.
Base: `e4f1f985428101eb6c8f83fce834e281860b1fd2` (current upstream master or the preceding feature).

Validation: Windows/GCC 13.2 Release: XR-off input fixture passes, including finite/range rejection, binocular translation/pitch, disabled state and clearing; command index is current.

Consumer: [Medal of Honor v0.1.6](https://github.com/FractalEngineer/Medal-Of-Honor-PSX-Recomp/tree/1957e28a5dc7f5daa79e2b9b6ed867eb06b41ace).
Requires a framework pin bump and rebuild; no game/BIOS regeneration.
