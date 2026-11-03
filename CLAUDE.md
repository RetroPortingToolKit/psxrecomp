# PSXRecomp v4 — Rules

This file is the constitution for v4. Read it at the start of every
session before doing any work.

---

## ⛔ RULE -1: BUILD THE FAITHFUL CORE — NO HACKS, NO PRESERVATION

This ecosystem is being BUILT, not maintained. There is nothing to preserve.

- The correct fix is ALWAYS the faithful, hardware-accurate, class-level core.
  NEVER a surgical per-game patch, a symptom workaround, a `game.toml` hack
  (e.g. `overlay_native_block`), or "make native agree with interp even if both
  are fake."
- When a narrow patch and a general faithfulness fix both exist, the
  faithfulness fix WINS — even if larger, even if it breaks other titles.
  Other games were built on a faulty ecosystem and will be **regenerated**.
  Backward-compatibility is NOT a constraint.
- The recurring failure mode (weeks lost) is veering into a quick hack that
  doesn't even work and burns the session. Do not do this. If you catch
  yourself proposing a "surgical"/"temporary"/"just for this title" fix — STOP
  and build the real thing.
- PSX timing specifically: the faithful core = ONE shared per-instruction
  cycle-cost function for BOTH backends (compiled + dirty-RAM interp), exact
  block cycle totals (delay-slot ownership), timers derived on-demand from a
  global guest-cycle counter, devices on scheduled event deadlines, every
  basic-block leader re-enterable. Confer with ChatGPT via the Chrome MCP
  browser (chatgpt.com, "PSX Static Recompiler Debug" chat), not the codex CLI.

This supersedes any pressure to ship fast. Completeness is absolute here.

**Scope (refined 2026-06-29 — faithfulness is the FOUNDATION, not the
destination).** The rule above governs FOUNDATION work — building the faithful
core — and forbids per-game patches / symptom-workarounds / `game.toml` hacks
*as a way to fake faithfulness*. It does NOT forbid them forever: once the
faithful core is proven, per-game shims/hacks are the legitimate tool of the
*enhancement* phase — accelerated load times (toward 0), widescreen, etc. (see
`docs/ENHANCEMENTS.md`). Likewise **LLE is the BASELINE, not an absolute**: a
faithful, Beetle-validated HLE *subsystem replacement* is permitted at a genuine
LLE landmine (non-determinism with no hardware analog, or profound performance
loss), never as the starting point — see §0's amendment below and
recomp-template `PRINCIPLES.md` → "LLE Is the Baseline; HLE Is a Subsystem
Replacement, Not a Starting Point".

**The authoritative game plan is `docs/internal/FAITHFUL_TIMING_PLAN.md` — READ IT EACH
SESSION** (north star, phased plan P1–P6, current status/log). Update its
Status/Log section every session. The full-coverage accuracy burndown across ALL
axes (semantics, cycle, IRQ, MMIO, peripherals, static-vs-dynamic, determinism)
lives in `docs/internal/ACCURACY_BURNDOWN.md` — every item must be cross-referenced against an
external comparative (psx-spx / in-tree Beetle source / DuckStation / HW test
ROMs), not asserted. Axis 5 (peripherals — esp. SIO/controller, the hybrid-pad
bug) is the suspected-weakest second front after the cycle axis.

---

## 0. The architecture is locked

v4 implements **Architecture A**: static MIPS-to-C recompilation of
`bios/SCPH1001.BIN`, producing native C that links into the runtime as
real compiled functions.

There is **no MIPS interpreter** in v4 for the BIOS path. Not as a
fallback. Not as a "temporary" measure. Not for "code we couldn't
recompile yet". If a BIOS function cannot be recompiled, the recompiler
is wrong and must be fixed. The interpreter does not exist. Do not
write one.

