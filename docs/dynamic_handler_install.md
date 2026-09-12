# Dynamic handler install — RAM 0xCF0 (and friends)

This document describes a class of BIOS behavior that requires special
runtime handling: **the BIOS writes MIPS instructions into kernel RAM
at runtime and then transfers control to those addresses.** A pure
static recompiler cannot follow this — only the runtime-installed
instructions do the dispatch — so PSXRecomp v4 maintains a small MIPS
interpreter for "dirty" RAM pages.

## The concrete case: SIO data-byte handler

When a memcard read command is in flight, the BIOS needs to read 128
bytes of sector data byte-by-byte over SIO. It does this not via the
chain dispatcher (which only has 13 handler slots), but via a stub
installed at RAM 0xCF0 that runs once per SIO IRQ.

### Install path

The function at ROM 0xBFC15EBC is the install entry. It:

1. Clears `mem[0x75c0] = 0` (the "data-mode active" flag).
2. Copies four words from RAM 0x6408 (= ROM 0xBFC15F08) to
   RAM 0xCF0..0xCFC. Those four words are:

   ```
   lui   v0, 0x0
   addiu v0, v0, 0x641C
   jalr  v0
   nop
   ```

   At runtime this resolves to `v0 = 0x641C` (the data-byte handler in
   RAM, which is ROM 0xBFC15F1C reached via the kernel's part-2 RAM
   image), then `jalr v0`. The `nop` is the delay slot.

3. Tail-jumps to a B0 vector trampoline at RAM 0x6A70.

The companion entry at ROM 0xBFC15E80 is the "set [0x75c0] = 1"
function (B0:0x80), which R11 calls on success at the end of the
read-protocol setup phase.

### Dispatch path

When an SIO IRQ fires, the kernel exception handler at RAM 0xC80
(= ROM 0xBFC10780) runs. It saves registers, calls a helper at
RAM 0xEA0, masks the cause register, and (for non-syscall causes)
falls through to:

```
RAM 0xCEC: sw v1, 0x80(k0)    ; save scratch
RAM 0xCF0: lui v0, 0x0        ; ←—— installed stub starts here
RAM 0xCF4: addiu v0, v0, 0x641C
RAM 0xCF8: jalr v0
RAM 0xCFC: nop
```

The installed stub `jalr`s to RAM 0x641C (= ROM 0xBFC15F1C), the
data-byte handler. That handler:

1. Reads `[0x75c0]`. If 0, returns immediately.
2. Reads SIO_DATA (the data byte from the card).
3. Stores it at `*mem[0x75c4]` (the running buffer pointer).
4. Increments `mem[0x75c4]`.
5. Acks SIO IRQ.
6. Returns.

After 128 such invocations, the BIOS clears `[0x75c0]` and the chain
dispatcher resumes for R12 (checksum) and R13 (end-byte 'G').

## Why static recompilation can't follow this

`generated/SCPH1001_full.c` was emitted from the ROM image. At ROM
addresses 0xBFC107F0..0xBFC107FC the bytes are NOPs — that's all the
recompiler sees. It emits:

```c
/* 0xBFC107F0: 00000000  nop */
/* nop */
/* 0xBFC107F4: 00000000  nop */
/* nop */
...
```

The recompiler has no way to know that the BIOS overwrites these
runtime addresses with a working dispatch stub. Even if it did, it
couldn't statically know the destination — `addiu v0, v0, 0x641C` is
a runtime computation against `lui v0, 0x0`, which by itself is
underspecified.

## The interpreter (runtime fix)

### Dirty-page tracking

The runtime maintains a bitmap covering all writable RAM pages
(typically 64 KB at 4 KB granularity = 16 bits). Any write into the
kernel code region (RAM 0x000..0xFFFF) sets the bit for that page.

### Dispatch interception

`psx_dispatch(addr)` checks the dirty bit for `addr`. If clean,
calls the static-recompiled function as before. If dirty, hands
control to the interpreter.

### The interpreter

A small MIPS interpreter (~300 LOC) executes the basic block at
`addr` against `cpu->gpr[]`, `cpu->cop0[]`, etc. It supports the
small subset of instructions that BIOS install stubs actually use
(arithmetic, loads/stores, branches, jr/jalr). Unsupported
instructions abort fatally — they should never appear in install
stubs.

After the interpreter executes the block (terminating at jr/j/jalr),
control returns to `psx_dispatch` for the next basic block. The
combined effect is that BIOS-installed code at RAM 0xCF0 *runs*,
correctly, on the same CPU register state as static-recompiled C.

## The patcher is usually the GAME, not the BIOS (measured 2026-09-11)

