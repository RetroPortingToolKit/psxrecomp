# Function discovery — `psxrecomp-analyze`

A developer-facing static analysis tool for PS-X EXE images. It exists because
the recompiler already derives everything a modder or decompiler wants —
function boundaries, the call graph, jump tables, argument usage — and then
throws it away after emitting C. This turns that knowledge into an artifact.

It is **not** part of the recompilation path and does not link `runtime/`.

```
recompiler/include/analysis_db.h    schema + the two rules
recompiler/src/analysis_db.cpp      the analysis passes
recompiler/src/analysis_export.cpp  JSON / symbols.toml / Ghidra / TSV writers
recompiler/src/bios_call_names.cpp  A0/B0/C0 kernel call names
recompiler/src/main_analyze.cpp     the CLI
```

---

## The two rules

**1. Static only.** No trace import, no overlay capture set, no executed-PC
feedback, no runtime translation unit in the link. Every function, edge, and
jump table reported was proven from the executable image alone. What static
analysis cannot reach is printed as a *gap*:

```
Static coverage gap (NOT filled from any runtime source):
  132 indirect transfer(s) have no proven target set.
  255504 bytes (40.1%) of the image belong to no code function.
```

The guarantee is enforced structurally: `recompiler/CMakeLists.txt` lists the
tool's sources explicitly and none of them come from `runtime/`.

**2. Tolerant, not fabricating.** CLAUDE.md §0 says an unsupported opcode is a
hard failure — correct for the recompiler, wrong for a tool someone points at a
weird region. The analyzer degrades instead: a region it cannot fully decode is
marked `partial` with the offending PCs listed. That is not a stub. It emits no
code and asserts no semantics it did not prove. Reporting *incomplete* is
allowed here; reporting *fabricated* is not.

---

## Usage

```bash
psxrecomp-analyze <EXE> [options]

# typical: analyse, write the JSON bundle, keep a diffable TSV
psxrecomp-analyze disc/SLUS_005.62 --symbols symbols.toml \
    --out analysis --emit-tsv analysis/functions.tsv
```

Key options: `--exact` (reachability-only partition), `--seeds <file>`,
`--emit-symbols` (merge into `symbols.toml`), `--emit-ghidra`,
`--emit-symbol-addrs`, `--disasm <file> --at <addr>`, `--diff <prev.tsv>`.

### Artifacts

| File | Contents |
|---|---|
| `analysis.json` | image metadata, stats, one record per function |
| `edges.json` | call edges: direct, jump-table, address-taken, resolved indirect |
| `indirect.json` | every indirect transfer, resolved or not, with context |
| `refs.json` | statically resolved memory references (largest; `--no-refs` skips) |
| `functions.tsv` | one line per function — grep/awk friendly, and the `--diff` input |
| `symbol_addrs.txt` | decomp-toolchain symbol map |
| `psxrecomp_import.py` | Ghidra script: creates functions, applies names, bookmarks unresolved sites |

The JSON files are the **stable public interface** for future front ends or
project-level CLI wrappers. Consumers should read those artifacts, never the
analyzer's in-process types.

---

## What it recovers

**Function boundaries** come from `FunctionAnalyzer` (the same code the emitters
use), then get scored:

| Confidence | Meaning |
|---|---|
| `verified` | prologue + epilogue + `jr $ra`, fully decoded, called from reachable code |
| `high` | `jr $ra` exit, reachable from the entry point |
| `medium` | prologue-shaped, or address-taken with no proven call path |
| `low` | boundary inferred; unreachable, no prologue, or partial decode |
| `data` | classified as data masquerading as code |

Every row also carries `confidence_reason`, so a claim can be argued with.

**Jump tables.** `resolve_exact_bounded_jump_table()` in `function_analysis.cpp`
is tried first and is authoritative when it fires, but it accepts exactly one
instruction schedule because a wrong table there emits wrong *code*. That is the
wrong trade for a discovery tool: on Masters of Teras Kasi it resolved **0 of
210** indirect sites, because the common schedule puts the `sll` in the branch's
delay slot and materializes the table base between the `sll` and the `addu`.

