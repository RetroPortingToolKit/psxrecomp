# runtime/src layout

The runtime is organised by **emulated component**, one folder per hardware
block, with the host and support layers separated from the machine itself.
Before this, all 112 sources sat flat in `runtime/src/`, which made it
impossible to see at a glance which code is the PlayStation and which code is
our scaffolding around it.

This layout is also the map for the launcher-core work: see
[`LAUNCHER_CORE.md`](LAUNCHER_CORE.md). `host/` is the seam that moves to the
launcher; everything above it stays in the core.

## The machine

| Folder | Holds |
|---|---|
| `cpu/` | R3000A execution: dispatch, interpreter and the dirty-RAM bridge, icache, cycle accounting, fibers, traps, interrupts |
| `gte/` | Geometry Transformation Engine, plus PGXP precision hooks |
| `memory/` | Bus, RAM, MMIO map, kernel patch ranges |
| `dma/` | DMA controller |
| `timers/` | Root counters and the vblank clock |
| `gpu/` | GP0/GP1, VRAM, the three render backends, widescreen geometry, texture packs |
| `mdec/` | Motion decoder |
| `spu/` | Sound processing unit and its shadow/trace instruments |
| `cdrom/` | CD-ROM controller, cue/iso/CHD readers, disc identity, data shards |
| `sio/` | Serial I/O: controllers, memory cards, analog sticks |
| `bios/` | BIOS HLE tier, backend selection, boot state |

## Around the machine

| Folder | Holds |
|---|---|
| `host/` | SDL audio device, window icon, OSD, keymap and keybinds, host clock, frame pacing, savestate menu. **Everything here is a candidate to move to the launcher.** |
| `state/` | Savestates and rewind |
| `net/` | Netplay, rollback, lobby client, and the vendored `lobby_ws/` RFC 6455 helpers |
| `mods/` | Mod packages, mod runtime, builtin plugins, string translation |
| `overlay/` | Runtime code compilation: autocompile, overlay loader and capture, code provider |
| `debug/` | Debug server, crash/function/parity/device traces, the always-on rings, freeze heartbeat, self-check, cosim |
| `oracle/` | Beetle PSX integration, built only into the separate `psx-beetle` binary |
| `util/` | CRC32, SHA-256 |
| `app/` | `main.cpp` and game options. This is the monolith the core split will carve. |

## Rules for new files

1. **A new emulated component gets its own folder.** Do not add hardware to
   `app/` or `util/`.
2. **Component-private headers live beside their sources**, and are included
   flat (`#include "cdrom_lid.h"`) from within the same folder.
3. **A header used by more than one component belongs in `runtime/include/`.**
   That is the rule that put `png_write.h` and `recomp_audio_drc.h` there
   rather than in a component folder.
4. **`host/` only grows things a launcher could own.** If a file in `host/`
   turns out to be load-bearing for the simulation, it is in the wrong folder.

## Notes on the move itself

- Every source file moved with `git mv`, so history follows.
- No `#include` line changed. Runtime includes are flat and resolved through
  `PSXRECOMP_RUNTIME_INCLUDE_DIRS`, which still points at `runtime/include`.
  The recompiler emits flat includes too (`psx_runtime.h`, `cpu_state.h`,
  `psx_bios_backend.h`), so **generated code and every cached codegen tree
  keep compiling unchanged**.
- Build files and documentation were rewritten mechanically: 355 stale
  `runtime/src/...` references across 74 files.
- `runtime/include/` was not reorganised in this pass. Mirroring this
  structure there, or colocating headers with their component the way
  snesrecomp does, is a separate change.

## Reproducing the verification

The move was validated by building `psx-runtime` before and after and
comparing. Use a build directory under `runtime/`, which `.gitignore` already
covers:

```sh
cmake -S runtime -B runtime/build-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DRECOMP_UI_ROOT=../recomp-ui
cmake --build runtime/build-core --target psx-runtime -j
```

Before and after the move the linked `PSXRecomp` binary was the same size to
the byte (16,141,256), and two sample test targets still build. Configure is
the real gate here: CMake fails at configure time on a missing
`add_executable` source, which is exactly how the 60 relative `src/...` paths
in the test targets were caught.