For the faithful reference, the BIOS remains recompiled code. The historical
no-HLE restriction below is superseded for deliberate replacements by the owner
HLE policy in this section. Historically there was **no HLE BIOS layer** in v4. No `bios.c` with case branches
intercepting A0/B0/C0 vectors. No C reimplementations of `OpenEvent` or
`StartCard` or `alloc_kernel_memory`. The BIOS IS the recompiled C
output of `SCPH1001.BIN`. If a BIOS routine misbehaves, the answer is
to fix the recompiler or fix the hardware simulation it touches via
MMIO — never to write a C "shim" that produces the answer the BIOS
would have produced.

There are **no stubs**. A function is either fully implemented or it
aborts with a fatal error. `return 0;`, `return 1;`, `cpu->v0 = 1;
return;` are all stubs. `// TODO`, `// FIXME`, `// for now` are all
stubs. Hand-delivering an event because the chain handler isn't
installed is a stub wearing a costume and is the worst kind because it
hides the missing integration.

**Owner HLE policy, 2026-10-06.** The older HLE amendments are superseded by
`F:/Projects/recomp-template/HLE.md` and [the build integration](docs/HLE_EXECUTION.md).
Keep LLE buildable and runnable as the correctness reference. Qualified HLE is
the default performance implementation, selected at build time. Preserve actual
caller inputs, outputs, required memory/register effects, continuation and
observable completion ordering. Batching, native/GPU algorithms, private host
state, caching and approximate internal timing are allowed. Small practically
unnoticeable differences are already authorized. HLE need not support every game,
dual-execute, retain a live LLE state or provide a runtime switch. Compare exact
outputs exactly, intentional approximations within their declared contract, and
measure useful performance. Missing effects, false success and material gameplay
changes remain defects. Faithful-core accuracy requirements apply to the LLE
reference, not to unobserved HLE internals.

---

## 1. The BIOS is recompilation target #1, the game is target #2

Phase 1-3 of docs/internal/PLAN.md exist to get the BIOS recompiled and booting on
its own. The BIOS must reach the Sony logo and the BIOS shell, running
entirely as native C, before any game work begins. There is no path
that loads a game EXE before the BIOS is fully working in v4. **Do not
load a game ISO. Do not load a game EXE.** Tomba does not exist in v4
until Phase 5.

If you find yourself needing to load a game to "test something",
whatever you're testing belongs to a phase that hasn't started yet.

---

## 2. Three sources of truth, in priority order

Truth comes from three sources, in this order:

1. **BIOS disassembly** at `docs/psx_bios_disasm.txt` for what the BIOS
   code is supposed to do. Human-annotated pseudocode with named functions,
   kernel data structures (IntRP, ExCB, TCB, TCBH), A0/B0/C0 tables, boot
   sequence, and exception handler logic. Check this FIRST.
2. **Ghidra MCP** for what the raw bytes are at a given address (static
   analysis of `SCPH1001.BIN` loaded at `0xBFC00000`). Use when the disasm
   doesn't cover a function or you need exact instruction-level detail.
3. **Beetle PSX oracle** (embedded in `psx-beetleoracle.exe`) for what real
   PS1 hardware does at runtime. Beetle PSX (mednafen-psx libretro core)
   runs in-process with the native runtime, sharing the same debug server.
   Oracle commands: `find_first_divergence`, `emu_read_ram`, `emu_sio_trace`,
   `emu_trace_addr`, `emu_step`. Where Beetle has gaps, **build new tooling
   and visibility** — don't guess, don't route around.

Use all three, never just one. Don't guess. Don't say "probably". If you
cannot answer a question from the disasm, Ghidra, or the Beetle oracle,
the answer is "I don't know yet" — not a confident guess.

---

## 3. No printf debugging. No log files. Ever.

If you need to inspect runtime state, **build a TCP debug server
command** for it. The v3 build accumulated 555 GB of `boot_trace*.log`
and `card_test*.log` files because previous sessions used `fprintf` for
"just this one thing". The rule is absolute: **no `fprintf(stderr, ...)`
in source code, ever, for any reason.**