`recover_jump_table()` walks the actual backward slice instead of matching a
fixed window, and labels how it terminated:

* `jump_table:proven` — the strict resolver accepted it.
* `jump_table:guarded` — an `sltiu` bound on the index register was found, so
  the case count is proven.
* `jump_table:scanned` — no bound found; the table was walked until a word
  stopped looking like a code address. **A hypothesis**, and labelled as one.

**Indirect calls** through a statically-known pointer — materialized inline, or
loaded from a fixed address holding a known function start — are resolved as
`call_ptr:immediate` / `call_ptr:static_slot`. Pointers from struct fields or
computed addresses stay unresolved, correctly.

**Kernel dispatch thunks.** PSY-Q titles call the BIOS through three-instruction
stubs (`addiu $t2,$zero,0xA0; jr $t2; addiu $t1,$zero,<idx>`). The generic rules
score these `low` — no prologue, no `jr $ra` — yet they are among the most-called
functions in any image. They are detected by exact pattern and named from the
A0/B0/C0 tables (`bios_call_names.cpp`, sourced from psx-spx), so
`func_800749DC` with 41 callers reads as `bios_A0_3F_printf` instead.

**Signatures** are inferred by a read-before-write scan: arguments in `$a0–$a3`,
return in `$v0/$v1`, callee-saved set from the prologue's stores, leaf/non-leaf,
GTE/MMIO/syscall use, frame size. This is a **heuristic** — a loop that reads a
register before the backward edge writes it over-reports an argument — so each
record carries `sig.confident`, false whenever the scan hit such a construct.

### Measured on three titles

| Title | Functions | Tables recovered | Indirect left | Coverage |
|---|---|---|---|---|
| Masters of Teras Kasi (`SLUS_005.62`) | 1331 | 46 (804 targets) | 132 | 59.9% |
| Twisted Metal 4 (`SCUS_945.60`) | 1099 | 11 (236 targets) | 185 | 76.1% |
| Klonoa (`SLUS_005.85`) | 230 | 2 (126 targets) | 74 | 71.5% |

Klonoa's boot EXE is 45 KB because the game streams in as overlays — a good
illustration of the limit below.

---

## Where static-only costs coverage

Two classes remain, and the tool says so rather than papering over them:

* **`jalr` through a runtime pointer** — a vtable slot or callback filled in at
  init. No static answer exists in general.
* **Overlay bodies** — for an overlay-heavy title, most of the code is not in
  the boot EXE at all. Closing this statically means parsing the disc
  filesystem, locating overlay blobs as files, and recovering load bases by
  analysing the loader's own `CdRead`/`memcpy` calls. Tractable, not done here.

`tools/overlay_xref.py` mines the runtime's capture set for exactly this, and it
works — but it is dynamic evidence. The rule this tool follows is not "runtime
never touched"; it is **you always know which claims are static**. If capture
import is ever added here, it must arrive as a separately-labelled source, never
silently merged.

---

## Symbol-reference qualification (2026-11-05)

### Complete archive-directory follow-up

The follow-up audited all 115 archives in the PlayStation directory, including
18 CPE inputs converted privately into PS-X EXE envelopes without changing
their loaded bytes. The corpus contains 173 executable views and 163 unique
input hashes. Symbols remain comparison references, never discovery seeds.

Against release v0.3.5-alpha (`3e3438763fd207751c94b2f986266dc7a1f659ba`),
the shared analyzer recovers **377 additional function-entry instances** with
**zero previously matched explicit entries lost**. Recoveries occur in 32
archives. Instances are deduplicated by executable SHA-256 and entry address;
regional revisions and different executable containers can still contain the
same implementation. This is not a count of distinct source functions.

The evidence supports these general changes:

- Recognize argument loads, register moves and canonical o32 argument home-slot
  stores before frame allocation. Longer arithmetic preludes require an
  independent return boundary or static address reference, with protected
  registers and control flow checked.
- Preserve exact direct-call and forced entries when an earlier prelude also
  qualifies. Recognize packed terminal-jump wrappers without absorbing the next
  frameless routine.
