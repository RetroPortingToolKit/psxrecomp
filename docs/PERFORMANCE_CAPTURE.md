# Frame-boundary performance capture

Developer TCP builds provide `perf_capture`. It is disabled until explicitly
started and records at most 16,384 vblank-boundary samples in memory. It never
reads pixels, runs guest code, writes files or inserts per-instruction timers.

Example request (addresses are hexadecimal strings):

```json
{"cmd":"perf_capture","op":"start","draw_addr":"80057960","draws":1800,"warmup":8}
```

`draw_addr` names an aligned counter in main RAM. `draws` is 1..8000; unsigned
counter wrap is supported. A backwards/reset-sized jump terminates with
`invalid_counter_or_start`. `warmup` is 0..600 vblanks. Alternatively supply
`state_addr` and integer `state` to stop at a game-defined state transition.
Starting already in that state is an error. An optional `aux_addr` records a
second RAM word without using it as a stop condition. Addresses must be main
RAM physical, KSEG0 or KSEG1 addresses; scratchpad/MMIO/mod memory are rejected.

Poll `{"cmd":"perf_capture"}` for status. After completion, retrieve pages:

```json
{"cmd":"perf_capture","op":"read","offset":0,"count":128}
```

Each row is `[host_ticks, guest_cycles, vblank, counter, state_or_aux, batches]`.
Divide tick differences by status `frequency`. `op:"stop"` freezes an incomplete
capture. `end` distinguishes complete, stopped, capacity and invalid starts or
counter resets. An active capture cannot be replaced or read.

Status also reports:

* `scale`: internal renderer scale, independent of the window dimensions.
* `overlay_delta`: loads, invalidations, revalidations, native dispatches,
  interpreted dispatches, stale dispatches, load microseconds, unregistered.
* `batch_delta`: textured batches, then isolation/source, blend, mask, filter,
  backdrop gate, texture-window and capacity flush counts. These reasons do
  not include every external flush (flat draws, uploads, readbacks, present).
* `gl_cpu_ms`: cumulative CPU time inside textured/flat batch submission.
  This excludes graphics-driver worker threads and is not GPU busy time.
* `audio`: output active, underrun and overflow-drop deltas.
* `rewind`: capture count, snapshot milliseconds, ring-storage milliseconds,
  thumbnail milliseconds. Timing requires `PSX_RUNTIME_PERF_DIAG=1`; the count
  remains available without timers. Flat submission timing has the same gate.

`PSX_RUNTIME_PERF_DIAG=1` additionally logs snapshot sections over 10 ms. Use
that for attribution separately from acceptance timings. GPU `frame_perf`
queries are optional via `PSX_GL_PERF`; unfinished queries are dropped without
waiting. Their scene intervals include gaps while the CPU produces commands.
They cannot independently establish GPU utilization.

`input_route_start` accepts optional `counter_addr`. Queued segment `frames`
then mean increments of that RAM counter, rather than input polls/vblanks.
Repeated polls with no progress consume nothing. Multi-increment progress can
cross multiple segments. Reset-sized jumps stop the route. Include a neutral
tail; completion releases the input override to the physical controller.

`display_aspect` without arguments returns its current fixed aspect or adaptive
maximum and the `adaptive` flag, allowing diagnostic clients to restore it.
`rewind` supports status/open/move (`delta` -200..200)/accept/cancel, using the
ordinary UI actions. `accept` stages a restore; validate subsequent guest
counter rollback and forward progress. TCP polling continues while this UI is
open. These commands are absent from builds without debug tools.

# Opt-in rendering policies

All new policies default off. They do not change display resolution, draw
distance, object activation, simulation clocks or stock DMA timing globally.

`psx_mod_set_native_texture_packets` treats valid G3/GT3 payloads wholly inside
registered enhancement texture-packet arenas as native host work. Each OT
header still costs its original guest clock; other nodes retain per-word
timing. Packet storage, order, texture data and active DMA state remain in
savestates. A title must opt in only for an enhancement already implemented as
host work. This changes the scheduling of those enhancement payloads, so it
requires game-level validation; it is not a general PSX speed hack.

`psx_mod_set_vram_texture_batching` allows opaque and supported dual-source
blend primitives to share a batch in live VRAM. Primitive order is unchanged.
The existing texture/CLUT dependency checks drain pending draws before packing
feedback; CPU uploads also drain them. Mask checking, subtractive blending,
texture source changes and incompatible state still split batches. Enable only
for validated title scenes and turn it off on exit. Other titles keep the
conservative existing isolation behavior.

Tests: `debug_perf_capture_test`, `mod_texture_banks_test`,
`dma_gpu_linked_list_timing_test`, and the real-GL
`runtime/tests/run_gl_readback_region.py` at internal scales 1 and 4.
