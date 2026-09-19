# Stubs to Fix

Audit date: 2026-04-24. Every item here is a place where the runtime
returns wrong data, crashes, or silently drops behavior that real
hardware provides.

## Pre-Phase 5 (BIOS shell correctness)

### S1 — GPU shaded line uses flat color
- **File:** `runtime/src/gpu/gpu.c:722`
- **What:** `gp0_exec_shaded_line()` reads C0 but ignores C1. Draws
  the entire line in the first vertex color instead of interpolating.
- **Fix:** Interpolate RGB between C0 and C1 along the line, same way
  `sw_draw_shaded_triangle` interpolates across scanlines.
- **Impact:** Visual — BIOS shell lines render with wrong color gradient.

### S2 — GPU polyline crashes
- **File:** `runtime/src/gpu/gpu.c:1462`
- **What:** GP0 opcodes 0x48-0x4F (mono polyline) and 0x58-0x5F
  (shaded polyline) hit `exit(1)`. These are variable-length commands
  terminated by a sentinel word (bit 31 set in vertex word, or
  0x55555555/0x50005000 pattern).
- **Fix:** Implement polyline state machine: accumulate vertices until
  sentinel, draw line segments between consecutive pairs.
- **Impact:** BLOCKING — any code that draws polylines crashes the
  runtime.

## Phase 5+ (game support)

### S3 — MDEC decoder is a no-op
- **File:** `runtime/src/memory/memory.c:147-148, 199-200`
- **What:** MDEC registers (0x1F801820, 0x1F801824) read as 0 and
  writes are silently absorbed. No MDEC DMA (ch1 in, ch0 out).
- **Fix:** Implement MDEC MPEG-style macroblock decompression with
  DMA input/output. Reference: DuckStation `src/core/mdec.cpp`.
- **Impact:** BLOCKING for FMV cutscenes in any game.

### S4 — SPU has no audio synthesis
- **File:** `runtime/src/spu/spu.c`
- **What:** Register reads/writes and DMA4 buffer transfers work, but
  no ADPCM voice decoding, no audio output, no reverb.
- **Fix:** Implement 24-voice ADPCM decoder with SDL_audio output.
  Reference: `psxrecomp-v2/runner/src/spu.cpp` for ADPCM tables.
- **Impact:** BLOCKING — no sound at all.

### S5 — DMA channels 0,1,3,5 unimplemented
- **File:** `runtime/src/dma/dma.c`
- **What:** Only ch2 (GPU) and ch6 (OTC) are implemented. Ch0
  (MDEC out), ch1 (MDEC in), ch3 (CDROM), ch4 (SPU), ch5 (PIO)
  will crash or silently fail.
- **Fix:** Add channel handlers as corresponding hardware modules
  come online. Ch3 (CDROM) and ch4 (SPU) are highest priority for
  game support.
- **Impact:** BLOCKING for games using MDEC, CDROM sector DMA, or
  SPU DMA.

## Resolved

- ~~DMA ch2 GPU→RAM writes zeros~~ — Fixed 2026-04-24 (4054dc1)
- ~~GPU raw texture mode uses color==0x808080 check~~ — Fixed 2026-04-24
