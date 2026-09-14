# Opt-in immutable texture-bank batching

`psx_mod_set_texture_bank_batching(1)` permits painter-ordered OpenGL batches
of opaque triangles and semi-transparent triangles in modes 0, 1 and 3 from an
immutable retained texture bank. It defaults off. Call it at an emulation-thread
render boundary; zero restores the conservative path.

Opaque vertices use `a_semi=0`; their dual-source destination factor is zero,
including texels with STP set. Semi-transparent vertices use the existing
per-fragment factors. Colour and alpha retain submission order in one pass.
This removes unnecessary opaque/semi batch boundaries without sorting faces.

The optimization does **not** apply to ordinary VRAM sampling, which can alias
a render target. Destination-mask checking and subtractive blending retain
their conservative paths. Bank, mask-set, filter, texture-window and backdrop
gate changes still split batches. No guest timing or simulation is changed.

## Validation

`runtime/tests/test_mod_texture_banks.c` checks default-off behavior and the
complete bank/mask/mode eligibility table. `test_gl_readback_region.c` uses a
hidden real OpenGL context with source-owned synthetic textures:

- Overlapping opaque and semi triangles, STP and transparent texels, all blend
  modes, nearest/bilinear filtering, mask-set/check combinations.
- Bank changes, ordinary VRAM interleaving and later destination-mask draws.
- Every RGBA sample in the tested region at both native and 4x resolution,
  compared against the batching-disabled implementation.
- Strict draw-batch reduction for eligible alternating opaque/semi sequences;
  unchanged counts when destination-mask checking is on.

NVIDIA RTX 3080 Ti, driver 616.86: 204 checks passed at both 1x and 4x. Before
enabling opaque coalescing, the new fixture fails exactly the four eligible
batch-reduction checks at 1x while its pixel checks pass. This demonstrates the
test exercises the new optimization. It is not an all-driver/game proof.

Use `runtime/tests/run_gl_readback_region.py --help` for the hidden-context
runner. The runner leaves commands, output and receipts under its output path.

Consumer evidence and remaining performance limitations are in Crash's
`docs/NATIVE-TERRAIN-20260914.md`. Tracking: beads-eio.3.162; Crash issue 4.