- Recognize bounded, ABI-preserving leaf callbacks referenced by pointer data
  or `LUI` plus `ADDIU`/`ORI`, including a low half in a call delay slot. Keep
  resolved switch cases owned by their host function.
- Track the complete materialized constant in reference analysis, invalidate
  clobbers and recognize `ORI` address references.

Ninety executable/reference views meet the correspondence checks: matching
basenames where available, at least 90% baseline explicit-entry agreement and
at least 90% agreement between debug frame records and resident instructions.
Those views improve from 105,344/105,729 to 105,724/105,729 explicit starts.
These totals count views; the 377 recovery total removes duplicate input hashes.
Pure function declarations pointing into zero-filled nonresident overlay space
are excluded and recorded. Active-overlay records are not boot-image entries.

Five views retain an ambiguous unreferenced boundary. All five work with the
existing explicit-entry interface; automatic promotion was rejected without
independent boundary evidence. Two symbol files use an unsupported `0x9c`
record and remain explicitly unqualified. Mismatched revisions, untyped MAP
labels and unsupported references do not count as discovery failures or proof
of complete recall. Explicit records establish entry recall, not overall
precision, function extents, switch completeness or gameplay correctness.

The developer CLI archive now includes `libexec/psxrecomp-analyze`, so these
discovery reports are available without building the analyzer separately.
Archive/input identities, correspondence decisions and counts are recorded in
[`accuracy/full_symbol_corpus_results.json`](../accuracy/full_symbol_corpus_results.json).

### Initial eight-build qualification

Eight independent executable/symbol pairs were compared with **unaided**
analysis: Croc, Croc 2, THPS2, Soul Reaver, Driver 2, Quake II, Abe's Oddysee
and 3D Lemmings. Symbols were comparison references, never entry seeds.
The baseline was `81dea78f4606a1e984d52f3ba0fa58aa2528bd2c`.

The comparison exposed one shared discovery defect: incoming o32 `$gp` loads
scheduled before stack allocation were omitted from function entries. The
pre-frame recognizer now accepts those loads and rejects preludes that
overwrite `$gp`, `$sp` or `$ra`. This also removes redundant interior starts.
The exact-entry traversal and instruction translation are unchanged.

| Explicit function records | Baseline entries found | Fixed entries found |
|---|---:|---:|
| THPS2 | 1643 / 1646 | 1646 / 1646 |
| Soul Reaver | 2256 / 2274 | 2274 / 2274 |
| Driver 2 | 970 / 977 | 977 / 977 |
| Quake II | 1030 / 1037 | 1037 / 1037 |

No previously recognized named entry was lost across the eight inputs. The
other four inputs supply untyped labels rather than explicit function records;
they cannot establish complete function recall or precision. A Wipeout XL
pair with inconsistent symbol/disassembly addresses was excluded, not counted
as an analyzer failure. Names alone also do not prove function sizes, switch
targets, or code/data classification.

Machine-readable identities and counts are in
[`accuracy/symbol_reference_results.json`](../accuracy/symbol_reference_results.json).
Original archives, normalized references, comparison script and raw runs stay
in the private qualification workspace, outside the source repository. Synthetic
positive and rejection cases live in the registered `reachable_discovery_test`
and `reachable_discovery_codegen` tests. No game code or SDK content is required
to run those regressions. Consumers need a framework pin bump and regeneration
to use corrected whole-image discovery; no runtime setting enables it.

## Naming loop

`symbols.toml` (see [SYMBOLS.md](SYMBOLS.md)) is the persistence layer. Both
writers are **append/edit-only and never regenerate the file**: it holds more
than functions — Ape Escape has 11 `[[object]]` entries and a top-level `game`
key, Crash Team Racing has 33 `[[site]]` entries, all hand-researched — and an
earlier version that rebuilt the file from parsed `[[func]]` entries silently
deleted every one of them.

```bash
psxrecomp-analyze <EXE> --symbols symbols.toml --emit-symbols \
    --min-confidence verified
```

New functions land as `status = "guessed"` with a note recording why the
analyzer trusted them. A name a human typed always outranks anything inferred,
and an entry this run did not rediscover is carried through rather than dropped.

