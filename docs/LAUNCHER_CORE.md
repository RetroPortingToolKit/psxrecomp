# psxrecomp as a launcher-hosted core

Branch `feat/launcher-core`. Status: **design recorded, no code changed yet.**
Written 2026-09-14 from a survey of this tree at `origin/master` 4be26f16.
Line references are pinned to that commit.

## 1. What we are building, and what "core" can mean here

One launcher process (`retcomm-launcher`) becomes the single front end for
every recompiler: it owns the window, configuration, the game library, netplay
browsing and mod selection. psxrecomp, snesrecomp and later n64lle become
"cores" it manages the way RetroArch manages cores. The per-game repositories
(`TombaRecomp`, `ApeEscapeRecomp`, …) are retired in favour of data-only
**profile packages** plus mods.

One constraint decides the shape of everything below, and it comes from the
doctrine rather than from this tree. `recomp-ai-rules/MODS.md` §8:

> A variant *ROM* still needs a variant *binary*. A static recompiler bakes one
> program image into one executable.

So a psxrecomp artifact can never be a RetroArch-style library that loads
whichever disc you point at it. Each title is still its own build, still
produced locally from the player's own media, exactly as `PSX/PRINCIPLES.md`
§10 already requires ("Public builds are a setup host … the player supplies
their own legal media and generates locally").

**Therefore "core" here means two things, not one:**

1. a **build component** — the emitters plus `runtime.cmake`, which the
   launcher invokes to scaffold and compile a title from ROM + profile; and
2. a **runtime contract** — the protocol the built title speaks to the
   launcher while it runs.

The per-title binary does not go away. What goes away is the per-title
*repository*, the per-title setup wizard, and the per-title window.

## 2. Transport: out of process, push model

**Decision: the title runs as a child process and pushes frames and audio to
the launcher over shared memory. The launcher does not drive the frame clock.**

The obvious alternative, a libretro-shaped in-process core with a
`retro_run()` the launcher calls once per frame, is rejected for now. This
tree has no such entry point and cannot cheaply grow one: the guest owns the
thread, `psx_scheduler_run()` never returns during play, and the frame
boundary is a callback *out* of the guest at vblank (`gpu.h:141`, installed
at `main.cpp:15341`). Flipping that to a
caller-driven step means a fiber switch at the vblank callback on every frame.
The machinery exists (`runtime/include/psx_fiber.h`, and the structured
escape enum in `runtime/include/psx_scheduler.h`), so it is possible, but it
is a deep change to the execution model and it is not needed to get a hosted
window.

Push, out of process, buys four things:

- **No control-flow inversion.** The existing vblank callback becomes the
  frame sink. The guest keeps its infinite loop.
- **Crash isolation.** Machine-translated guest code cannot take the launcher
  down with it.
- **Language neutrality.** n64lle is a Rust tree; a C++ in-process interface
  would not serve it. A shared-memory protocol serves all three.
- **Precedent.** The workspace already runs code-under-test and oracle as two
  processes with an identical debug protocol (`PSX/PRINCIPLES.md` §9).

The cost is that the launcher cannot rewind or run-ahead the core by calling
it, and cannot implement rollback netplay itself. That is fine: rollback
already lives in the engine (`runtime/src/net/psx_netplay_rb.c`, plus
`lib/retcomm-rbengine`), and it stays there.

## 3. The seam in this tree

`runtime/src/app/main.cpp` is 16,471 lines and owns CLI parsing, config
resolution, the launcher window, SDL window/renderer/audio, input, present,
pacing, netplay, savestates, OSD and shutdown. We are not refactoring that
file wholesale. We are cutting one seam through it.

The cut runs through `sdl_vblank_present_body()` (`main.cpp:6464`, ~1,150
lines), which today does: debug-server poll, FPS/OSD, input poll, savestate
and rewind poll, netplay admit, present, pacing. The split is:

| Stays in the engine | Moves to the launcher |
|---|---|
| produce the frame (`gr_render_display_hires` and friends) | present it to a window |
| produce audio (`RtlRenderAudio` analogue, `psx_sdl_audio`) | own the audio device |
| accept an input word per frame | poll SDL, map keybinds, manage controllers |
| pacing *of the simulation* | pacing of presentation, vsync policy |
| savestate/rewind mechanics | the menus that drive them |

Three existing abstractions make this cheaper than it sounds:

- **The renderer is already engine-side and already vtable'd.**
  `GpuRenderBackend` (`runtime/include/gpu_render.h:127-205`) already exposes
  `render_display(uint32_t* out, int pitch, …)`. A per-game project supplies
  no renderer code at all. Nothing has to be hoisted upstream for PSX.
- **Audio is already a pull model** behind a small abstraction
  (`runtime/include/psx_sdl_audio.h`, `sdl_drc_callback`), and the SPU is
  advanced on guest cycles rather than host demand.
- **An out-of-process control channel already exists.** The debug server
  (`runtime/src/debug/debug_server.c`, JSON over TCP, `docs/TCP_COMMANDS.md`)
  already implements `set_input`, `clear_input`, `present_shot`, pause/step,
  savestate and watchpoints. The core protocol should extend this rather than
  invent a second channel.

**The one genuine obstacle is the renderer backend.** Only the software path
produces a plain CPU framebuffer today. OpenGL blits its hi-res FBO straight
to the window and swaps; Vulkan owns its swapchain and has no readback hook at
all (`docs/TCP_COMMANDS.md` already records that `present_shot` is unavailable
there). A hosted core therefore has to choose per backend: software gives a
buffer for free, GL has `gl_renderer_present()` plus an existing readback path
for 24-bit FMV, and Vulkan needs either a new readback or a shared swapchain
with the host. Phase 1 uses software and GL readback and states the quality
cost plainly rather than hiding it.

## 4. Per-title content: what moves where

The survey found exactly two kinds of hand-written C in a title repo.

- **`codegen_setup.c` / `codegen_setup.h`** is a filled
  `PsxrecompCodegenHostConfig` struct plus two trampolines (see
  `TombaRecomp/codegen_setup.c:8-40`). It is data wearing a `.c` extension and
  becomes a TOML block in the profile package. No engine change needed beyond
  reading it.
- **Mod plugin C** is the real per-title code. Tomba ships five
  (`src/mods/tomba_*_plugin.c`) compiled into the runtime via
  `EXTRAS_SOURCES`. Mod archives are data-only by doctrine
  (`recomp-ai-rules/MODS.md` §2: a manifest *selects* a trusted plugin by
  stable id, it never supplies one), and this tree already implements exactly
  that model (`runtime/include/mod_plugins.h:16-19`). So per-title plugins
  must move **upstream into the engine** and register under stable ids, or be
  re-expressed as declarative operations. They cannot ship in a profile
  package as source.

Everything else in a title repo is config, seeds, assets, CI and docs.

## 5. Staging

Each stage ends with something demonstrable, and none of them is a big-bang
rewrite.

1. **Protocol specification.** Frame, audio, input, and lifecycle messages;
   shared-memory layout; versioning. Written as a header in this repo and
   implemented by the launcher, so both sides compile against one definition.
2. **Frame sink.** An engine-side hook that, when a host is attached, receives
   the composed frame instead of presenting it. Default path unchanged when no
   host is attached; `--headless` already exists and is the natural base.
3. **Input and audio.** Input word arrives from the host per frame instead of
   from `SDL_PollEvent`; audio frames are pushed to the host ring.
4. **A title runs in the launcher window.** One PSX title, software backend,
   no mods, no netplay. This is the checkpoint that proves the model.
5. **GL readback path**, and the honest statement of what Vulkan cannot do
   yet.
6. **Profile packages** replace `codegen_setup.c` and the per-title CMake, and
   the launcher scaffolds a title from ROM + profile without a game repo.
7. **Per-title mod plugins move upstream** under stable ids.

Netplay is deliberately last: it already works in-engine and nothing above
needs to touch it.

## 6. Open questions

- Whether the launcher or the core owns the pause/menu overlay. The engine
  currently has modal overlay loops that re-enter SDL event pumping from
  inside the guest's frame callback.
- Whether the debug server's JSON-over-TCP channel carries the control
  protocol, with only frames and audio in shared memory, or whether control
  moves to the shared-memory channel too.
- Vulkan: readback, shared swapchain, or unsupported under a host.