When the v4 runtime is built (Phase 2+), it will have a TCP debug
server on a fresh port. All inspection goes through that.

---

## 4. Never modify generated code

The output of the recompiler — files in `recompiler/output/` or
`generated/SCPH1001_full.c` etc. — is a build artifact. If the
generated code is wrong, the fix is in the recompiler source
(`recompiler/src/code_generator.cpp` and friends), not in the
generated file.

This is the same rule as v3 had, and it stays.

---

## 5. Don't accept partial milestones

Phase completion requires the user-visible end state, not "I think it
should work now". Phase 3 is "Sony logo displays on screen". Not "the
recompiler emitted code that probably draws the logo". Not "the GPU
command stream looks right in the debug server". **The pixels appear
on screen, or the phase is not done.**

This was the v3 failure mode: declaring "memory card screen freeze
RESOLVED" when in fact the screen had been unlocked by hand-delivering
a fake event. The fake delivery was not progress, it was theater.

---

## 6. Session start checklist

At the start of every session, before any code change:

1. Read this file (CLAUDE.md).
2. Read docs/internal/PLAN.md to confirm what phase we are in and what the next
   concrete milestone is.
3. Verify `docs/psx_bios_disasm.txt` exists (primary reference).
4. Verify Ghidra MCP is reachable. If not, stop and ask.
5. State out loud: "Architecture A is locked. No interpreter. No stubs.
   LLE default + oracle; HLE only per the §0 amendments (opt-in tier,
   LLE fallback, no fakes). BIOS first. Game never until Phase 5."

If any of these fail, do not proceed with the user's task — surface
the failure first.

---

## 7. Salvage from v3 — what's allowed and what's not

The recompiler in `recompiler/` was salvaged from v3 because the
core MIPS-to-C translator pieces (`basic_block.cpp`, `control_flow.cpp`,
`function_analysis.cpp`, `mips_decoder.cpp`, `code_generator.cpp`)
operate on raw MIPS bytes and have nothing wrong with them. They just
need a new entry point that ingests a flat ROM at `0xBFC00000` instead
of a `PS-X EXE`-headered file, plus extensions to `code_generator.cpp`
to handle COP0 kernel-mode instructions the BIOS uses.

**The runner from v3 was not salvaged.** Specifically:

- `bios.c` (1808 LOC HLE shims) — discarded
- `interpreter.c` (919 LOC MIPS interpreter) — discarded
- `events.c`, `threads.c` — discarded (recompiled BIOS manages its own EvCB/TCB)
- `bios_trace.c`, `func_logger.c` — discarded (interpreter-era helpers)
- `main_runner.cpp` — discarded (drove the interpreter)

The hardware simulation files from v3 (`memory.c`, `gpu.c`,
`gpu_sw_renderer.c`, `dma.c`, `interrupts.c`, `timers.c`, `sio.c`,
`memcard.c`, `cdrom.c`, `iso_reader.cpp`, `gte.cpp`, `spu.c`,
`debug_server.c`) are **eligible for salvage in Phase 2** when v4
needs them, but they will be copied in **one at a time**, audited for
HLE-state-leakage and stub patterns first, and only the parts that are
hardware simulation (not BIOS state simulation) are kept.

**Do not bulk-copy `psxrecomp/runner/src/` from v3.** Doing so will
re-import the disease.

---

## 8. Reference the right project for examples

PSXRecomp v4 is a sibling project to:

- **N64Recomp** (RT64 team) — proven static recompilation model for N64
- **SuperMarioWorldRecomp** (`F:/Projects/SuperMarioWorldRecomp/`) — sibling SNES recomp
- **SuperMarioWorldRecomp-oracle** (`F:/Projects/SuperMarioWorldRecomp-oracle/`)
- **NESRecomp** — referenced in v3's debug_server.c comments

When you need to know "how does a recomp project handle X?", read those
projects. **Do not** look at v1 (`F:/Projects/psxrecomp/`) or v2
(`F:/Projects/psxrecomp-v2/`) or v3 (`F:/Projects/psxrecomp-projects-v3/`)
for architectural guidance. They are reference for what failed, not
what worked.