## Relationship to the emitters

`psxrecomp-game` answers *"what C do I emit for this?"* and must be conservative,
because a wrong answer is a broken build. `psxrecomp-analyze` answers *"what is
this?"* and can afford a labelled hypothesis, because a wrong answer is a wrong
report that a human will notice. They share the boundary detection and the
strict table resolver; they differ in what they are allowed to guess, and the
labels exist so a consumer can tell which is which.

---

## Widescreen site scanning (`--scan-widescreen`)

Widescreen bring-up on a new title means finding a handful of exact instruction
addresses for `[widescreen.cull]`. Tomba's `game.toml` carries four bias/range
pairs and three `a1` nops, each located by hand from disassembly or by mining
the runtime's capture set with `tools/overlay_xref.py`. Every one of them has a
crisp syntactic signature, so for a main-EXE title that search is static.

```bash
psxrecomp-analyze <EXE> --scan-widescreen --emit-ws-sites ws.toml
```

### What it finds

**The per-game screen extents.** These are not fixed — Tomba tests `0x140/0x141`
on a 320 display, Ape Escape `0x181` on 368, Wipeout 3 `0x200/0x240` on 512.
Frequency alone picks the wrong values (Wipeout 3's real immediates appear in
only three funnels, drowned by ordinary bounds checks in 83 GTE functions), so
the scan scores **co-occurrence**: the screen-extent signature is a width
compare and a height compare in the *same* function. Pairs are weighted toward
GTE code and toward the console's actual display modes, then completed with the
inclusive-bound sibling when the image contains a compare against it. That last
step is load-bearing — Tomba's pairing keys on `sltiu_imm - 2*bias == W`, and
`449 - 128` is `0x141`, so without it three of its four pairs vanish.

When neither chosen set lands on a real console mode, the report and the emitted
TOML say so rather than presenting a guess with a confident face.

**`auto_screen_x` coverage.** Functions carrying the screen-extent signature are
listed using `ws_cull_detect.h` — the same detector the recompiler and the
runtime interpreter use, so the verdict cannot drift. Knowing that 22 of Tomba's
functions are already handled without any per-address list is the most useful
thing to learn before hand-listing anything.

**bias/range pairs**, the masked-u16 X window:

```
80022E78  addiu $v0, $v0, 64        <- bias_sites   (+halfwidth)
80022E7C  andi  $v0, $v0, 0xFFFF
80022E80  sltiu $v0, $v0, 449       <- range_sites  (W + 2*halfwidth)
80022E84  beq   $v0, $zero, reject
```

The arithmetic tie between the two immediates is what makes this safe to
propose. Without it, any neighbouring `addiu`/`sltiu` would qualify.

**`a1` margin sites** — a repurposable `nop` before a caller-supplied margin
joins an X term. These classifiers are *callees* of the render funnel, not
funnels themselves (Tomba's uses no GTE op and has no static caller at all), so
the signature is local: the function takes `$a1`, folds it in with `addu`, and
has a nop just before that is not already a branch delay slot.

**`screen_x` sites** — width compares in functions `auto_screen_x` cannot reach,
which are exactly the addresses that still need listing by hand.

### Measured against hand-found ground truth

Tomba's `game.toml` sites were found by hand over the course of the widescreen
work. Scanning `SCUS_942.36` with everything auto-discovered:

| key | recall | candidates proposed |
|---|---|---|
| `bias_sites` | **4/4** | 4 (no false positives) |
| `range_sites` | **4/4** | 4 (no false positives) |
| `a1_sites` | **3/3** | 17 |

Independently, the scan recovers Ape Escape's documented `0x181` and both of
Wipeout 3's configured `0x200`/`0x240`.

### Safety

Every proposal already satisfies the instruction form `code_generator.cpp`
requires — `addi`/`addiu` for bias, `sltiu` for range and screen_x, a bare `nop`
for a1 — because the emitter hard-errors on a mismatch in main-EXE mode. A
suggestion that would break the build is not a suggestion. What the scanner
cannot tell you is whether widening a given site *helps*; that is still
playtesting.
