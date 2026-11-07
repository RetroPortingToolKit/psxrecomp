# Frame-rate presentation

The old image crossfade, motion-adaptive blending, flip tracker and temporal
output scheduler have been removed. The legacy interpolation activation APIs
reject requests; stale settings and environment switches cannot enable them.

The source-geometry replacement is described in [FRAME_GENERATION.md](FRAME_GENERATION.md).
It uses explicit scene identities, owned geometry and immutable resources.
Presentation samples must not execute guest code or postpone native deadlines.

Native VBlank-rate changes alter the simulated machine and are separate from
presentation interpolation. Stereo and per-player views use independent
[view transactions](RENDER_PASSES.md).
