# Geometry interpolation replacement (in progress)

The legacy temporal crossfade, duplicate-frame smoother, inferred draw-list
interpolation, reprojection and guest draw replay paths are retired. Their
configuration keys cannot activate them. Rejection-only ABI adapters remain
while consumers migrate. The GL history, replay-image queue, temporal selector,
cost planner and output scheduler have been removed. Stereo and netplay own-view
VRAM transactions retain their separate backup, journal and restoration paths.
Historical investigations remain in Git history.

The replacement owns explicit scene snapshots and immutable texture versions.
Object identity, lifetime, topology, local vertices, transforms and camera domains
come from title source metadata. It never infers correspondence from screen
positions, depth, draw order or nearby coordinates. Arithmetic, untracked writes
and reset invalidate transported source tokens. Camera cuts, invalid metadata,
resource limits and unsupported command streams select the native fallback.

`presentation_scene.c` samples decomposed object and camera poses, shortest-arc
quaternion rotation and corresponding animated vertices. `presentation_draw.c`
clips before perspective division and preserves command/resource order.
`presentation_gpu.inc` renders into a separate GL target at the actual internal
resolution; retained native-word texture versions cannot be overwritten while
referenced. Source capture and rendering never execute guest code or modify
CPU, RAM or authoritative VRAM. Export readback is separate from live rendering.

The Ape development adapter captures skeletal, rigid, world, terrain and explicit
subdivision sources. Ordered asynchronous GPU snapshots prepare the midpoint
between adjacent complete scenes. The ordinary repeated-buffer presentation can
select that output after a nonblocking completion check. A missing or late pair
keeps the native presentation deadline. This currently supplies the 60 FPS pilot;
independent display-rate scheduling is not implemented.

Focused scene/draw, source-transport and title-producer checks cover identity,
discontinuities, clipping and state preservation. Actual-game 60 FPS checks have
qualified a limited 1x workload, including current-build save/load and matching
guest fingerprints. A separate 3x GL fixture checks stereo restoration, native
surface preservation and full-resolution geometry PNG export. Higher-resolution
actual-game qualification, broader scene coverage, display-rate support and
remaining consumer migration are still open. No complete replacement release
is claimed by these development checks.

Pilot order is 60 FPS followed by display rate. Measure responsiveness against
the native baseline, targeting <=16.67 ms added median/p95 input-to-visible delay.
If complete bracketing frames cannot satisfy that budget, revisit the design
rather than introduce prediction or additional source-frame latency.

Track implementation and final qualification in central Beads issue
`beads-eio.3.445`.