---

## 9. Memory and prior session context

Auto-memory continues to work across sessions. Existing v3-era memories
about printf rules, no-stubs, BIOS-first, DuckStation oracle, etc. all
still apply. New v4-specific memories should be tagged so future
sessions can tell them apart from v3 memories. The most important new
memory is: **"v3 failed because it was an interpreter+HLE emulator
masquerading as a recompiler. v4 fixes this by ACTUALLY recompiling
the BIOS."**

---

## 10. No speculative progress

If a step involves:

- indirect jumps
- relocation
- hardware interaction

You MUST produce:

- manifest
- proof artifact

Code without proof is invalid.

---

## 11. First milestone is absolute

Before any Phase 2 work:

- docs/internal/FIRST_MILESTONE.md must be complete
- boot_slice must compile
- all instructions must be supported

No exceptions.

---

## 12. Relocation is mandatory before full BIOS

Do NOT attempt full BIOS recompilation until:

- BOOT_RELOCATION_PLAN.md is implemented
- address_aliases.json exists
- duplicate code is impossible

---

## 13. No large-step execution

You may NOT:

- "recompile the full BIOS"
- "walk the entire ROM"

Until:

- function discovery pipeline exists
- manifest output is verified

---

## 14. Unknown is acceptable. Guessing is not.

If something is unknown:

→ STOP  
→ produce artifact showing unknown  

Do NOT guess behavior.

---

## 15. Broken tooling is never acceptable. Fix it when identified.

If a tool, command, or verification mechanism fails or returns
unexpected results:

→ **Fix the tool, immediately, the moment you identify the breakage.**
   Diagnose why it failed and repair it before continuing the
   investigation that surfaced it.
→ Do NOT route around it with indirect evidence.
→ Do NOT infer correctness from two broken implementations agreeing.
→ Do NOT log the breakage as a "caveat to live with" or carry it forward
   in handoffs as a known limitation. A known-broken tool is a debt that
   compounds: every later session pays interest in the form of
   reconstructed-from-fragments evidence and shaky conclusions.

"The screenshot command returns black" is not a reason to skip visual
verification. It is a reason to fix the screenshot command.

"Both the native runtime and interpreter show the same wrong value"
does not make the value correct. It means both have the same bug.

"Beetle's fntrace caller_pc field is always 0, so we'll lean on ra
chains" is not an acceptable workaround. It is a reason to fix the
fntrace hook before the next investigation pass.

If you cannot fix the tool, **ask the user** what they observe.
Never declare a result "correct" without direct verification against
the oracle.

---

## 16. Two independent processes, identical debug harness

v4 runs two processes for cross-checking, NEVER one process with both
backends in it:

- **`psx-runtime.exe`** — recompiled BIOS only. SDL window, keyboard
  input, TCP debug server on port **4370**.
- **`psx-beetle.exe`** — Beetle PSX (mednafen-psx libretro core) only.
  SDL window, keyboard input, TCP debug server on port **4380**.

Both binaries expose the **same JSON wire protocol** for debug
commands — `read_ram`, `press`, `set_input`, `clear_input`,
`pad_status`, `wtrace_*`, `fntrace_*`, `screenshot`, `ping`, etc.
This means a tool written against psx-runtime works unchanged against
psx-beetle just by switching ports. Implementations differ (psx-beetle
uses libretro hooks; psx-runtime uses recomp-emitted instrumentation),
but the protocol is identical.

**Why two processes, not one:** the embedded-oracle approach
(`psx-beetleoracle.exe`, retired 2026-05-05) shared input across both
backends in lockstep. They desynced constantly — once their internal
state diverged, the same keypress drove them to different screens, and
"compare on the same press" became unreliable. Two independent
processes is the only setup where each backend can be navigated to its
own state and queried on its own timeline. Cross-process comparison
is done by querying both ports from a tool, NOT by sharing memory.

