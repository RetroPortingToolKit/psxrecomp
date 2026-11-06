# Frame generation replacement (in progress)

The legacy temporal crossfade, change-adaptive blend, duplicate-frame smoother,
heuristic draw-list interpolation and reprojection renderer are retired. Their
configuration keys and old activation APIs cannot enable them in this branch.
Removed algorithms and historical investigations remain in Git history.
Rejection-only ABI adapters and remaining inactive presenter plumbing still need
cleanup as consumers migrate; they are not a starting point for the replacement.

The new implementation begins with `runtime/include/presentation_scene.h` and
`runtime/src/presentation_scene.c`: bounded, owned scene/resource snapshots;
explicit identity/lifetime/topology; separate camera and object poses;
shortest-arc quaternion rotation; stable corresponding animated vertices.
It does not run guest code, blend images, infer correspondence or extrapolate.

Status: geometry/identity/resource/discontinuity/native-deadline CTests pass
(2/2), as does the geometry test on Linux/WSL. The full debug-enabled Ape
runtime builds, its catalog CTest passes, and isolated muted software/headless
boot advances from frame 191 to 904 and exits normally with all four old enable
environment switches set. Debug queries report interpolation disabled, no
captures/swaps, and frame generation retired. This is a retirement smoke test,
not gameplay or new-interpolation qualification. Ape now has a development-only,
read-only producer for skeletal/rigid projection tuples and bounded copies of
original ordering-table commands. Explicit vertex tokens travel through intact
CPU/GTE word copies using the address-keyed transport in `pgxp.cpp`; source
tracking works independently of precision correction, allocates its RAM token
table only when enabled, and never uses the position fallback. Arithmetic,
partial/untracked writes and generation changes discard tokens. Its unit checks
cover identical coordinates with different source IDs, copy lineage, rejection,
reset and checkpoint rollback. Original-disc software/headless capture exercises
both projection paths and complete polygon lineage, while leaving unknown
polygons explicitly untracked. This is source-transport qualification only.

The fresh `presentation_draw.c` renderer front end consumes sampled scenes,
clips in homogeneous coordinates before dividing by depth, preserves draw order
and resource indices, and prepares triangles for a separate presentation target.
Near/viewport clipping distinguishes affine PS1 texture coordinates and colour
from perspective-correct texture coordinates. Fixed capacities and invalid
coordinates refuse the entire draw stream. Its geometry-phase, clipping,
attribute, resource and failure-path tests pass; no GPU presentation is enabled
by this module. The Ape producer now also covers world quarter-turns, terrain
source indices and explicit parent recipes for recursive GT4 subdivision.

`presentation_gpu.h` and `presentation_gpu.inc` now provide a separate OpenGL
target and retained native-word texture versions. GPU-to-GPU copies use explicit
raw-mirror write epochs; referenced versions cannot be overwritten by cache
turnover. Separate program/VAO/buffer ownership and a complete state bracket
protect the native renderer. Ordinary PS1 texture/CLUT, transparency, masking and
dither shader semantics are shared with the rasterizer. Actual Ape retained-pair
phase exports passed same-command CPU/RAM/VRAM and native-image invariance.
Debug/export readback is separate from regular GPU capture and rendering.

The current GPU API serializes with the GL owner and renders a native-resolution
geometry reference. It is development integration, not the asynchronous 60 FPS
presenter. The Ape adapter can sample explicit world-root/camera/local-vertex
bindings into this target. Its first phase exports expose visible terrain and
component coverage defects, so visual qualification has failed. Complete source
coverage, all-command endpoint qualification, display integration, widescreen,
scheduling and latency gates remain open.

Actual object lifetimes and camera domains (including rigid UI producers),
remaining geometry paths, complete resource and renderer
integration, actual-game correctness, cadence and latency
qualification and owner visual approval are outstanding. This branch does not
claim that enabling interpolation is ready or that all consumers are migrated.
Stereo and netplay local-view VRAM transactions are separate features and remain.

Pilot: current delivered Ape v0.5.5 sources (Ape 18513a0, engine 5f664a69,
UI d5af2c77), 60 FPS followed by display rate. Responsiveness comes first;
measure against the native baseline and target <=16.67 ms added median/p95
input-to-visible delay. If bracketing frames cannot satisfy that budget, revisit
the design explicitly rather than silently use predictive geometry.

Track implementation and final owner acceptance in central Beads issue
`beads-eio.3.445`. The release remains unchanged until the complete pilot passes.