The section above describes the BIOS patching itself. In practice the larger
source of patched kernel words is the **game**: every Psy-Q SDK title links
libapi, whose `_patch_gte` / `_patch_card` / `_patch_card2` / `_patch_pad`
routines rewrite kernel RAM during `ResetCallback`. Measured on Breath of Fire
III (SLPS-00990, SDK 3.70) against both BIOS images, 30 s into a boot, five
distinct shapes appear — and only the first fits the original one-PC,
`word != 0` model:

| Shape | Example | Resume |
|---|---|---|
| 4-word `lui/addiu/jalr/nop` stub over ROM **zeros** | retail `0x0CF0`, OpenBIOS `0x281C` | `"jalr"` — the call returns to `+0x10` |
| 11-word prologue rewrite with an inserted `mfc0 v0, Cause` | retail `0x0C88`, OpenBIOS `0x27B4` | `"fallthrough"` — the patched words ARE the function |
| 11 words NOP'd (a driver routine removed) | retail `0x4964` | `"fallthrough"` |
| 5-word `jalr` into game text, `ra` landing word also NOP'd | retail `0x4D98` | `"fallthrough"`, not `+0x10` |
| 4-word `jr` into game text over **real ROM instructions** | retail `0x6444`, OpenBIOS `0x357C` | `"none"` — a `jr` never comes back |

The three patched shapes that overwrite real ROM instructions are invisible to
a zero test, and two of them never return to `ram_addr + 0x10`. That is why a
slot is a **range** with an explicit `resume` (see
[`config_schema.md`](config_schema.md) → install slots), detected by comparing
every word against its ROM-baked value.

## The slot and the bless verifier must compose

Declaring a slot is necessary but not sufficient, and getting this wrong cost
one title its whole kernel budget. The mechanism has three parts that only
work together:

1. **The emitted hook** (`full_function_emitter.cpp`) fires when any word in
   the declared range differs from ROM, and dispatches to the range start so
   the interpreter runs the guest's patched instructions.
2. **The bless verifier** (`memory.c`, via `kernel_patch_ranges.c`) compares a
   kernel body in segments that SKIP the declared ranges. Without this the
   body's whole-extent memcmp fails on the first patch and the body is
   MISMATCH for the life of the process — so native code never reaches the
   hook at all. The two halves were written separately and did not compose:
   Breath of Fire III's card stub landed at exactly the address
   `bios/SCPH1001.toml` already declared, and the exception handler still
   interpreted **1.22 billion instructions**, 44.6 % of all interpreted work.
3. **The interpreter hand-back** (`dirty_ram_interp.c`) surfaces to static
   dispatch when straight-line flow reaches a range's `hi`, which the emitter
   registered as a continuation key. Kernel page 0 is permanently dirty (the
   handler saves registers there on every exception), so without this the
   interpreter would run the entire function after the patch rather than just
   the patched words.

`PSX_KERNEL_PATCH_RANGES=0` drops the declared ranges at runtime, restoring
the whole-body memcmp. One binary then measures both sides of the change
(companion to `PSX_KERNEL_BLESS=0`, which disables the mechanism outright).

## Discoverability

Finding the ranges for a new image is a diff, not a hunt: snapshot the live
kernel RAM copy, compare it word-for-word against the ROM source it was
copied from, and keep the differences that land inside a compiled body.

- Read the kernel-bless window and ROM offset out of the linked image
  (`psx_bios_image`), read live RAM over the debug server, and diff.
- Attribute each differing word to a body using the emitted body table; words
  outside every body are data (event tables, TCB saves, pad tables) and
  unbless nothing — do not declare them.
- Group contiguous differing words into one range, then read the LIVE words to
  pick `resume`: a `jalr` returns to `+0x10`, a `jr` never returns, and
  anything else falls through to the range end.
- Snapshot twice (early and late). Some patches land at boot and some on the
  first card access, and a range that appears only late is still a range.
- Cross-check `kernel_bless` over the debug server: `mismatch` should fall to
  the bodies you have NOT declared, and `patch_skips` should be non-zero.

A word whose writer you cannot identify is a word you should not declare: the
declaration tells the verifier to stop checking it. Leaving it undeclared
costs performance in one body; declaring it wrongly costs correctness
everywhere (CLAUDE.md Rule 14).

Slot addresses are per BIOS revision AND per libapi release. Re-measure on a
second title before assuming a profile's list is complete; the schema
tolerates declaring the union.

## NOT HLE

This mechanism is explicitly *not* high-level emulation:

- We do not synthesize what the handler "would have produced".
- We do not intercept events, mark them delivered, or short-circuit
  any BIOS state machine.
- We do not write C reimplementations of `_card_read`, `OpenEvent`,
  or anything else.

We let the BIOS run its own code. The only thing we add is the
ability for that code to live in dirty RAM rather than ROM. That is
a property of the program; we accommodate it. See CLAUDE.md Rule 18.
