# PSXRecomp Overlay System — Feature Reference

## Overview

PSX games use **overlays**: regions of game code that are loaded from disc into a fixed
RAM window at runtime via CD DMA. The same physical addresses are reused by different
overlay images throughout the game. The static recompiler only sees the boot EXE; it
cannot see overlay code that does not exist at compile time.

The overlay system has two layers, B and A, that together solve this.

---

## Layer B — Capture and static compilation (IMPLEMENTED, B-1 done / B-2 in progress)

### B-1: Overlay capture (runtime, done)

`overlay_capture.c` hooks every CD DMA completion into game-code RAM
(`dest < 0x1C0000`). On each unique load address, it stores the DMA-time bytes
(unpatched copy, taken before the game can modify them) in an in-process set.

On clean process exit, `overlay_capture_write_json()` writes
`build-dev/overlay_captures.json` alongside the executable. The JSON contains:

- `load_addr` — virtual address where the overlay loaded (e.g. `0x800E7000`)
- `size` — byte count of the full dirty region
- `bytes_b64` — base64 of the DMA-time image (zero-filled for scatter-load gaps)
- `seeds` — per-PC interpreter hit addresses (execution-verified entry points)

The `seeds` list is how we know which addresses inside the overlay are real function
entry points. They come from `dirty_ram_interp`'s PC table: addresses the interpreter
actually branched to during gameplay.

### B-2: Static overlay compilation (build-time, in progress)

`compile_overlays.py` reads `overlay_captures.json`, wraps the bytes in a fake PS-EXE
header, runs `psxrecomp-game` to recompile, post-processes the generated C for
standalone compilation, and produces a C file that is compiled into the binary.

In **static mode** (B-2), the output is a `.c` file placed in `generated/overlays.c`
that the CMake build includes directly. No DLL, no runtime loading, no CRC matching.
The overlay functions join the static dispatch table exactly like game EXE functions.

`game.toml` gains an `[[overlays]]` section that points to the captures file; CMake
includes `generated/overlays.c` when it exists.

---

## Layer A — Dynamic DLL loading (runtime) — SHIPPED

> **Status corrected 2026-07-27.** This section described Layer A as "SHELVED".
> It is not: dynamic DLL overlay loading is the **live production dispatch
> path**. `runtime/src/overlay/overlay_loader.c` is ~4,400 lines with 15 `LoadLibrary`
> call sites and was last modified 2026-07-26;
> [`COMPILING_OVERLAYS.md`](COMPILING_OVERLAYS.md) documents building the
> `cache/…/<phys>_<crc>.dll` shards that ship inside release packages. The
> shelving described below was a real moment in the project's history, and the
> account of the CRC-keying dead ends is worth keeping — but it was superseded.
> Content-hash dispatch replaced the unstable CRC key, and the Load Game crash
> was subsequently root-caused rather than worked around.
>
> Read everything below this banner as **history**, not current state. For the
> current pipeline see [`COMPILING_OVERLAYS.md`](COMPILING_OVERLAYS.md) and
> [`AOT_OVERLAY_PLAN.md`](AOT_OVERLAY_PLAN.md).

### What it was supposed to do

A-1 was the "live" complement to B: while B produces a binary rebuild, A-1 lets a
user contribute a new overlay by dropping a compiled DLL into a cache directory.
The runtime detects the overlay firing in `dirty_ram_dispatch`, finds the matching
DLL in the cache, loads it with `LoadLibrary`, and wires the overlay functions into
a dynamic dispatch hash table — all without a rebuild.

The idea was that contributors could run `compile_overlays.py`, drop the DLL in
`build-dev/cache/<game-id>/`, and immediately have compiled overlay code.

### What we built

- `overlay_loader.c` — dynamic dispatch hash table (16 384 slots), DLL load + PE
  export enumeration, `overlay_loader_dispatch` called from `dirty_ram_dispatch`
- `overlay_api.h` — shared ABI: `OverlayCallbacks` struct, `overlay_init` entry point
- `compile_overlays.py` DLL path — generates `{addr}_{crc}.dll` in the cache dir
- `overlay_loader_status` TCP debug command

### Why it was shelved

**Load Game regression.** When a DLL exists in the cache, loading a save triggers
the overlay dispatch, which loads the DLL, which runs the compiled overlay code.
This crashed consistently. Clearing the DLL cache restores stable behavior. We never
confirmed whether the compiled code itself is correct (the crash analysis was blocked
by the plumbing issues below).

**CRC key instability.** We tried three different naming schemes before landing on
`{addr}_{dma_crc}.dll`:

1. CRC of live RAM — wrong: scatter-load gaps contain old data in live RAM but zeros
   in the DMA-assembled image, so the CRC never matched.
2. Address-only — no collision safety.
3. DMA-CRC (from `overlay_capture_get_region_crc`) — correct in theory, but the DMA
   blocks are not in `s_entries` after a fast-boot restore (no DMA fired), so the
   function hashed a zeroed image and produced the wrong CRC.

**Final fix attempted:** scan the cache dir at init for `{addr}_*.dll` files and
build a `region_start → path` index. This avoids recomputing the CRC at dispatch
time. The strlen check in the scanner had an off-by-one (22 vs 21) that was fixed,
but the Load Game crash was not diagnosed before shelving.

### What remains for A-1 to ship

1. Confirm the generated DLL code is actually correct (same question as B-2).
   Debug with `B476006F_patched.c` in `build-dev/cache/SCUS-94236/`.
2. Understand why Load Game crashes with the DLL active.
3. Possibly audit `patch_generated_c` in `compile_overlays.py` for incorrect
   cross-call patching or missing extern declarations.

The infrastructure is complete. The blocker is code correctness, not plumbing.

*(Historical. All three items were subsequently resolved — the CRC key was
replaced by content-hash dispatch and the cache layout now carries an emitter
tag, `<game-id>/<gcc|tcc>/<os>-<arch>/cg<N>_<hash>/<phys>_<crc>.dll`.)*

---

## Tomba overlay map (known)

| Region         | Load addr    | Size    | Seeds | Status              |
|----------------|--------------|---------|-------|---------------------|
| `000E7000`     | `0x800E7000` | 364 544 | 66    | Captured, DLL built, shelved |

More overlays will appear as gameplay progresses past Dwarf Village.
