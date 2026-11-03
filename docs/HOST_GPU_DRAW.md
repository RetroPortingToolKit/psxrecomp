# Host-generated world drawing

Trusted render enhancements can declare complete immutable draw packets in allocated GPU-DMA backing storage as host work. Unmodified games keep the original word-by-word DMA timing. This API does not accelerate native RAM lists or VRAM transfers.

On the emulation thread:

1. Allocate packet backing with `psx_mod_alloc_gpu_dma_memory`.
2. Copy finished packet bytes with `psx_mod_write_gpu_draw_memory`. The call refuses native RAM, KSEG2, out-of-allocation spans and host-output callback scope. Render-pass writes participate in rollback. The caller owns packet metadata and must not mutate a declared node while it is being consumed.
3. Register only the completed world packet span with `psx_mod_set_host_gpu_draw_range(slot, start, bytes)`. There are two slots; `bytes=0` clears one. Native packets appended after this span must remain outside it.
4. Republish snapshot-owned spans from the mod's save-state restore callback. DMA/session initialization clears all registrations. Clear them when the enhancement is reset.

The production CPU-hold walker validates a complete node before treating it as host work. Accepted payloads are complete GP0 polygons (20–3F) and one-word drawing-environment commands (E1–E6). Unknown, incomplete and VRAM transfer commands retain the native path. A node crossing a registered boundary also retains the native path. The same linked-list walker emits all commands in order and stops exactly before the next native header. Node limits still bound cyclic lists.

This contract is for explicitly produced host drawing, not a global simulated GPU speed control. The enhancement must preserve its native simulation effects and qualify current-build save/load and visual output. Host GPU ranges are not serialized by DMA itself; they are derived from the enhancement's snapshotted packet metadata.

Always-on `dma_hold_stats` includes `host_draw_nodes` and `host_draw_words`. Focused production tests:

```
py -3 runtime/tests/test_dma_gpu_list_cpu_hold.py --cc <compiler>
py -3 runtime/tests/test_mod_memory_snapshot.py --cc <compiler>
```

The first fixture checks default holds, mixed native/host/native packet order, non-draw refusal, boundary crossing, reset and compound environment/polygon nodes at O0/O2. The second checks owned backing copies, invalid spans/scopes, snapshots and render-pass rollback across pages. Unrelated runtime seams abort if called.