```bash
# Build beetle-psx static lib (one-time, or after modifying beetle-psx/ source)
cd beetle-psx && make platform=mingw_x86_64 STATIC_LINKING=1 HAVE_LIGHTREC=0 -j8
cp mednafen_psx_libretro.dll libmednafen_psx.a && cd ..

# Build both binaries
PATH=/c/msys64/mingw64/bin:$PATH
cd runtime/build && cmake --build . --target psx-runtime psx-beetle && cd ../..

# Run independently. Either can run alone; both can run together.
taskkill //F //IM psx-runtime.exe 2>/dev/null
taskkill //F //IM psx-beetle.exe   2>/dev/null
start "" "./runtime/build/psx-runtime.exe" "./bios/SCPH1001.BIN"
start "" "./runtime/build/psx-beetle.exe"  "./bios/SCPH1001.BIN"
```

**Key files:**
- `beetle-psx/` — cloned beetle-psx-libretro repo
- `beetle-psx/libmednafen_psx.a` — static library used by psx-beetle
- `runtime/src/main.cpp` + `runtime/src/debug_server.c` — psx-runtime
- `runtime/src/beetle_main.cpp` + `runtime/src/beetle_debug_server.c`
  + `runtime/src/beetle_libretro.cpp` — psx-beetle

**Where Beetle has gaps, build new tooling.** Do not fall back to
DuckStation. Do not guess. Build the diagnostic tool that gives you
the visibility you need.

---

## 17. Phase 5 gate — fix hardware stubs before loading a game

`docs/internal/STUBS_TO_FIX.md` lists every known stub in the runtime. Before any
Phase 5 work (loading Tomba or any game EXE), every stub marked
"Phase 5+" in that file **must be implemented and oracle-verified**:

- **S3 — MDEC decoder** (FMV playback)
- **S4 — SPU audio synthesis** (sound output)
- **S5 — DMA channels 0/1/3/4** (MDEC, CDROM, SPU data pipes)

These cannot be tested until disc data flows, but they cannot be
skipped either. The first task of Phase 5 is to implement them, not
to load the game and see what breaks. Loading the game with known
stubs is how v3 ended up with 1808 lines of shims.

---

## 18. Self-modifying / install-at-runtime RAM is interpreted, not HLE'd

The PSX BIOS dynamically writes 4-instruction dispatch stubs into kernel
RAM (e.g. RAM 0xCF0 for the SIO data-byte handler) and then transfers
control to those addresses. The static recompiler CANNOT see those
instructions, because they don't exist at compile time — only the
program's intent to install them does.

**The correct answer is to interpret, not to HLE.** A small MIPS
interpreter in the runtime tracks writes into the kernel-RAM code region,
marks affected pages "dirty", and runs any `psx_dispatch` whose target
falls in a dirty page through the interpreter. The interpreter executes
the program's own instructions on the CPU register state. After the basic
block, control returns to static-recompiled C.

**This is not HLE.** HLE means "the program would have produced result X,
so we synthesize X ourselves and skip the program's code." This rule is
the opposite: we run the program's code, exactly as the BIOS author wrote
it. The only difference from a pure static recompile is the *source* of
the instructions (RAM-written-at-runtime vs ROM-at-compile-time).

**This rule does NOT relax Rule 0.** The interpreter is not a fallback for
code the recompiler failed to translate. If a function exists in ROM at
recompile time, it MUST be statically recompiled. The interpreter only
runs against PCs in pages that have been written to since boot — i.e.,
code that was put there at runtime by the program.

Mature static-recompilation projects (N64Recomp, mednafen-PSX's dynarec)
all handle install-at-runtime code this way. PSXRecomp v4 follows suit.

Implementation lives in `runtime/src/dirty_ram_interp.c` (or similar). It
is intentionally small (~300 LOC), modular, and isolated. It does NOT
expand into a general-purpose CPU emulator.