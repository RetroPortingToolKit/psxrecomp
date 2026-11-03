# Where runtime code and its facts come from

This note is for anyone who writes or reviews runtime code (`runtime/src`,
`runtime/include`). It explains which outside sources we use and how, and lists
the places that still need work. Issue #443 tracks the series of changes that
removes the remaining passages.

## The rules

1. **Hardware facts are cited to documentation.** When a comment states how the
   PlayStation behaves (a register bit, a table, a timing, a quirk), it names
   PSX-SPX and the page and section where the fact is written. PSX-SPX is
   published at <https://psx-spx.consoledev.net/>. A page is cited by its name
   in the address, for example `graphicsprocessingunitgpu` for
   <https://psx-spx.consoledev.net/graphicsprocessingunitgpu/>, followed by
   the section heading in quotes.
2. **Emulators are black-box references.** We run Beetle PSX, DuckStation,
   Mednafen or PCSX-Redux and compare what they output with what we output.
   That is how the oracle tools (`psx-beetle`, the trace and diff commands) are
   used, and comments may say "the Beetle reference gives the same result".
3. **We do not copy from emulator source.** Do not copy code, tables, constants,
   identifiers or the structure of a routine from an emulator's source, and do
   not quote emulator source in a comment. Their licences (GPL for Beetle,
   Mednafen and PCSX-Redux, CC BY-NC-ND for DuckStation) differ from this
   project's, and [`CONTRIBUTING.md`](../CONTRIBUTING.md) already rules out
   copying from incompatible licences.
4. **When documentation is silent, measure.** If PSX-SPX does not state a fact,
   write a small test program, run it on hardware or on the references, and
   record what it shows. Say in the comment that the value was measured and
   where the measurement lives.
5. **When PSX-SPX and the code disagree, say so.** Write both in the comment and
   leave the behaviour as it is. Changing it is a separate change with its own
   evidence.

## What the first change of the series did

It changed comments and documentation only; no code changed.

- Comments that named an emulator's source file, function or line as the
  authority for a hardware fact now cite PSX-SPX, where PSX-SPX states the
  fact. Each new citation was checked against the PSX-SPX text.
- Two comments quoted emulator source: the I-cache store rule in
  `psx_icache.c` and the interrupt line rule in `interrupts.c`. The quotes are
  removed. The comments now describe the behaviour and cite PSX-SPX.
- Where PSX-SPX says something different from the code, the comment now says
  so (GP0 commands 03h and FFh in `gpu.c`, the I-cache tag value in
  `psx_icache.c`).

## What is still to do

These passages say that code follows an emulator's source, or they name a
source file, function or line. Rewording would hide that, so they stay as they
are until the code itself is replaced. The order is the one agreed on #443.

| Change | Files | What the comments say today |
|---|---|---|
| 2. spu | `spu.c`, `spu_gauss.h` | The envelope rate decoder is "ported verbatim" from Beetle's `CalcVCDelta`; envelope steps follow `RunEnvelope`; several Beetle `spu.cpp` line references; the Gaussian table names DuckStation's `spu.cpp`. |
| 3. cpu/icache and memory/cycles | `psx_icache.c`, `psx_icache.h`, `psx_instr_cost.h`, `psx_cyc.h`, `psx_cycles.c`, `memory.c`, `cpu_state.h`, `dirty_ram_interp.c`, `main.cpp`, `docs/internal/CYCLE_MODEL_BEETLE.md` | The fetch, load-timing and I-cache models are "transcribed" from Beetle `cpu.cpp`; MULT/DIV and GTE latencies are transcribed from Beetle; `ReadAbsorb`/`LDWhich` names; the MTC0 software-interrupt check is labelled with a PCSX-Redux function name. |
| 4. mdec | `mdec.c` | The zig-zag table, IDCT matrix load and passes, dequantisation and RGB packing follow Beetle `mdec.cpp` with line references. |
| 5. cdrom | `cdrom.c`, `cdrom.h` | Command latencies (ID, ReadTOC, Pause, Init) and the data-ready notification follow Beetle `PS_CDC` functions; DuckStation `cdrom.cpp` handlers are cited for command cancellation and read restarts. |

Passages found in areas the agreed series does not cover yet:

| Area | Files | What the comments say today |
|---|---|---|
| GPU rasteriser | `gpu.c` | The triangle edge stepping cites DuckStation `gpu_sw_rasterizer.inl` (`makefp_xy`, `makestep_xy`). |
| GPU display crop | `gpu.c` | The vertical crop follows DuckStation `GetFullDisplayResolution`. |
| Texture sampling | `gpu_uv.h`, `gpu_sw_renderer.c`, `gpu_gl_renderer.c` | The UV offset model and bilinear filter are described as the Beetle / parallel-psx formulation (`Calc_UVOffsets_Adjust_Verts`). |
| SPU DMA cost | `dma.c` | The 48-cycle per-word cost cites Beetle `dma.cpp` lines. |
| PGXP cache | `gte.cpp` | The vertex cache layout names Beetle `pgxp_gpu.c` and DuckStation `cpu_pgxp.cpp`. |

Comparisons with the references (what the oracle showed, which emulator agrees),
the oracle tools themselves, and the DuckStation texture-pack file format
(`duckstation_texture_pack.*`, `docs/DUCKSTATION_TEXTURE_FORMAT.md`) are not on
these lists: they compare outputs or read a published file format, which the
rules allow.
