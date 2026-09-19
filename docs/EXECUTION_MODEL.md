# The Execution Model — static, native-overlay, and interpreter

*A semi-technical tour of how PSXRecomp actually runs a PlayStation game. If you
only read one design doc, read this one.* For the low-level plumbing, see
[`ARCHITECTURE.md`](ARCHITECTURE.md).

## The one-sentence version

PSXRecomp turns a PS1 game into a **native program** by translating its MIPS code
to C ahead of time; the small amount of code it *can't* see ahead of time is
either compiled to native code the first time it appears, or run by a tiny
built-in interpreter until it is — so the game is always **correct**, and gets
**faster** the more it's played.

## Why a PS1 game can't just be "decompiled once"

A PlayStation game is not one fixed blob of code sitting in memory. The main
program (the `PS-X EXE` on the disc) is, but games are far bigger than the PS1's
2 MB of RAM, so they **stream code off the disc as you play**. A level's logic,
a boss's AI, a menu — these are loaded from the CD into a fixed region of RAM,
run, and then *overwritten* by the next chunk when you move on. These streamed
chunks are called **overlays**.

That creates a hard problem for any ahead-of-time recompiler:

- The main EXE exists on disc at build time → we can translate all of it.
- An overlay's code **does not exist anywhere at build time** in a form we can
  find. It only appears in RAM at a specific moment during play, and the same
  RAM addresses get reused by dozens of different overlays over a playthrough.

So "recompile the game" can never be a single up-front pass. PSXRecomp handles
this with **three tiers of execution**, in strict priority order.

```
        ┌─────────────────────────────────────────────────────────┐
        │  Guest wants to run code at some address                 │
        └─────────────────────────────────────────────────────────┘
                                   │
             ┌─────────────────────┼─────────────────────┐
             ▼                     ▼                     ▼
      1. STATIC (AOT)      2. NATIVE OVERLAY      3. INTERPRETER
      Translated at        Compiled to native     Runs the guest's
      build time from      code the first time    own instructions
      the disc's EXE       the overlay is seen,   directly, as a
      and the BIOS.        then cached forever.   correctness safety
      The bulk of          Fills the "streamed    net for anything
      execution.           from disc" gap.        not yet native.
      ── fastest ──        ── fast ──             ── slowest, always
                                                     correct ──
```

## Tier 1 — Static (ahead-of-time)

At build time the **recompiler** reads two things and translates their MIPS
machine code into C, function by function:

- the real PS1 **BIOS** (`SCPH1001.BIN`) — this becomes the kernel, and
- the game's **main executable** extracted from your disc.

That C is compiled and linked into the runtime as ordinary native functions, so
when the game calls one of them the CPU just runs native code — no emulation of
individual instructions. **This is the overwhelming majority of what executes**:
boot, the engine, the main loop, most gameplay systems. It is as fast as the C
compiler can make it.

Everything a pure AOT recompiler can see, it recompiles. The only things left
over are the two categories below.

## Tier 2 — Native overlays (compiled the moment they're discovered)

Overlays can't be seen at build time — but they *can* be seen the instant the
game loads one. PSXRecomp watches for that moment and turns each overlay into
native code so that, from then on, it runs at Tier-1 speed:

1. **Capture.** Every time the game DMAs code from the disc into the overlay RAM
   window, the runtime records the exact bytes (before the game can modify them)
   and the addresses that were actually executed. These go into a per-game file,
   `overlay_captures.json`.
2. **Compile.** Those captured bytes are fed back through the *same recompiler*
   used for the main EXE, producing C for that overlay. The C is compiled to a
   small native library (a DLL) — using `gcc` in development, or a bundled
   `tcc` (TinyCC) toolchain in shipped releases so players need no compiler
   installed. Each library is keyed by the overlay's content hash.
3. **Cache & dispatch.** The compiled overlay lands in a content-addressed
   cache. On this run and every future run, when that overlay loads, the runtime
   finds the matching native library and dispatches into it **before** falling
   back to the interpreter.

The practical effect: **the first time anyone visits an area, it may run in the
slow tier for a moment; after it's been captured and compiled, that area is
native for them — and, once the capture is shared into a release, for everyone.**
Coverage grows toward "100% native" the more the game is played.

## Tier 3 — The interpreter (the correctness safety net)

There is always a window between "an overlay just appeared in RAM" and "we have
native code for it." There's also a genuinely hard corner: code that is
**rewritten to different bytes on every load** (self-modifying / per-load
relocated), which by definition has no single ahead-of-time translation.

For both, PSXRecomp includes a **small MIPS interpreter** that runs the guest's
*own* instructions directly on the CPU register state. It is deliberately narrow:

- It runs **only** code that was installed into RAM at runtime (dirty pages) and
  is not yet covered by a native overlay. It is **never** on the BIOS or
  main-EXE path — those are always static.
- It is a **safety net and a coverage feeder**, never a substitute for
  recompiling. If the interpreter is running something, that's a signal to
  capture-and-compile it (Tier 2), not to leave it interpreted.

Crucially, this is **not** high-level emulation (HLE). The interpreter executes
the program's real instructions, exactly as the game's authors wrote them — the
*only* difference from static recompilation is where the instructions came from
(RAM-at-runtime vs disc-at-build-time). We never synthesize "the answer the code
would have produced."

## The guiding rule: precision over recall

The tiers are ordered so the system can only ever fail *toward being slow*, never
*toward being wrong*:

- Code we **haven't** compiled yet safely drops to the interpreter and gets
  captured for next time — under-coverage **self-heals**.
- Code we might compile **wrong** would corrupt the machine — so native code is
  dispatched *only* when its source RAM is provably unchanged, and a compiled
  overlay is instantly revoked the moment the RAM it came from is overwritten.

> **The worst case is always performance, never correctness.** Anything not yet
> native simply runs interpreted, correctly.

## How the gaps actually get plugged

Two complementary loops close the gap toward full native coverage:

- **While you play (automatic).** The capture step runs continuously in the
  background. New areas you reach are recorded and compiled to native code —
  often within a minute, without interrupting play — and reused on every later
  launch. Your discoveries live in `overlay_captures.json` next to the game.
- **In releases (shared head start).** A game release ships a `cache/` folder of
  overlays already compiled from everywhere players have collectively visited,
  so common areas are native from the very first moment you arrive.

The frontier of the project is pushing Tier-2 coverage up (so the interpreter
goes idle) and, eventually, handling the self-modifying corner by baking in the
relocation pattern at build time. Until then, that corner stays correctly
interpreted — a narrow, accepted cost, not a wall.

## Where this lives in the code

| Concern | Where |
|---|---|
| MIPS→C translation (static + overlay) | `recompiler/src/` (`code_generator.cpp`, `mips_decoder.cpp`, …) |
| Overlay capture | `runtime/src/overlay/overlay_capture.c` |
| Overlay compile pipeline | `tools/compile_overlays.py`, `runtime/src/overlay/overlay_backend.c`, `overlay_compile_worker.c` |
| Native overlay load/dispatch | `runtime/src/overlay/overlay_loader.c` |
| Runtime interpreter (dirty RAM) | `runtime/src/cpu/dirty_ram_interp.c` |

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for how these connect to the hardware
simulation, and [`docs/FEATURES.md`](FEATURES.md) for the overlay-system feature
reference.
