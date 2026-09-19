# Retail BIOS freeze: PR #327 rollback

Issue: `beads-iisy`. Investigation and validation: 2026-09-07.

## Confirmed introduction

The visible retail SCPH1001 boot regression starts at
`0661ce7773c3b82ea441ac9c82b0df08b4ea1563`, "Preserve active display field
during interlaced raster drawing", authored by Alexbeav in
[PR #327](https://github.com/mstan/psxrecomp/pull/327).
It reached master through [PR #334](https://github.com/mstan/psxrecomp/pull/334),
merge `421f58ec8c2a07f2cd608c1afa507404076e9882`.

Matched historical builds used unchanged runtime sources, the same generated
Ape Escape and SCPH1001 code, BIOS/disc, game config, and OpenGL 2x settings.
A minimal CMake wrapper adapted the current game's build declaration to the
older runtime helper. All runs explicitly set `PSX_BIOS_HLE=0`; logs confirmed
`bios_backend=LLE`, `bios_boot=LLE (real intro)`, and `image=SCPH-1001`.

| Runtime revision | Result |
| --- | --- |
| `47bda817`, parent of original DMA slicing | Game code; normal close at frame 3979 |
| `8af48ae9`, PR #277 DMA slicing | Game code; normal close at frame 2959 |
| `99f775c8`, PR #304 before word-cost change | Game code; normal close at frame 3468 |
| `bc88c06c`, PR #304 word-cost change | Game code; normal close at frame 3845 |
| `155e269b`, exact parent of `0661ce77` | Game code; normal close at frame 2192 |
| `0661ce77`, first PR #327 commit | Severe Sony-screen stall around frames 375-376 |
| `39ebb06f`, master immediately before #334 | Game code; normal close at frame 2652 |
| `421f58ec`, merged #334 | Same severe Sony-screen stall around frames 374-376 |

This rules out naming #277 or #304 as the demonstrated introduction of the
visible freeze merely because they introduced/refined asynchronous DMA.

## Mechanism and scope

`GR_RASTER_ROWS` resubmits a primitive once per permitted scanline, changing
the backend draw area each time. That multiplies backend submissions and
flushes in the retail BIOS's interlaced display mode.

A separate DMA defect makes this especially costly: the CPU can overwrite a
primitive header before the asynchronous DMA walker consumes it. On master,
BIOS memcpy at `BFC188D8` clears packet `00138CA8` before DMA node 1023 reads
its link, sending the walker into a malformed `0 -> 1A0000 -> 0` list. The
per-row renderer magnifies the resulting bogus primitives into massive GL
work. The apparent freeze can advance roughly a frame over many seconds;
headless progress alone misses the user-visible failure.

The immediate fix rolls back only #327's five dependent commits:
`0661ce77`, `43909718`, `7c4b0300`, `5aee9c83`, and `6fcfb8ad`.
PR #326's bounded GL readback and #328's Vulkan aspect-ratio changes remain.
DMA scheduling, BIOS code, generated code, and launcher settings are unchanged.

This restores the previous interlaced raster behavior, including its missing
active-field preservation. A replacement should avoid per-row backend draw
submission and validate real retail BIOS boot as well as field correctness.
CPU/DMA arbitration is a separate follow-up, `beads-eio.3.122`; its prototype
needs Vampire Hunter D/Spot Goes to Hollywood compatibility testing before
integration. Do not mix that wider timing change into this rollback.

## Rollback validation

The full Ape game target (SDL3, launcher compiled in, OpenGL, Vulkan compiled
in) passes retail intro/disc handoff, visibly reaches the title/menu, and
closes normally at frame 11669. The same rollback executable also passes the
OpenBIOS LLE control run and closes normally at frame 3701.
Use the owned SCPH1001 BIOS and Ape disc with:

```powershell
$env:PSX_BIOS_HLE = '0'
& .\ApeEscapeRecomp.exe --no-launcher --bios <SCPH1001.BIN> --disc <Ape.cue>
```

Ensure the game's `game.toml` is staged beside the validation executable.
Judge actual windowed boot, not just a headless frame threshold.

Passed: `gl_readback_runner_test`, `psx_cyc_batch_test`,
`dma_gpu_linked_list_timing_test`, `psx_cycle_event_boundaries_test`,
`ws_prepass_guard_test`, `gpu_primitive_reject_test`, `gpu_sw_edges_test`,
and `gpu_vk_upload_alignment_test`.

The real hidden-GL readback fixture passes `checks=71 failures=0` at both 1x
and 4x on NVIDIA 616.86. Its stock runner initially failed to link because
the local MSYS2 SDL static library depends on iconv; rerunning the recorded
link command with `-liconv` allowed both unmodified fixture runs to pass.
