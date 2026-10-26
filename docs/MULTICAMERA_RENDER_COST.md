# Shared GL draw submission cost

The native GL raster path rejects primitives whose entire integer bounding box
lies outside the current GPU drawing area. GPU decoding has already applied the
draw offset. A one-pixel guard keeps boundary lines and triangles conservative.
Wide backing targets, high-resolution wide mirrors, precision/depth coordinates
and seam correction keep their existing path. Fills, transfers and VRAM copies
are excluded because they do not obey primitive drawing-area clipping.

Repeated drawing-area and drawing-offset packets retain their pending batch
when the values have not changed. Changed state still flushes in painter order;
software state is forwarded in either case. These are host submission savings:
GP0 decoding, simulation, interrupts, GPU timing and software raster calls keep
their existing behavior. Both HLE and LLE textured batchers use this path. No
CPU-specific instructions or new user control are needed.

`runtime/tests/test_gl_render_thread.c` includes a clipped workload, a visible
texture control, a scissor-edge sliver and repeated state packets. It verifies
the skipped batches, retained visible pixels, clipping and shared batching.
The existing real-context stream runs at 1x/4x, with the worker disabled/enabled
and with dynamic resolution changes. On Windows/NVIDIA the native readback,
VRAM, hires and actual displayed composition hashes remain unchanged in all
six configurations under both batchers. The 22 blend/mask oracle checks pass.

Mega Man X4's experimental four-view build is the measured caller. Its separate
title config enables the existing audited PsyQ store-counter idle detector at
USA address `0x800E4F6C`; that is not a new framework default. The detector
retains device/IRQ boundaries and rejects unqualified loops. The title's
source compatibility hash includes this config. See its heavy-gameplay QA
record for performance and native multi-peer evidence. These checks do not
qualify every game or guarantee full-speed four-view play.
