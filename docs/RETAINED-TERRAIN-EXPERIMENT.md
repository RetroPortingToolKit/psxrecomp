# Retained-terrain services (experimental, opt-in)

Issue: `beads-eio.3.156`. These services do not replace the faithful renderer.
They support a trusted, statically linked game plugin retaining original assets
outside guest RAM/VRAM. The first consumer is Crash's N. Sanity Beach experiment.

- `psx_mod_read_disc_file`: emulation-thread-only, reads the original or derived
  mounted ISO/CUE/BIN/CHD through an independent reader. Query with NULL/0, then
  supply capacity for the whole file (64 MiB ceiling). Applies active raw/user
  sector patches; never seeks the emulated CD controller. Current ISO path
  lookup supports root files and one subdirectory.
- Immutable texture banks: stable nonzero 16-bit IDs, owned copies of PS1
  texels/indices and palettes, lazy OpenGL upload. ID 0 means ordinary VRAM.
  256 MiB aggregate host budget, maximum 1024x512 words per bank. A resolver can
  rebuild a missing bank after a cold save-state restore. IDs must not be reused
  for different assets during a process lifetime. Software/Vulkan and the GL
  CPU-authoritative dual path report unsupported.
- Trusted GT3 packet arenas: bank ID in unused C1/C2 colour high bytes; optional
  fractional XY/reciprocal-depth suffix. Only explicitly allocated packet
  arenas interpret this extension; stock packets retain their meaning. See
  `runtime/include/mod_plugins.h` for the wire layout. CPU-side immutable assets
  are intentionally not serialized. No host/GL pointers are written to guest
  memory or save states.
- GPU-DMA enhancement aperture: allocated-only physical 0x00800000..0x00FFFFFF
  (8 MiB), outside all four PS1 RAM mirrors. Unallocated DMA addresses retain
  retail 2 MiB folding. BIOS/expansion/MMIO aliases cannot map to this aperture.
- Save states with allocations use v8 and a required enhancement-memory section.
  Both CPU and DMA allocations are captured; a layout cookie rejects older or
  differently sized layouts before applying RAM. Vanilla saves still write v7.
  Allocate during activation, not while rewind is recording. A matching layout
  is necessary, not a substitute for the game's asset/version compatibility.

The texture bank store lives for the process lifetime. This prototype is not a
general eviction/reload API, and there is no global terrain depth buffer or bulk
native mesh submission API yet. The Crash prototype's textured whole-level path
is substantially slower than its stock path; do not treat it as production-ready.

## Validation

Source-owned CTests: `mod_texture_banks_test`, `mod_memory_snapshot_test`,
`mod_gpu_dma_aperture_test`, `mod_runtime_test`, `gl_readback_runner_test` pass.
The real-GL readback fixture passes 82 checks at both 1x and 4x on NVIDIA,
including retained 4-bit CLUT/16-bit textures, VRAM bank transitions and painter
order. `run_gl_readback_region.py --sdl-library` accepts an explicit system SDL3
import library in addition to its default static dependency.

Crash's expanded-memory save/load and cold asset reconstruction were exercised
live. Baseline rewind was previously validated; this new enhanced path still
needs a dedicated live rewind acceptance run and performance profiling.

## TCP-only view diagnostics

`display_aspect num=21 den=9` changes the rendered view without moving, resizing
or focusing the window. `display_aspect num=3 den=1 adaptive=1` resumes adaptive
view, capped at 3:1. Neither writes settings. Unlike `ws_aspect` (GTE-only), this
updates the presented view and native-wide surface. Use `screenshot` for the
rendered native-wide buffer or `present_shot` for the composed window surface.
