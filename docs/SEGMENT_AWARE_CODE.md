# Segment-aware code

Status: design proposal (2026-09-28). The four owner decisions in §10 were
settled on 2026-09-29; each follows this document's recommendation.
Based on master. It was first stacked on RetroPortingToolKit/psxrecomp#417
(`fix/overlay-segment-alias`), which merged on 2026-09-29 together with #418
(mult/div deadlines) and #420 (store-PC forwarding, ABI v24). Rollout PR A
merged as #429 on 2026-09-29 (§8). This change adds only this document and an
acceptance test; it does not change behaviour. Rollout PR B
(`refactor/emitter-runtime-pc`, stacked on this design) implements §5.2,
rollout PR C (`feat/kuseg-linked-exe`, stacked on B) implements §5.3 and §5.5,
rollout PR D (`feat/segment-variants`, stacked on C) implements §5.4, the
exact return checks of §5.5 and the KSEG1 variants of §5.6, and rollout PR E
(`feat/overlay-segment-keys`, stacked on D) implements §5.7.

Acceptance test: `recompiler/tests/test_segment_aware_codegen.py` (ctest
`segment_aware_codegen`). Synthetic EXE: `tools/segment_testrom/gen_segment_exe.py`.

## 1. Summary

A PS1 PC carries a segment:
- KUSEG `0x0xxxxxxx`
- KSEG0 `0x8xxxxxxx` (cached)
- KSEG1 `0xAxxxxxxx` (uncached)

The three map the same physical RAM. The CPU keeps the segment in:
- every link value;
- every EPC;
- every I-cache tag.

KSEG1 also changes what each fetch costs. Compiled code bakes a single segment
into all of these, so it is only correct for PCs in that segment.

#417 made overlay shards fail closed for other segments. Three problems
remain:

1. **KUSEG-linked EXEs.** Static game code for these executables runs at KUSEG
   PCs through KSEG0-baked bodies (Kula World SCES-01000, Alien Resurrection
   SLUS-00633).
2. **Overlay code at KUSEG or KSEG1.** Overlay code that genuinely executes at
   KUSEG or KSEG1 is interpreted forever, because nothing records or compiles
   its segment.
3. **Uncached fetch charging.** Compiled KSEG1 code charged the uncached +4
   fetch only at cache-line leaders; Beetle and the interpreter charge it for
   every instruction. In the BIOS ROM, 5,477 of the 9,592 KSEG1 instruction
   sites in OpenBIOS were undercharged. PR A (#429) fixed that in both
   emitters (§5.6). Game code still has no KSEG1 bodies to charge (§5.4).

**Recommendation: per-segment compiled variants, not segment-relative
emission.**
- Code identity becomes the full runtime virtual address: segment plus
  physical address.
- The emitter takes the segment as a compile-time constant. This generalizes
  the BIOS emitter's existing `relocate_ra`.
- A body exists for each `(segment, entry)` that is actually executed:
  - static code: the EXE's link segment, plus variants requested with
    segment-qualified seeds;
  - overlays: the segments the capture observed.
- A PC with no body for its segment is a loud dispatch miss. In game and
  overlay code it never runs another segment's body. The compiled BIOS, whose
  dispatch is keyed by the normalized address, still runs its window's body
  for such a PC, but records it as a BIOS segment miss (§5.4).
- Uncached variants charge fetch on every instruction.

For every existing KSEG0 title the emitted code is byte-identical, so the
static fast path costs nothing. Segment-relative emission would cost
+3.8 % code size for the constants alone, and +27.6 % with exact KSEG1
charging (measured on R4, §6).

## 2. What the segment changes: the Beetle model

Beetle is the oracle (CLAUDE.md §2). The citations below are to
`libretro/beetle-psx-libretro` `mednafen/psx/cpu.c` @ `a7f0811`. The in-tree
transcription, `runtime/src/psx_icache.c`, cites the older in-tree
`cpu.cpp:534-601`; the semantics are the same.

| Architectural value | Beetle | Depends on segment? |
|---|---|---|
| Link of `jal`/`jalr`/`bgezal`/`bltzal` | `GPR[31] = new_PC`, i.e. PC+8 (`DO_BRANCH`, 1012-1031) | yes: PC's segment |
| `j`/`jal` target | `((PC+4) & 0xF0000000) + (target << 2)` (JAL 1810-1818) | preserves PC[31:28] |
| Branch target | PC-relative | preserves the segment inside any RAM window |
| EPC | `CP0.EPC = PC` (−4 in a delay slot) (`CPU_Exception`, 846-850) | yes |
| I-cache hit | `ICache[(addr & 0xFFC) >> 2].TV == addr`: the **full virtual address** (744) | yes: KUSEG and KSEG0 aliases never share a line |
| Uncached fetch | `addr >= 0xA0000000` or BIU bit 11 clear: **+4 on every fetch**, no fill, clears the load give-back (746-763) | yes: KSEG1 |
| Cached miss | +3, plus 1 per word refilled to the line end; tags written with the full address (764-831) | yes |
| Data loads and stores | `addr_mask[addr >> 29]`: KSEG0/KSEG1 reduced to physical | no |

The interpreter (`dirty_ram_interp.c`) already follows every segment row: it
links with `pc + 8` (2019), computes `j` targets from `pc + 4` (1905), and
fetches at `cpu->pc` (1554). It does not model the BIU cache-disable case
(§9).

**Invariant used by this design.** Direct control flow never changes segment:
- `j`/`jal` keep PC[31:28];
- branches stay within ±128 KiB of the PC.

Only indirect transfers switch segment: `jr`/`jalr`, exception entry, and the
return through EPC. All three already go through dispatch with the full PC.
So a body compiled for segment S only ever reaches other S bodies by direct
edges.

**Oracle deviation (decided 2026-09-29, §10).** Beetle's own comment
(719-730) says hardware clears bit 31 of a KSEG0 fetch address before the tag
compare and the tag write. On hardware, KUSEG and KSEG0 aliases therefore *do*
share lines. I-cache tags follow the oracle, as `psx_icache.c` does today. The
acceptance test pins this: its Beetle model and `psx_icache.c` must agree.
Moving to hardware semantics would be a separate, oracle-backed change. §5.6
keeps the tag computation in one place so that change stays one line. The
hardware difference is recorded under ACCURACY_BURNDOWN axis 4
("KUSEG/KSEG0/KSEG1 mirroring").

## 3. Where psxrecomp loses the segment today

### 3.1 Baked PCs in the game and overlay emitter (`code_generator.cpp`)

Before PR B, every one of these sites wrote the compile address, which is
always KSEG0. The emitter functions are named instead of line numbers, which
move with every change.

| Site | Emitter | R4 static count |
|---|---|---|
| `cpu->gpr[31] = 0x…u` (jal, jalr, bgezal, bltzal links) and a `jalr` link in a branch delay slot | `translate_basic_block`, `translate_instruction` | 5,373 |
| `psx_icache_fetch(cpu, 0x…u)` (fetch tags) | `translate_basic_block` (`emit_pre_icache`) | 61,181 |
| `psx_check_interrupts_at(cpu, 0x…u)` (resume PC → EPC) | `emit_interrupt_check`, ~20 callers | 30,596 |
| `cpu->pc = 0x…u; return;` (CPS exits, stale-static guard, image-edge tail transfer) | `translate_basic_block`, `generate_function`, `generate_alias_group`, `emit_stale_static_guard` | 6,927 |
| `g_debug_last_store_pc = 0x…u` (before every `sb`/`sh`/`sw`/`swl`/`swr`/`swc2`) | `translate_instruction` | 22,936 |
| CPS continuation keys `case 0x…u: goto block_…` | `generate_function`, `generate_alias_group` | — |
| Reserved-instruction EPC `cpu->cop0[14] = 0x…u` | `translate_basic_block` | — |
| `psx_slice_block(cpu, 0x…u, …)` (the interpreter resumes here) | `translate_basic_block` | — |
| `psx_vsync_query_hle_enter(cpu, 0x…u, …)`: `load_accel.c` bases fetch tags, store-PC stamps and resume PCs on it | `generate_function` | — (R4 configures none) |

The R4 figures come from all 50 generated shards: 2,994 functions and
141,335 emitted instruction sites.

**The store-PC stamp is guest-affecting, not a debug hook.** Its name and the
emitted `extern` comment describe debug attribution, but the runtime reads it
in every build:
- `memory.c` `psx_write_word_raw` drops a word store to RAM `0x0`-`0xF` when
  the stamp equals one of a list of exact PCs. An opt-in Tomba card filter
  (`PSX_TOMB_CARD_EVCB_PROTECT`) keys on it the same way.
- `memory.c`'s GP0 write path compared it with `0xBFC38B1C` before PR B
  (§9 has the re-key), a BIOS store to GP0. On a match it hands `gpu_set_gp0_source()` a RAM source
  key taken from `$a0`. No preprocessor gate covers this, and `main.cpp`
  binds `debug_cpu_ptr` at startup (`debug_server_set_cpu`), so it also runs
  in every build. The resulting `gp0_cmd_source_addr` feeds the opt-in
  presentation paths (widescreen prim matching, the `ws_*` code in `gpu.c`,
  geometry/texture correction, mod texture keys) and prim-ring diagnostics.
  The faithful rendering path does not branch on it.
- The interpreter stamps the full executing PC (`dirty_ram_interp.c`). A
  compiled body that stamps a different segment than the one it
  runs in can make a filter or key match in one execution path and miss in
  the other.
- #420 (`fix/fingerprint-guest-facts`, ABI v24, merged 2026-09-29) makes
  overlay shards write the host's copy in every build. Before it, overlay
  stores wrote a private copy,
  so after an overlay store the filters saw the PC of an older store.

So the stamp is a baked PC like the others in the table, and §5.2 routes it
through `runtime_pc()`.

Two other groups carry PCs but are identity keys, not architectural state:
- debug and identity hooks: `debug_server_*` (the runtime masks
  `debug_server_cyc_observe`'s argument to physical),
  `psx_mod_function_entry`, `psx_datashard_enter` and `psx_native_bad_entry`'s
  owner;
- `.ranges` manifests.

The `cosim_*` hooks (the `PSX_COSIM` build only) record the executing PC at
their checkpoints, as the interpreter does, so PR B routes them like the
table.

Before PR C, the dispatch file (`main_psx.cpp`, `psxrecomp_game_main`):
- keyed its table by KSEG0 address and looked it up with the physical
  address (`psx_game_find_entry`, `want = addr & 0x1FFFFFFF`);
- then set `cpu->pc = entry->resume_pc`, which is a KSEG0 constant.

A KUSEG or KSEG1 PC therefore entered the KSEG0 body and continued in KSEG0,
and `test_kuseg_dispatch_lookup.py` asserted exactly that alias behaviour.
PR C (§5.5) moved the emitter to `game_dispatch_emitter.cpp`, made the lookup
exact and rewrote the test: an alias with no body of its own misses.

### 3.2 KUSEG-linked EXEs

Before PR C (§5.3), `ps1_exe_parser.cpp` rewrote the header's KUSEG addresses
to KSEG0 in place and recorded no original segment. It also rejected a KSEG1
entry. Three things followed:
- The whole game compiled for KSEG0 while hardware runs it at KUSEG. Every
  link it saves on the stack and every EPC was `0x8…` instead of `0x0…`.
  Every fetch tag differed from the one the BIOS, the interpreter and Beetle
  use.
- **Seeds in the game's own segment were silently dropped.** `main_psx.cpp`
  range-checked seeds against the normalized `load_address`, so a seed
  written as `0x000100E4` was ignored. It had to be written `0x800100E4`.
- R4 is linked at KSEG0 and is unaffected.

### 3.3 Aliases

Before PR C, a KSEG1 or KSEG0 alias of static text resolved to the same body
(§3.1). The uncached cases are rare but real: code that ORs `0xA0000000` into
a function address to run it uncached, and cache-maintenance trampolines.
Those ran with the wrong fetch cost and the wrong links. Since C they miss and
run interpreted (§5.5). Since D a segment-qualified seed compiles a body for the
second segment (§5.4).

**Measured in the BIOS (2026-09-29).** SCPH-1001 enters its relocated kernel
through the uncached alias `0xA0000500`. In #429's Beetle gate (§7.2) Beetle
charges the four instructions there as uncached fetches, 20 cycles; native
runs the cached body and charges 11. That is the −9 cycles native − Beetle at
SCPH-1001's kernel entry and first kernel calls. The BIOS emitter needs a
KSEG1 variant of that entry to close it, which PR D (§8) provides: SCPH-1001's
seeds ask for `0xA0000500`, and the first hit of every SCPH-1001 boot anchor
now matches Beetle (§7.2).

### 3.4 Overlays (after #417)

Segment is lost at every stage:
- **Capture:** it records bytes and entry PCs per physical word
  (`g_dirty_ram_{exec,dispatch}_pc_bitmap`). It writes every PC back out as
  `PSX_OVERLAY_CODE_SEGMENT | offset` (`overlay_capture.c` 365, 420-446).
- **Interpreter dispatch:** it sees the full PC, but drops the segment at
  `dirty_ram_interp.c:2814`.
- **Cache names and manifests:**
  - filenames and namespaces start from the physical address
    (`{phys}_{crc}`, `ov_{phys}_…`);
  - `.ranges` F entries are forced to KSEG0 (`overlay_loader.c:942`);
  - exports must be named `func_{KSEG0 entry}`;
  - candidates are indexed by physical address.
- **Gate:** #417's gate (`overlay_loader.c:3690`) interprets every
  non-KSEG0 PC.

In R4 that is about 27 dispatches per frame: OpenBIOS enters its RAM patch
slots `0x0000281C` and `0x0000357C` at KUSEG. Nothing ever asks for a KUSEG
shard. PR E records the segment at capture and keys shards by it (§5.7).

### 3.5 Uncached fetch in compiled code

Fixed by #429 (§5.6). This section records the gap as it was on master
before that PR.

Both emitters emitted `psx_icache_fetch` only at cache-line leaders:
- block leaders, jump-table targets, and `addr & 0xC == 0`
  (the leader test, now `code_generator.cpp:1903` and
  `full_function_emitter.cpp:790`).

The stated reason is that "intra-line followers reached by fall-through are
guaranteed hits". That holds for cached segments only. Every KSEG1 fetch
misses and costs +4 (§2).

The BIOS main ROM runs in place at `0xBFC0…` (KSEG1). In OpenBIOS, 5,477 of
its 9,592 KSEG1 instruction sites had no fetch call. Each was:
- 4 cycles short;
- missing the load give-back clear.

The interpreter charges every one. Compiled and interpreted ROM code
therefore disagreed, against the "one shared per-instruction cost" rule
(CLAUDE.md RULE −1). Ruler #2 and ruler #1 both run in cached RAM, so neither
caught it.

## 4. Options

### A. Segment-relative emission

One body per function takes the segment at run time: from a field set by
dispatch, or from a parameter. It emits `seg | phys` at every site in §3.1.

- **Code and speed.** Every baked constant becomes an OR with a live register.
  That is about 127k sites in R4 (22,936 of them store-PC stamps), before
  continuation keys and switch tables.
  CPS continuation switches can no longer be `case` constants; they need a
  physical-address switch plus a segment check.
- **Exact KSEG1 charging.** The body cannot know statically whether it is
  uncached. So each non-leader instruction (about 80k sites in R4) needs a
  run-time `if (seg >= KSEG1) fetch` test. A block-level surcharge is not
  exact, because each uncached fetch clears the load give-back between
  instructions.
- **ABI.** Every generated function needs the segment source:
  - direct calls must carry it;
  - dispatch must scope it;
  - the overlay DLL ABI (`PSX_OVERLAY_ABI_TAG`) and mod entry hooks change.
- **Measured cost** (§6): +3.8 % `__TEXT` for the constants, +27.6 % with
  exact KSEG1 charging.
- **Benefit:** one body serves any segment, and no variant set has to be
  chosen.

### B. Per-segment compiled variants (recommended)

- The emitter bakes the segment as a constant, as it does now, but reads it
  from a per-compile `code_seg` instead of assuming KSEG0.
- A body is compiled for each `(segment, entry)` the program executes.
- Uncached variants emit a fetch charge on every instruction; cached ones keep
  the leader rule.

Costs:
- Code grows only by the variants actually requested. These are almost always
  small closures, such as a cache-flush trampoline.
- The variant set must be known. The segment-miss ring (§5.5) and capture
  (§5.7) supply that evidence.

Reasons to choose B:
- It keeps the fast path exactly as it is for the home segment.
- It keeps each body's cost model static, so it can be checked against Beetle
  per instruction.
- It follows the model the BIOS emitter already uses: per-window runtime PCs
  through `BiosAddressModel::runtime_pc`.
- Option A spends 4-28 % of every title's code on a case most titles never
  hit.

## 5. Design

### 5.1 Code identity is the runtime virtual address

A compiled body is identified by `seg | phys`:
- `func_` names use it: `func_00010000` for a KUSEG-linked entry,
  `func_A00100F0` for a KSEG1 variant.
- Direct C calls and CPS exits then resolve to the same-segment variant with
  no extra lookup.
- KSEG0 names do not change.

### 5.2 Emitter: one `runtime_pc()` for every baked PC

- `CodeGenerator` gets `runtime_pc(compile_addr) = code_seg | (compile_addr &
  0x1FFFFFFF)`, modelled on `bios_runtime_pc`.
- Every site in §3.1 goes through it, the store-PC stamp included. That is
  9 helpers and about 40 format sites, plus 8 store-PC format sites.
- The helper also covers `generate_alias_group` bodies and the dispatch
  emitter's rows (`addr`, `resume_pc`).
- Identity keys (block labels, `.ranges`) may keep the compile address. The
  store-PC stamp is not an identity key (§3.1).
- `code_seg` defaults to the segment of the image's load address. Until
  §5.3 the EXE parser folded KUSEG headers to KSEG0, so that was KSEG0 for
  every title; since PR C it is the link segment. Either way
  `runtime_pc(addr) == addr` for every address in the image.
- **Acceptance:** regenerating any KSEG0 title is byte-identical. That proves
  the refactor has zero fast-path cost. Closes nothing in the ledger on its
  own; enables §5.3-5.6.

**Implemented in PR B.** `CodeGenerator::set_code_segment()` and
`runtime_pc()`. Routed through it:
- links (`jal`, `jalr`, `bgezal`/`bltzal`, and a `jalr` in a branch delay
  slot, which `translate_instruction` emits) and the call contract's return
  PC;
- fetch tags and `emit_pre_icache`'s uncached test (§5.6);
- interrupt resume PCs and the reserved-instruction EPC;
- CPS exit PCs, continuation `case` keys (function and alias bodies) and the
  alias `entry` keys;
- `call_by_address` and stale-static-guard targets, the slice resume PC and
  the `cosim_*` PCs;
- store-PC stamps, including the widescreen-backdrop and persisted-option
  store sites;
- the VSync-query hook's first argument. `load_accel.c` derives fetch tags,
  store-PC stamps and resume PCs from it, so it is a PC, not an id. Its
  hand-timed body charges cached line-leader fetches only, so the hook is not
  emitted for an uncached code segment: a KSEG1 body runs its own
  instructions, each charged (§5.6). The dispatch-side
  `psx_vsync_query_hle_try` compares the full PC exactly, so another segment
  never takes it (nothing calls `psx_vsync_query_hle_configure` today);
- the PC `psx_unknown_dispatch` reports from a data stub;
- dispatch rows (`addr`, and `resume_pc` when it is a PC).

`func_`/`block_` names, `.ranges`, `debug_server_log_call_entry`, the mod and
data-shard hook ids and `psx_native_bad_entry`'s owner keep the compile
address. So does `debug_server_cyc_observe`: the game emitter passes the
compile address and the runtime masks it to physical.

Jump-table `case` values are data words, the PCs the `jr` reaches, and are
emitted as read. In B each case continues in the current body. That is exact
only while every table value is in the body's own segment. A variant (§5.4)
has tables in another segment than its own, and so does a home or overlay
compile whose table holds another segment's PCs of its own bytes (a
KSEG0-compiled overlay shard of code that runs at KUSEG). From PR D every
game and overlay compile tail-transfers a case whose segment differs from its
own (`CodeGenerator::foreign_segment_case()`); the BIOS emitter applies the
same test in its variants.

The BIOS side is `StrictTranslator::translate(d, runtime_pc)`: the store-PC
stamp, the syscall EPC and the break and unaligned-access PCs use the runtime
PC, while the `psx_ldd_` temporaries and `terminator_target` keep the ROM
address. The full-function emitter passes `relocate_ra()` to every
`translate()` call, including orphaned delay slots and the two calls that
read only terminator metadata, and publishes the runtime PC on a
fallthrough. The seven `memory.c` keys are re-keyed in the same commit, and
gated so that each still matches only its own SCPH-1001 instruction (§9).

Tests:
- `emitter_runtime_pc_test` compiles a small program in CPS, CPS-overlay and
  legacy mode, through `generate_all_functions`. With the code segment left
  at its default, set to KSEG0, set to KUSEG and set to KSEG1, it checks each
  game-emitter PC class above except the dispatch rows, the identity keys,
  and that KUSEG output differs from KSEG0 only in constant segments. The
  program reaches every emission path listed above (the test fails if one
  stops being emitted), including split conditional branches, alias groups,
  data stubs, the image edge, the backdrop and persisted-option store sites
  and a `jalr` in a delay slot. It also checks that a KSEG1 code segment
  charges exactly what a KSEG1 image charges. For the translator it checks
  every PC-bearing form, the deferred loads and both syscall forms (ctest runs
  it a second time with `PSX_CPS=0`).
- The dispatch rows are routed too. In B they were not tested: a real
  executable compiles in its own segment, where `runtime_pc()` is the
  identity. PR C moved the dispatch source to `game_dispatch_emitter.cpp`,
  and `emitter_runtime_pc_test` now emits it with a KUSEG and a KSEG1 code
  segment: row keys and resume PCs carry the segment, `func_` names keep the
  compile identity.
- A single-site mutation sweep (revert one route, rebuild, run the unit test
  in both `PSX_CPS` modes and the ledger) killed 77 of the 80 routes in B.
  The three survivors were the two dispatch-row routes, which PR C's test
  kills, and the continuation key for a call return in the middle of a
  block, which the CFG analyzer does not produce (it starts a block at every
  call return; R4's 2,994 functions have none).
- The ledger gains two regression guards (§7.1).

The BIOS emitter's precedent has holes to close in the same pass:
- the fallthrough `cpu->pc = next_addr` (`full_function_emitter.cpp`);
- the `strict_translator` syscall, break and unaligned-access PCs, which use
  the ROM address;
- the `strict_translator` store-PC stamp, which also uses the ROM address.
  Moving it changes which `memory.c` keys match: the store filters (RAM
  `0x0`-`0xF` and the opt-in Tomba EvCB filter) and the GP0 source key
  (§3.1). Seven of those keys, six store-filter keys and the GP0 key
  `0xBFC38B1C`, are ROM addresses of code that runs relocated, so they must
  be re-keyed to runtime PCs in the same change (§9).

### 5.3 EXE parser keeps the link segment

- `PS1Executable` records `link_segment = initial_pc & 0xE0000000`.
- It keeps physical helpers for byte addressing instead of rewriting the
  header.
- Load address and entry must share the segment:
  - KUSEG and KSEG0 are accepted;
  - KSEG1 is accepted and compiles as an uncached home segment.
- The game emitter runs with `code_seg = link_segment`.
- Seeds are range-checked by physical address, so `0x000100E4` is a valid
  home seed.

Closes `link-segment`, `fetch-tag-segment`, `irq-resume-segment`,
`resume-pc-segment`, `store-pc-segment`, `home-seed-accepted` and
`alias-fetch-coherence`.

**Implemented in PR C.** The image is analysed and compiled at its link
virtual addresses, so code identity is the VA (§5.1) and `runtime_pc()` is
the identity for every home body: a KUSEG-linked EXE gets `func_00010000`,
KUSEG rows, links, fetch tags, resume PCs and store-PC stamps with no other
emitter change.
- `PS1Executable::link_segment()` is the load address's segment. The header
  is kept as written; `phys_load_address()` and `contains_phys()` do byte
  addressing, and the BSS overlap check and the `text_size` bound are
  physical. The load address and entry must lie in the 8 MiB RAM window of
  KUSEG, KSEG0 or KSEG1 and share the segment; anything else is rejected
  with a message that names both.
- `function_analysis.cpp`'s three JAL scans took their targets' top bits
  from KSEG0. They now take them from the delay slot's PC, as every other
  jump decoder does; for a KSEG0 image nothing changes.
- Seeds are range-checked by physical address. A seed in the link segment is
  an entry. A seed for the same bytes in another segment is a variant
  request (§5.4): until PR D it is listed in a `WARNING` with its home
  spelling and not compiled, and never folded into the home body. Seeds
  outside the image are counted. The seeds file's directive lines
  (`producer_range`, `cross_call_allow`, `hosted_interior`) have no variant
  form: one that names the image's bytes in another segment is refused with
  its link-segment spelling.
- `tools/collect_game_misses.py` writes each dirty-RAM miss in the link
  segment (from `--segment`, `--game-toml` or a segmented text range; it
  refuses to guess from a physical one) and each `segment_misses` row at its
  full PC, a variant request. Before C it wrote every seed as KSEG0.
- Config code sites that the emitter or the runtime match exactly, and that
  name the image's bytes in another segment, are refused with the matching
  spelling: `[widescreen]` and `[widescreen.cull]` sites, mod entry hooks,
  hot and data-shard functions, the VSync-query function and its
  event-horizon return PCs (compared with `$ra`), and persisted option
  stores. After C such a site would match nothing, with no message. Kinds
  matched by physical address reach the same bytes in any spelling and are
  not refused: `[[recompiler.patches]]` (byte patches), the full-word-guarded
  `[widescreen.cull]` keep, angle and aspect-cone sites,
  `[[widescreen.signed_x_bound]]`, and `[widescreen.dome]` call sites (the
  runtime masks them). Overlay compiles are exempt; their segment keys are
  §5.7.
- A KSEG1-linked image compiles as an uncached home segment: every
  instruction is charged its own fetch (#429's rule).
- Until PR E, a KUSEG-linked title's overlay code runs interpreted. Its static
  code now enters overlay RAM at KUSEG PCs (direct `jal`/`j` keep the
  caller's segment, and `call_by_address` takes `runtime_pc()`), and #417's
  gate keeps every non-KSEG0 PC off the KSEG0-compiled shards
  (`segment_alias_interp` counts them). Before C the folded KSEG0 static code
  entered them at KSEG0 and ran the shards natively, with KSEG0 links. Since
  PR E those entries are captured with their segment and compile to KUSEG
  shards (§5.7).
- A KSEG0 title that enters its own static text through a KUSEG or KSEG1
  alias (§3.3) runs that path interpreted after it is regenerated, recorded
  as segment misses, until PR D compiles a variant. Before C the alias ran the
  KSEG0 body. Of the in-tree titles only R4 was checked (0 segment misses).

### 5.4 Variant requests and closure

- A seed whose segment differs from the link segment requests a variant, for
  example `0xA00100F0`. This reuses the seeds file; no new configuration
  surface is needed. Decided 2026-09-29 (§10): segment-qualified seeds are the
  only request mechanism, with no `game.toml` table.
- The recompiler compiles the variant's **direct-edge closure** in that
  segment:
  - direct calls;
  - tail jumps;
  - CPS continuation targets.
- By the invariant in §2, that closure is everything the variant can reach
  without dispatch.
- Bodies are emitted once per `(segment, entry)`. Cached variants use the
  leader rule; KSEG1 variants use §5.6.
- A jump-table case whose value lies in another segment tail-transfers to
  dispatch instead of continuing in the variant (§5.2).

Closes `segment-variants`.

Until D, PR C reports such a seed and compiles nothing for it (§5.3), so the
PC stays a recorded segment miss (§5.5).

**Implemented in PR D.**
- `main_psx.cpp` compiles the home image first. `plan_segment_variants()`
  (`segment_variants.cpp`) then maps each variant seed to the home compile's
  final pieces (`CodeGenerator::last_functions()`, after the split pass): a
  seed at a function entry roots that function, a seed at a CPS continuation
  roots its owner, and an alias entry brings its whole alias group (one shared
  body). The closure follows `direct_successors()`: branch and jump targets and
  not-taken paths that leave a function, call targets, call returns split into
  another piece and a fall-through past its last block. A seed that names no
  home dispatch entry stops the build with its home PC. So does a seed in a
  segment that does not map physical memory (`maps_physical()`: KUSEG below
  `0x20000000`, KSEG0 and KSEG1 do). `0x20000000`-`0x7FFFFFFF` and KSEG2 are
  no alias of the image: Beetle's `addr_mask` leaves them unmasked, so they
  address no RAM. Such a seed is refused where the seeds file is read and
  again by the planner, and `collect_game_misses.py` reports such a segment
  miss instead of writing it as a seed.
- Each segment compiles through `segment_view()`, the image with its header
  moved into that segment, so the variant's code identity is its VA (§5.1:
  `func_A00100F0`, `block_A00100FC`, `psx_alias_body_A…`), the generator's
  code segment is the view's, and `runtime_pc()` is the identity again. The
  CFGs are re-derived on the view (the same bytes give the same blocks). The
  split pre-pass is off: the closure is already made of the home pieces.
- `rebase_codegen_config()` moves every exact-match config code site that
  names the image into the variant's segment, so a variant gets the same
  hooks and substitutions as its home body (for example `hot_funcs`, mod
  entry hooks, widescreen sites). Physically matched kinds are kept as
  written.
- The variant's bodies join the home shards and declarations header, its F/R
  records join `.ranges`, and its rows join the dispatch table
  (`emit_game_dispatch()` takes one unit per compile).
- A cached variant is the home C with the segment swapped
  (`kuseg_dispatch_lookup` compares them); a KSEG1 variant charges a fetch
  before every instruction (§5.6).
- A jump-table case whose value is in another segment than its body is not a
  local case (`foreign_segment_case()`); the default tail-transfer dispatches
  it with its full PC. This applies to every game and overlay compile, not
  only to variants. A variant has such cases whenever it switches on its home
  segment's PCs. A home or overlay compile has one only when its table holds
  another segment's PCs of its own bytes, for example a KSEG0-compiled overlay
  shard of code that runs at KUSEG (a KUSEG-linked title's overlays, or
  BIOS-installed RAM code). Such a case used to continue in the KSEG0 body with
  KSEG0 links and fetch tags; now a game-text PC with no row in its segment is
  a recorded segment miss (§5.5), and #417's gate sends an overlay PC to the
  interpreter. Both run it at its own PC. R4's game C and its 26 overlay shard
  sources are unchanged (§8 D); no other title was regenerated here.
- Overlay compiles take no variant seeds; a variant seed there is reported and
  not compiled, as before. Overlay code gets its segments another way: capture
  records the segment of each entry, and each segment's entries compile as
  their own shard (§5.7, PR E).

**BIOS.** The BIOS emitter keys its dispatch by the normalized address, so a
PC in any segment runs the body compiled for its window's segment. A BIOS
seed that is a runtime PC inside a relocated copy window, in another segment
than the window runs in, now asks for a variant (`main_bios.cpp`
`split_segment_variant_seeds()`): the function at those ROM bytes and its
direct-edge closure (branch and J/JAL targets that leave it, the call return
and fall-through past its end) are emitted again with every runtime PC in the
seed's segment (`bios_runtime_pc()` under `g_variant_seg`). The variants and
their continuations get exact-PC rows in `segment_variant_table`. A variant
seed in a segment that does not map physical memory stops the build; a
runtime PC in its window's own segment is no variant request and stays a
discovery seed.

The dispatch still finds the word by its normalized key (so the kernel-bless
guard still applies); `psx_bios_hit_body()` then picks the body for the exact
PC. A variant compiled for that PC runs. Any other PC runs the word's home
body as before, and when it is in another segment than the one that body was
compiled for, it is a **BIOS segment miss**: the home body bakes its own
segment's links, EPCs, fetch tags and store PCs, so the run goes into the
segment-miss ring with kind `bios` and the home PC (TCP `segment_misses`,
`dispatch_stats`, the exit report). The miss is loud, as in game code (§5.5),
but the home body still runs. Sending the PC to the dirty-RAM interpreter
instead would change what runs, which needs a measured case (there is none),
and ROM code has no such path. The fix is a seed in the BIOS profile's
seeds; `collect_game_misses.py` lists such a row instead of writing it into
the game's seeds.
`psx_bios_key_home_pc()`, emitted from the address model next to
`normalize()`, gives each key's home PC: its copy window's runtime address,
or the ROM run in place. SCPH-1001's seeds ask for `0xA0000500` (§3.3). Every
BIOS's dispatch C gains the two helpers; a BIOS without variant seeds keeps
its full C.

Measured (§7.2): through the probe's spin (an LLE disc boot through the shell
into the game, 198 M cycles on OpenBIOS and 399 M on SCPH-1001) neither BIOS
records a BIOS segment miss, so `0xA0000500` is the only alias entry either
boot makes. Without SCPH-1001's `0xA0000500` seed the same run records
exactly that PC (kind `bios`, home `0x00000500`, frame 0).

### 5.5 Dispatch is exact; a segment miss is a dispatch miss

- The table is keyed by the full VA. `psx_game_find_entry` keeps its
  physical-word index. The index now points at the first row for that word,
  and the lookup requires `row.addr == addr`.
- Rows for one physical word are adjacent. A title with no variants has one
  row per word, which is today's shape.
- `resume_pc` is per-variant, so `cpu->pc = entry->resume_pc` is correct by
  construction.
- A physical hit with no row for the PC's segment is a **segment miss**:
  - it is counted and recorded in a TCP-visible ring with the full PC;
  - it takes the existing clean-text-miss path (`dirty_ram_interp.c`
    `clean_game_text_miss`).
- Resolving it follows the project rule for any dispatch miss: add the
  segment-qualified seed, then regenerate.
- It never runs another segment's body.
- Decided 2026-09-29 (§10): a segment miss in static game code interprets
  loudly until the title is regenerated. It does not fail fast.

Closes `segment-miss`. The plan was to make `psx_call_contract`'s
segment-masked return check (`cpu_state.h`, its bail-resolve and `$ra`
compares) exact in the same PR; C deferred it to PR D, below.

**Implemented in PR C.**
- `game_dispatch_emitter.cpp` (moved out of `main_psx.cpp`) sorts rows by
  physical address, indexes the physical word and then requires
  `row.addr == addr`, in both the indexed and the binary-search lookup. One
  compile has one code segment, so a second row for one word is a build
  error until D's variants share the table.
- `psx_game_address_in_text` stays physical (byte identity). A segment miss
  therefore takes `dirty_ram_dispatch_inner`'s clean-text-miss path and is
  interpreted.
- The runtime records it (`psx_segment_miss.c`): when a clean-text miss has
  no row but another segment's alias does, the full PC, that row, `$ra`,
  `$sp` and the frame go into an always-on ring and a per-PC count. TCP
  `segment_misses` returns the summary or `{"tail":N}`; `dispatch_stats`
  carries `segment_miss_total`/`_unique`; `psx_last_run_report.json` has a
  `segment_misses` section.
- `psx_call_contract` stays segment-masked for now. The BIOS dispatch
  loop the full-function emitter generates (`psx_dispatch_impl` and its
  return-boundary helper) makes the same masked return check in four
  places, and one call contract must not have two rules. Before D the
  masked compare can only let a body continue for another segment when a
  callee returns to an alias of its own call site, which no title is known
  to do. D makes the five checks exact together.
- Tests: `kuseg_dispatch_lookup` compiles the emitted lookup in its indexed
  form and, for a table spanning 2 MiB or more, its binary-search form, and
  queries every segment's alias of every row; `emitter_runtime_pc_test`
  checks that two rows on one physical word are a build error;
  `psx_segment_miss_test` covers the dispatch hook's helper
  (`psx_segment_miss_note`), and `segment_miss_wiring_test` pins its call in
  `dirty_ram_dispatch_inner`, the TCP command, the `dispatch_stats` and
  `ping` fields and the exit-report section.

**Implemented in PR D.**
- A word may carry a row per compiled segment (the home body and its
  variants, §5.4), adjacent and in PC order. When any word has more than one
  row, the lookup finds the word's first row (the index points at it; the
  binary search finds the lowest row of the word) and then searches that
  word's rows for the exact PC. A table without variants emits the lookup as
  before. Two rows for one PC stay a build error.
- The five return checks compare the full PC: `psx_call_contract`'s
  bail-resolve PC and `$ra` check, and the emitted BIOS dispatch loop's
  return boundary, bail resolve, wild-return test and exception-stack
  straddle. A guest that returns to another segment's alias of its call site
  did not return to that body, so the suspended C continuation no longer runs
  there. B's `runtime_pc()` and C/D's per-segment bodies make every legitimate
  link match in full; LLE boots and R4 fingerprints are unchanged by it
  (§7.2). Ledger guard `exact-return-contract`.
- The segment-miss ring carries a kind: `game` (static game text, run
  interpreted) or `bios` (a compiled BIOS window entered in another segment,
  run through its home body, §5.4). TCP `segment_misses` rows and the exit
  report name it.

### 5.6 Per-instruction fetch charging for uncached code

Landed in PR A, #429 (2026-09-29). The rule: an instruction whose **runtime
PC** is uncached is charged a fetch of its own.
- Cached code keeps the leader rule, which is exact for it: the ledger's model
  check proves that against Beetle for the probe block.
- Both emitters emit `psx_icache_fetch(cpu, pc)` before every instruction for
  which `psx_fetch_uncached(pc)` holds (`psx_instr_cost.h`, `pc >=
  0xA0000000`). `psx_icache.c` uses the same predicate: such a fetch never
  hits, so it clears the load give-back, adds +4 and fills nothing, as Beetle
  does. The A0/B0/C0 call-vector stubs charge one fetch per executed word.
- The same PR fixed the BIOS emitter's cached line-start test, which used the
  ROM address. It now tests the runtime PC (`relocate_ra`): OpenBIOS copies
  its kernel from ROM `0x1FC1E4D4` to RAM `0x500`, which shifts bits[3:0] by
  4, and 1,152 kernel instruction sites were charged one instruction early.
- The tag compare stays in `psx_icache.c`. Tags follow Beetle (decided, §10),
  and this is the one place a later, oracle-backed move to hardware tags (§2)
  would change.

Where it applies:
- **BIOS emitter:** `relocate_ra(rom)` is uncached for the ROM run in place.
  Done in #429, which closed `bios-kseg1-fetch-charge`.
- **Game and overlay emitter:** the predicate was already there, but
  `emit_pre_icache` tested the compile address, which is always KSEG0. §5.2
  routes two things through `runtime_pc()`: the `psx_fetch_uncached()`
  argument and the fetch tag (done in PR B). Routing only the tag would
  charge a KSEG1 variant at line leaders alone. With both routed, a KSEG1
  variant (§5.4) is charged per instruction with no further emitter change;
  `emitter_runtime_pc_test` checks that a KSEG1 code segment charges exactly
  what a KSEG1 image does. PR D adds those variants and closes
  `kseg1-fetch-charge`; the ledger checks the KSEG1 `probe_run` variant against
  Beetle's fetch model.
- **Interpreter:** unchanged; it already charges every fetch.

BIU bit 11 (cache disable) makes every fetch uncached in Beetle. It is a run-time
state, not a segment, and is listed under §9.

### 5.7 Segment-aware overlay capture and cache keys

The bytes are segment-free; execution is not. So the byte identity (region,
CRC, per-function `code_crc`, pair dedup) stays physical, and the segment
joins only the *entry* identity and the *compiled artifact* key.

**Capture**
- `g_dirty_ram_dispatch_pc_bitmap` keeps its meaning (dispatched in any
  segment), and region building keeps using it.
- Add one sibling bitmap per segment: KUSEG, KSEG0 and KSEG1.
- The siblings are set at the interpreter dispatch that already sets the bit,
  from the full `addr` before `dirty_ram_interp.c:2814` masks it. The cost is
  one bit set per interpreted dispatch, not per instruction. Host memory is
  3 × RAM/32 (192 KiB retail, 768 KiB in 8 MiB mode).
- The execution bitmap needs no segment: direct edges keep the entry's
  segment.

**Capture JSON**
- Schema v3 adds `"dispatch_entry_segments": {"kuseg": [...], "kseg0":
  [...], "kseg1": [...]}`.
- If it is absent, entries are KSEG0 only, so every existing v2 capture stays
  valid.
- The #417 counter `segment_alias_interp` becomes the trigger: a miss records
  its entry into these bitmaps, and the next compile builds the shard.

**Compile (`compile_overlays.py`)**
- One shard per `(region, segment with entries)`.
- `make_psxexe(seg | phys, …)` relies on §5.3's parser.
- `_canonical_guest_addr` and the `| 0x80000000` sites become `seg | phys`.
- The static-overlay generator accepts non-KSEG0 variants: it emits the
  segment in its table and matches exactly, replacing the #417 rejection at
  2535.

**Cache keys** (layout decided 2026-09-29, §10)
- KSEG0 artifacts keep today's path and names, so no existing cache
  invalidates.
- Other segments go in a per-segment subdirectory of the same cache tag:
  `…/cg<N>_<hash>_gc<hash>_f<n>/seg-kuseg/{phys}_{crc}.{dll,so}` (and
  `seg-kseg1/`).
- The filename grammar (`psx_overlay_cache_name_parse`, `8_8` or `8_8_8`)
  does not change.
- Static namespaces add `s<seg >> 29>` for non-KSEG0 shards (`ov_s0_{phys}_…`
  for KUSEG, `ov_s5_…` for KSEG1), so static variants can link side by side.
- Exports are `func_{VA}` (§5.1), so they are distinct per segment.
- `pair_id` hashes the C source, so variants never pair-alias one another.
- A variant shard stamps its own segment's store PCs (§5.2). This relies on
  #420 (ABI v24), which forwards overlay stamps to the host's
  `g_debug_last_store_pc` in every build, so the `memory.c` filters see them.

**Manifest**
- `.ranges` gains `S <segment>`; if it is absent, the segment is KSEG0.
- `F` entries carry the full VA and must match `S`. `parse_manifest` stops
  forcing KSEG0 (942).
- `R` lines stay physical extents: they validate bytes, not execution.

**Loader**
- Candidates keep the physical index and gain a `seg` field.
- The #417 gate generalizes from "PC is KSEG0" to "a candidate for this
  physical address has `seg == pc & 0xE0000000`".
- Other aliases still interpret. Rule 18 permits that for runtime-installed
  code, and it is counted and fed back to capture.
- `overlay_idle_note_is_internal_or_return` (2308) compares full VAs.

**Docs:** update AOT_OVERLAY_PLAN's "canonical KSEG0 entries" contract and
AOT_SHARDING's KSEG0 window. OVERLAY_CACHE_V2's per-function key already
names `guest_entry_vaddr`; this design makes that VA include the segment.

**Expected R4 effect:** the OpenBIOS KUSEG patch slots are captured as
KUSEG entries of the kernel page and get a KUSEG shard. `segment_alias_interp`
drops to 0 on a warm cache.

**Implemented in PR E.**
- **Capture.** `g_dirty_ram_dispatch_seg_bitmap[3]` (`dirty_ram_interp.h`)
  holds one bit per word for KUSEG, KSEG0 and KSEG1. `dirty_ram_dispatch_inner`
  sets the bit for the full PC next to the any-segment bit. A PC in
  `0x20000000`-`0x7FFFFFFF` or KSEG2 maps no RAM in Beetle, so it gets no bit.
  Every site that drops dispatch evidence clears the siblings too
  (`dirty_ram_dispatch_evidence_clear()`): boot reset, a store into an
  executed page, DMA preservation and the autocapture epoch. Snapshot jobs
  copy the siblings with the rest of their evidence.
- **Schema v3** (`overlay_capture.c`). Each region adds
  `"dispatch_entry_segments": {"kuseg": [...], "kseg0": [...], "kseg1":
  [...]}`, each PC spelled in its segment. `dispatch_entry_pcs` keeps its v2
  meaning and spelling (entered in any segment, written at KSEG0), so v2
  readers keep working. `coverage_vault.py` unions the record when it merges
  variants and crops it when it compacts regions. A v2 record's entries are
  KSEG0 entries.
- **Compile** (`compile_overlays.py`). `capture_segment_views()` splits a
  capture into one view per segment with entries. A dispatch entry that no
  segment list names is a KSEG0 entry, and a v2 capture comes back unchanged.
  A view is the capture seen from one segment: `load_addr` and every PC in it
  are spelled in that segment, its dispatch entries are that segment's alone,
  and it carries all the execution evidence (it has no segment). Two kinds of
  record also get a view where they saw no dispatch entry:
  - a record with no dispatch entry in any segment (execution evidence only,
    which the runtime writes) is one KSEG0 view, exactly as a v2 reader reads
    it, so its classification, forced interiors, declared entries and prior
    manifests apply as before;
  - a demand from outside the capture names its segment
    (`declared_view_segments()`): a `--force-interior` PC inside the record
    (KSEG0, since a physical and a KUSEG PC are the same number) asks for the
    KSEG0 view, and a `game.toml` `[[overlays]]` table asks for the segment its
    load address, or else each of its entries inside the record, is spelled in.

  Each view is one ordinary compile, region and fragment pass alike:
  - the image is compiled at `segment | phys`, so the recompiler's §5.3 path
    names and bakes it in that segment;
  - helpers that name PCs without an image address spell them in the view's
    segment (the `image_segment()` context);
  - the shard lands in `segment_cache_dir()`.

  The static generator emits the exact PC in its table: KUSEG, KSEG0 and
  KSEG1 rows of one word coexist, and its segment gate admits the segments it
  compiled. Its isolated-fragment pass compiles each (entry, image) demand
  against its own segment's view of the image, so one entry dispatched in two
  segments gets a fragment in each. `--force-interior` PCs are KSEG0 as
  before, so they force KSEG0 views only. `[[overlays]]` entries in
  `game.toml` match their spelled load address.
- **Config code sites.** The recompiler compiles an overlay view with
  `overlay_codegen_config()` (`segment_variants.cpp`): every exact-match site
  kind that D's `rebase_codegen_config()` moves, and whose physical address
  lies in the image in any segment that maps RAM, moves into the view's
  segment. The config names overlay code by its bytes, and those bytes run in
  any segment. The runtime keys mod function-entry hooks by physical address,
  and the interpreter fires them at every entry, whatever its segment, so a
  KUSEG or KSEG1 shard must emit the same `psx_mod_function_entry` calls as its
  KSEG0 sibling. Otherwise a hook fires on a cold cache and not on a warm one.
  Widescreen, data-shard and persisted-store sites reach every view the same
  way. A KSEG0 view whose sites are spelled at KSEG0, as every pre-§5.7 overlay
  compile's are, compiles exactly as before.
- **Cache keys**, as decided: KSEG0 shards keep their directory and names, and
  KUSEG and KSEG1 shards go in `seg-kuseg/` and `seg-kseg1/` under the same
  cache tag, with the same `{phys}_{crc}` names. The candidate-capacity
  namespace counts the segment directories with their leaf, because the
  runtime's candidate table is shared. Static namespaces are `ov_s0_{phys}_…`
  (KUSEG) and `ov_s5_…` (KSEG1).
- **Manifest.** A KUSEG or KSEG1 manifest starts with `S <segment>` (the base,
  `00000000` or `A0000000`) before its first `F`. `F` entries are full VAs in
  that segment. `R` records stay KSEG0-spelled byte extents. A KSEG0 manifest
  is unchanged, with no `S`. The loader (`parse_manifest`) and the tool
  (`parse_runtime_shard_manifest`) apply the same rule, and both refuse:
  - a manifest whose segment differs from its directory's;
  - an `F` entry outside the manifest's segment;
  - `S` after an `F`, `S` twice, or an `S` that is not a RAM segment base.
- **Loader.** Each candidate records its segment. The physical indexes stay
  (the bytes are one identity) and every lookup matches the PC's segment:
  - the entry chain;
  - CPS range ownership;
  - the lazy manifest index;
  - `try_load_region`;
  - the interpreter's call contract (`overlay_loader_call_native`).

  The negative caches (the lazy-miss and range-owner memos) are keyed by the
  full PC. A PC in a segment that maps no RAM never runs a shard. Exports are
  resolved by their full VA. `segment_alias_interp` now counts the KUSEG and
  KSEG1 dispatches the loader leaves to the interpreter (no shard of their
  segment yet), and the new `segment_native` counts those a shard of their
  own segment runs. Both are in `overlay_loader_status`. A warm cache takes
  the first to 0, except for dispatches at a PC in a segment that maps no RAM
  (`0x20000000`-`0x7FFFFFFF`, KSEG2): those are counted there too and never
  get a shard (none occur in R4 or the probe). The `overlay_candidates` reply
  gains each candidate's `seg` and each lazy manifest's `entry`.
- **Full-VA compares.** `overlay_idle_note_is_internal_or_return` compares the
  resume PC with `$ra` exactly, and treats it as internal only for a running
  shard of its own segment. The shadow diff's two "returned to the caller"
  tests compare the exact PC; these were the last masked compares against a
  call's link (§5.5). The diff mode's kernel-window exclusion now tests the
  physical address, so KSEG0 and KUSEG kernel candidates are treated alike.
  Before, it tested the full PC, so only a KUSEG PC matched, and #417's gate
  never let one reach it.
- Tests:
  - `overlay_segment_keys` (new) runs capture views, manifests, the cache
    layout, the real `compile_overlays.py` driver on a v3 capture, and the
    real loader on the shards it builds. The driver cases also cover
    dispatch-less records and forced interiors against their v2 reading, a
    `--static` isolated fragment per segment of one entry, a KUSEG/KSEG1 view
    compiled into a cache that holds KSEG0 manifests and fragments of the same
    region, and mod function-entry hooks spelled in another segment than the
    view's;
  - `segment_variants_test` (extended) checks `overlay_codegen_config()` in
    every segment's view;
  - `overlay_segment_gate` (extended) covers, in the loader harness:
    per-segment shards, stale bytes, CPS continuations, lazy loads, refused
    manifests (including through live publication), the native-call
    contract and the capture wiring;
  - `overlay_capture_retry_test` (extended) checks the schema v3 record;
  - `coverage_vault` (extended) covers segment unions and crops;
  - the ledger adds the guard `overlay-segment-shards` (§7.1).

## 6. Cost for the static fast path

| | Option A (segment-relative) | Option B (variants), this design |
|---|---|---|
| Home-segment code | +3.8 % `__TEXT`; +27.6 % with exact KSEG1 charging | **0**: byte-identical for KSEG0 titles |
| Per-instruction work | an OR at each baked PC, plus a KSEG1 test at about 80k non-leader sites | none added |
| Dispatch | segment scoping on every dispatch | one `row.addr == addr` compare per dispatch; with no variants, the same row count as today |
| ABI | every generated function and the overlay DLL ABI | unchanged (it relies on #420's v24 store-PC forwarding, which is independent of this design) |
| Extra code | none | the requested variant closures only |
| KSEG1 fetch fix | runtime test everywhere | a per-instruction charge only in KSEG1 bodies: 5,477 added call sites in OpenBIOS ROM code, paid only while ROM code runs |

**How the A numbers were measured**
- R4 shards 00, 17 and 33 (911,454 B `__TEXT`), compiled with the build's own
  command (Apple clang 21, arm64, `-O3`).
- Rewritten in a scratch copy to load the segment once per function and OR it
  into every link, fetch-tag, IRQ-resume, CPS-exit and store-PC constant:
  946,046 B (+3.8 %).
- With `if (seg >= KSEG1) fetch` added before each non-leader instruction:
  1,163,390 B (+27.6 %).
- The first measurement (2026-09-28) left out the 1,261 store-PC stamps in
  these shards, because it treated them as debug-only. Without them the
  figures were 940,382 B (+3.2 %) and 1,159,826 B (+27.2 %).
- This is a size measurement, not a timing one. The per-instruction test
  sits on the hottest path and is only exact if it runs every time.

**Why B's figure is 0**
- It follows by construction, not measurement: a KSEG0-linked title (R4
  included) is emitted with `code_seg = KSEG0`, which is today's output. PR B
  (§8) proves it by byte-identical regeneration.

**What B adds at run time**
- One compare per dispatch.
- The capture bitmaps (§5.7).
- The recovered native coverage: the ~27 per-frame interpreted KUSEG alias
  dispatches in R4 become native.

## 7. Validation against Beetle

### 7.1 Acceptance ledger (this PR)

`test_segment_aware_codegen.py` builds the synthetic KUSEG-linked EXE and runs
the real `psxrecomp-game` and `psxrecomp-bios` on it (OpenBIOS for the latter,
0.3 s). It checks three things:
- **Emitted PC constants** in the bodies that dispatch picks for link-segment
  PCs.
- **Dispatch behaviour**, using the emitted lookup compiled and queried:
  - home PCs;
  - requested variants;
  - unrequested aliases, which must miss.
- **Fetch cycles.** The emitted fetch sequence runs through the real
  `runtime/src/psx_icache.c`. The result must equal the per-instruction
  sequence Beetle executes, computed by a Python transcription of
  `ReadInstruction`.

**Model checks (must always pass)**
- `psx_icache.c` equals the Beetle transcription on:
  - cold then warm cached runs;
  - a KSEG0 → KUSEG alias refill;
  - repeated KSEG1;
  - partial refill.
- The leader rule is exact for the cached probe block.
- A mutation of the model to hardware bit-31 tags, or to a +5 uncached cost,
  fails.

**Gap ledger.** `KNOWN_GAPS` in the test lists each id with the section that
closes it. The test passes only when the observed gaps equal the ledger. Ten
were open when the ledger landed; PR C closed eight and PR D the last two, so
`KNOWN_GAPS` is empty:

| id | observed before the closing PR | closed by | state |
|---|---|---|---|
| `link-segment` | 7 link constants, e.g. `0x80010018` | §5.3 | closed (C) |
| `fetch-tag-segment` | 30 fetch tags in KSEG0 | §5.3 | closed (C) |
| `irq-resume-segment` | 4 resume PCs in KSEG0 | §5.3 | closed (C) |
| `resume-pc-segment` | 10 exit PCs / continuation keys in KSEG0 | §5.3 | closed (C) |
| `store-pc-segment` | 10 store-PC stamps in KSEG0, e.g. `0x8001000C` | §5.3 | closed (C) |
| `home-seed-accepted` | KUSEG seed loaded 0 of 1 | §5.3 | closed (C) |
| `alias-fetch-coherence` | interp-then-compiled `leaf`: 22 cycles vs Beetle 11 (the #417 shape) | §5.3 | closed (C) |
| `segment-variants` | `0xA00100F0` resolved to the `0x800100F0` body; after C it missed | §5.4 | closed (D) |
| `segment-miss` | unrequested KSEG0/KSEG1 aliases resolve; KUSEG PCs resolve to KSEG0 rows | §5.5 | closed (C) |
| `kseg1-fetch-charge` | no KSEG1 body (Beetle: 40 cycles for the 10-instruction run) | §5.4 + §5.6 | closed (D) |

Each implementation PR removes the ids it closes. PR A (#429, §8) was the
exception: it landed on master before this test did. This PR dropped its id,
`bios-kseg1-fetch-charge`, when it was rebased onto it. The BIOS check stays
as a regression guard: an OpenBIOS KSEG1 instruction without its own fetch
reports that id again, as a new gap.

PR B closes no id. It adds two more regression guards, which are
`REGRESSION_GUARDS` in the test:
- `bios-runtime-pc` (§5.2): no store-PC stamp, syscall EPC, break or
  unaligned-access PC, or fallthrough PC in the OpenBIOS output names a ROM
  address inside a relocated window. Before B, 2,295 distinct PCs did (3,328
  PC and site-class pairs, since a `sh`/`sw` carries both a stamp and an
  unaligned-access PC), all in the Kernel ramtext and Shell windows. The
  output only shows the translations OpenBIOS exercises, and most
  orphaned-delay-slot paths never inline a PC-bearing instruction there, so
  the guard also requires every `StrictTranslator::translate()` call in
  `full_function_emitter.cpp` to pass `relocate_ra()`.
- `store-pc-keys-runtime` (§9): no `memory.c` store-PC key lies inside one of
  SCPH1001.toml's relocated ROM windows. A key for relocated code must be the
  runtime PC of its ROM store, computed from the profile's windows as
  `BiosAddressModel::runtime_pc()` does, and must go through the gated
  `scph1001_relocated_store()`; a bare RAM key would also match other BIOSes
  and game code.

PR C removes its eight ids from `KNOWN_GAPS` and lists them in
`REGRESSION_GUARDS`, so each reports a new gap if it breaks again.
`resume-pc-segment` now also checks every dispatch row's key and resume PC.

PR E closes no id (§5.7 had none in the ledger) and adds the guard
`overlay-segment-shards`, described below; `KNOWN_GAPS` stays empty.

PR D moves `segment-variants` and `kseg1-fetch-charge` to `REGRESSION_GUARDS`.
With variants in the table, `resume-pc-segment` requires each row's key and
resume PC to be in the segment of the body it enters, and `segment-variants`
also requires every PC a variant body bakes (links, fetch tags, IRQ resume
PCs, exits and continuation keys, store PCs) and its name to be in its
segment. D adds the guard `exact-return-contract` (§5.5): the test compiles
`psx_call_contract` and checks that another segment's alias of the call site
starts a bail and resolves none, and that the emitted OpenBIOS dispatch loop
has its four exact return checks and no masked one.

The overlay half (§5.7) gets its own acceptance case in
`runtime/tests/test_overlay_segment_gate.py`:
- a KUSEG-compiled fixture shard runs natively for its KUSEG PC;
- the same shard is interpreted for the KSEG0 and KSEG1 aliases.

PR E implements it there, loaded lazily from `seg-kuseg/`, entry and CPS
continuation alike, together with a shard per segment for one word and the
manifests the loader must refuse. `recompiler/tests/test_overlay_segment_keys.py`
runs the same property end to end: the real `compile_overlays.py` builds KUSEG
and KSEG1 shards from a v3 capture, and the real loader runs them. Each run
links, stamps and fetches in its segment, and costs what Beetle's fetch model
charges. PR E also adds the ledger guard `overlay-segment-shards`. It compiles
the probe's disc-loaded `ov_run` (§7.2) from a v3 capture, one view per
segment, with `compile_overlays.py`'s own helpers and the real recompiler. It
requires every PC a shard bakes and its names to be in its segment, the `S`
record, and `ov_run`'s straight run to cost Beetle's cycles: line leaders
cached at KUSEG and KSEG0, +4 per fetch at KSEG1.

### 7.2 Oracle runs (implementation PRs)

**The synthetic EXE is also an oracle probe.** It writes into a results block
at `0x00011000`, outside the image:
- the three link values;
- the executing-segment link from each of the three `probe_run` entries;
- a Timer 2 delta around each of those entries.

Since PR E it then loads an overlay (§5.7): it leaves the critical section,
reads its own EXE file back from the disc through the BIOS file calls (B0:32
open, B0:34 read, B0:36 close; the kernel DMAs the sectors) into RAM at
`0x000A0000`, and calls the copy of the position-independent `ov_run` there
through KUSEG, KSEG0 and KSEG1. It records each run's T2 delta and
`ov_getpc` link at `R+0x28`-`R+0x3C`, and the file descriptor and byte count
at `R+0x40`/`R+0x44`. The overlay phase replaces the old `j .` with a `j` of
the same size, so every earlier label keeps its address.

It then spins. Procedure:
- Boot it from a `tools/cycle_testrom`-style disc (SYSTEM.CNF plus the EXE).
  Do not sideload it: Beetle's EXE loader forces a KSEG0 start address, so a
  sideloaded run executes at `0x8001…` and every link comes back KSEG0. Under
  OpenBIOS a sideloaded run never reaches the program at all.
- Read the block from Beetle, from the interpreter (`PSX_FORCE_INTERP=1`) and
  from the compiled build, and compare.
- Expected values: links `0x00010018`, `0x00010024`, `0x00010038`; segment
  probes `0x000100FC`, `0x800100FC` and `0xA00100FC`. The KUSEG and KSEG0 T2
  deltas are equal, and the KSEG1 delta exceeds them by the uncached fetch
  surcharge: 26 cycles for the 15 KSEG1 fetches (Beetle's fetch model: 60
  uncached against 34 cold cached).

**Beetle results (2026-09-29).** psx-beetle built on macOS
(`docs/beetle-macos.md`, from #431), disc boot, OpenBIOS and SCPH-1001:
- The first run found a probe bug: the T2 subtraction sat in the second
  `lw`'s load delay slot and read the stale register, and the difference was
  not masked to T2's 16 bits. Every delta was unusable. The generator now
  waits one `nop` and masks with `andi 0xFFFF`. It also starts each call
  sequence and `probe_run` on their own I-cache lines; before that, alignment
  alone made the deltas read 59/59/89.
- Links and segment probes match the expected values above on both BIOSes.
- T2 deltas: 56 / 56 / 82 cycles for KUSEG / KSEG0 / KSEG1 on both BIOSes.
  The KSEG1 surcharge is 26, exactly the fetch model's.
- A cycle watch on the first layout, before these fixes, timed the pieces
  directly: 10 / 10 / 15 cycles from `probe_run` entry to `getpc`, and 8 / 8 /
  10 from `getpc` back.

**Interpreter and compiled columns (PR B, 2026-09-29).** Both come from a probe
runtime built like `tools/cycle_testrom`'s: disc boot, LLE, with `seeded`
seeded as `0x800100E4`. The interpreter column needed a tooling fix:
`PSX_FORCE_INTERP=1` had stopped affecting clean game text. It only marks
pages dirty, and the native-safety checks now decide by the bytes, so B
restores it.

| | links | segment probes | T2 KUSEG / KSEG0 / KSEG1 |
|---|---|---|---|
| Beetle | `0x00010018/24/38` | `0x000100FC`, `0x800100FC`, `0xA00100FC` | 56 / 56 / 82 |
| interpreter | same | same | 56 / 56 / 82 |
| compiled (B = base) | `0x80010018/24/38` | `0x800100FC` × 3 | 56 / 20 / 20 |

- These results are the same on OpenBIOS and SCPH-1001.
- A cycle watch shows the same split. From one `probe_run` entry to the next,
  Beetle and the interpreter take 90 and 90 cycles, and compiled code takes
  90 and 54. From `probe_run` to `getpc`, Beetle and the interpreter take
  10 / 10 / 15 and compiled code takes 10 / 1 / 1.
- The compiled column is the expected set of gaps:
  - links in KSEG0 (§3.2, closed by C);
  - fetch tags in KSEG0, so the two alias runs hit the lines the home run
    filled, where Beetle refills or runs uncached (`fetch-tag-segment`,
    `alias-fetch-coherence`, C);
  - no KSEG1 body (D).
- The compiled probe output is byte-identical before and after B.

**Compiled column after PR C (2026-09-29).** The probe runtime is rebuilt
with the EXE compiled at KUSEG (`game.toml` load and entry `0x00010000`, and
`seeded` seeded as `0x000100E4`, the spelling B could not use). The KSEG0 and
KSEG1 runs of `probe_run` have no body of their own, so they are segment
misses and run interpreted.

| | links | segment probes | T2 KUSEG / KSEG0 / KSEG1 |
|---|---|---|---|
| Beetle | `0x00010018/24/38` | `0x000100FC`, `0x800100FC`, `0xA00100FC` | 56 / 56 / 82 |
| interpreter | same | same | 56 / 56 / 82 |
| compiled (C) | same | same | 56 / 56 / 82 |

- The same on OpenBIOS and SCPH-1001.
- Cycle watch: from one `probe_run` entry to the next, all three take 90 and
  90 cycles (compiled took 90 and 54 before C); from `probe_run` to `getpc`,
  all three take 10 / 10 / 15 (compiled took 10 / 1 / 1).
- The compiled build and the interpreter now agree to the cycle for the
  whole run: the spin is reached at 198,522,602 cycles on OpenBIOS and
  398,724,722 on SCPH-1001 in both (B's compiled build: 198,522,504 and
  398,724,624, 98 fewer).
- The segment-miss ring at the spin holds six entries on both BIOSes, each
  once: `probe_run` (`0x800100F0`, `0xA00100F0`), its `getpc` callee
  (`0x80010124`, `0xA0010124`) and the continuation after that call
  (`0x800100FC`, `0xA00100FC`), each with its KUSEG row as `home`.
- Apart from segments, the compiled probe C equals B's. The dispatch file
  differs only in the lookup (§5.5).

**Compiled column after PR D (2026-09-29).** The probe runtime is rebuilt
with the variant seeds `0x800100F0` and `0xA00100F0` added to C's seeds. They
compile `probe_run` in KSEG0 and KSEG1 with its `getpc` callee and the call's
continuation (6 variant rows), so every run of `probe_run` is native.

| | links | segment probes | T2 KUSEG / KSEG0 / KSEG1 |
|---|---|---|---|
| Beetle | `0x00010018/24/38` | `0x000100FC`, `0x800100FC`, `0xA00100FC` | 56 / 56 / 82 |
| interpreter | same | same | 56 / 56 / 82 |
| compiled (D) | same | same | 56 / 56 / 82 |

- The same on OpenBIOS and SCPH-1001.
- Cycle watch: from one `probe_run` entry to the next, all three take 90 and
  90 cycles; from `probe_run` to `getpc`, all three take 10 / 10 / 15.
- The compiled build and the interpreter reach the spin on the same cycle as
  in C (198,522,602 on OpenBIOS, 398,724,722 on SCPH-1001).
- The segment-miss ring at the spin is empty on both BIOSes (C: six entries),
  with 0 dispatch misses.

**LLE boot after PR D (2026-09-29).** Native − Beetle at each hit, the same
anchors as #429's gate:
- OpenBIOS: unchanged from C (FLUSH ×5 0 → −126, A0 ×12, shell entry −126).
- SCPH-1001: the first hit of every anchor is 0 (FLUSH, A0, B0 and C0; it was
  −9 in A, B and C), and every later hit moves by the same +9: the shell
  entry is −445 (was −454). That is the IsC gap alone (−52, −149, −149 and −95
  at SCPH-1001's four boot flushes, §9).
- With #435 (IsC) merged locally on top of D: both BIOSes are at 0 at every hit
  of every anchor up to and including the shell entry (OpenBIOS FLUSH ×5,
  A0 ×12, shell; SCPH-1001 FLUSH ×5, A0 ×17, B0 ×147, C0 ×7, shell).
- After the shell entry SCPH-1001 still differs, as it did before D: the C0
  hit 1,460 cycles after the shell entry is −836 with #435 (−864 relative to
  the shell entry without it), and a FLUSH about 14 s after power-on is
  −66,558,224.
  These lie outside this gate and are not caused by the segment work.

**After PR D's review fixes (2026-09-30).** The BIOS dispatch now picks the
body through `psx_bios_hit_body()` and records BIOS segment misses (§5.4);
the variant rules refuse seeds outside the physical segments. Rerun on the
rebuilt probe runtime:
- Every LLE anchor hit above is identical to D before the fixes, on both
  BIOSes, with and without #435 on top.
- The probe's results, spin cycles and cycle-watch intervals are identical.
- The segment-miss ring at the probe's spin is empty on both BIOSes: neither
  boot enters a compiled BIOS window through another segment's alias, apart
  from SCPH-1001's `0xA0000500`, which has its variant.
- The same build with SCPH-1001's `0xA0000500` seed removed records exactly
  one BIOS segment miss, `0xA0000500` (home `0x00000500`, `$ra`
  `0xBFC0683C`, frame 0), and its shell entry returns to −454.

**Overlay phase after PR E (2026-09-30).** The probe runtime is rebuilt with
`overlay_cache = true` in its `game.toml`. At the spin, the cold run's capture
(schema v3) holds one region, `0x800A0000`, with its dispatch entry
`0x000A0A20` in all three segments. `compile_overlays.py` builds one shard per
segment from it: `000A0000_*` at KSEG0, `seg-kuseg/`, `seg-kseg1/`. Each is an
isolated fragment, because `ov_run` has no callable boundary.

| | links (`ov_getpc`) | T2 KUSEG / KSEG0 / KSEG1 |
|---|---|---|
| Beetle | `0x000A0A2C`, `0x800A0A2C`, `0xA00A0A2C` | 56 / 56 / 82 |
| interpreter (`PSX_FORCE_INTERP=1`, overlay native off) | same | 56 / 56 / 82 |
| compiled, cold cache (overlay interpreted) | same | 56 / 56 / 82 |
| compiled, warm cache (a shard per segment) | same | 56 / 56 / 82 |

- The same on OpenBIOS and SCPH-1001. The fd is 2, and 0x1000 bytes are read,
  on every backend.
- Cycle watch at the copy's `ov_run` and `ov_getpc`: from one `ov_run` entry to
  the next, every backend takes 90 and 90 cycles; from `ov_run` to `ov_getpc`,
  10 / 10 / 15. The static `probe_run` watches equal D's on every backend,
  Beetle included (first hit 198,522,323 native and 224,958,018 Beetle on
  OpenBIOS; 398,724,443 and 496,646,350 on SCPH-1001).
- The three native modes reach the spin on the same cycle: 205,367,484 on
  OpenBIOS and 400,644,561 on SCPH-1001.
- `overlay_loader_status` at the spin: cold `segment_alias_interp` 2 and
  `segment_native` 0 (the KUSEG and KSEG1 entries interpreted); warm 0 and 6
  (the KUSEG and KSEG1 runs native, each through its `ov_getpc` call and
  continuation). There are 0 dispatch misses and 0 segment misses.

**LLE boot after PR E (2026-09-30).** Every anchor hit on both BIOSes equals
D's (FLUSH, A0, B0, C0 and the shell entry, whose native − Beetle stays −126 on
OpenBIOS and −445 on SCPH-1001). This is with the overlay cache enabled, which
the D probe runtime did not have.

**R4 after PR E (2026-09-30).** A cold boot-to-race smoke run with the
runtime's own autocapture and autocompile produced two non-KSEG0 shards:
- `seg-kuseg/00002000_82812130`: OpenBIOS's patch slots `0x0000281C`,
  `0x00003554` and `0x0000357C`;
- `seg-kseg1/0000D000_E54B69C3`: kernel RAM code entered uncached at
  `0xA000DFAC`, `0xA000DFC0` and `0xA000DFD4`.

Neither region was entered at KSEG0 in that run, so it has no KSEG0 shard. The
KSEG0 shards earlier compilers built from these regions' KSEG0-spelled entries
did not run in these runs either: #417's gate kept the KUSEG and KSEG1 entries
off them.
Counters:
- during that run, before and after the shards arrived:
  `segment_alias_interp` 101,373, `segment_native` 21,355;
- in a warm run: `segment_alias_interp` 0 and `segment_native` 66,508 (at
  frame 8,857), with 0 dispatch misses and 0 segment misses. The native count
  grows with the frame the smoke script reaches in wall time: earlier warm
  runs counted 66,309 and 66,344 (frame 8,792).

Every baked PC in both shards is in its segment, and the KSEG1 shard charges a
fetch before each of its instructions.

**Other runs**
- **§5.6 BIOS fix (PR A, #429): done, passed.** LLE boot (`bios_hle = false`)
  cycle parity against live Beetle to the shell, OpenBIOS and the owner's
  SCPH-1001 dump, the same image on both sides. Native − Beetle at the shell
  entry moves from −2,165,591 to −126 cycles (OpenBIOS) and from −7,049,462
  to −454 (SCPH-1001). The residue comes from two known gaps that #429 does
  not cause or widen: IsC stores that do not invalidate the I-cache model (§9)
  and SCPH-1001's KSEG1 kernel entry (§3.3). Ruler #1 (SCPH-1001) matches
  pass for pass on all 64 passes, and ruler #2 matches on all 14 components.
- **KUSEG titles:** regenerate Kula World and Alien Resurrection and smoke
  them. Use a warm/cold A/B with the #417 procedure (seeded,
  `PSX_OVERLAY_AUTOCOMPILE_OFF=1`, compare cyc/mc/sp plus an order-independent
  write sum).
  - **Kula World (SCES-01000): done, passed** (#671, `bce31aff`, SCPH-1001,
    971 functions). Attract demo without input to frame 10,000, cold cache
    against warm cache (5 shards: 4 in `seg-kuseg/`, 1 in `seg-kseg1/`):
    every judge column and both locators are identical at every frame.
    `segment_alias_interp` 172,863 → 733, `segment_native` 0 → 59,524,
    0 dispatch misses, 0 segment misses. Both demos end with Beetle's timer
    and score (3603 / 9950, 3017 / 6845). Memory card and the ending FMV
    run too (macOS arm64).
  - Alien Resurrection: open.
- **KSEG0 titles:** byte-identical regeneration (§5.2).

## 8. Rollout

Small PRs in this order. Each PR updates the ledger and FAITHFUL_TIMING_PLAN's
log.

- **A: `fix/uncached-fetch-per-insn`** (§5.6). Merged on 2026-09-29 as
  #429. Both emitters charge `psx_icache_fetch` before every instruction whose
  runtime PC is uncached, and the BIOS emitter's cache-line start test uses
  the runtime PC. Closed `bios-kseg1-fetch-charge`; this PR dropped the id
  when it was rebased onto A (§7.1). Beetle parity (LLE boot to the shell,
  §7.2): native − Beetle at the shell entry is −126 on OpenBIOS and −454 on
  SCPH-1001, down from −2,165,591 and −7,049,462. The residuals are the IsC
  gap (§9) and SCPH-1001's KSEG1 kernel entry (§3.3). The oracle recipe for
  macOS is `docs/beetle-macos.md` (#431).
- **B: `refactor/emitter-runtime-pc`** (§5.2). No behaviour change for game
  code, including the store-PC stamps; proven by byte-identical regeneration
  of KSEG0 titles. The BIOS-emitter holes in §5.2 do change BIOS output. The
  store-PC one ships with the re-key of the seven `memory.c` keys: six
  store-filter keys and the GP0 source key (§9). Implemented 2026-09-29:
  - R4 regenerates byte-identically: 53 files, including the dispatch table,
    and all 26 overlay shard sources compiled from a race capture.
  - Every changed BIOS line is a `runtime_pc()` move: 2,334 lines in OpenBIOS
    and 12,501 in SCPH-1001. The dispatch tables are unchanged.
  - LLE boot against Beetle matches the previous build at every anchor:
    shell entry −126 on OpenBIOS and −454 on SCPH-1001, as in A's gate.
  - The probe's total cycles to its spin are unchanged on both BIOSes.
  - R4 fingerprints (12000 frames, cold and warm) agree on every column
    that does not hash store PCs. `mmio` (a judge column whose ordered hash
    includes the store PC) and the `pc` locator first differ at fingerprint
    frame 4 and stay different, as rolling hashes do. An ordered recording
    of `PSX_RECORD_FRAME=3` (12,238 entries) holds the whole difference: 102
    store PCs moved from ROM to runtime in OpenBIOS's Kernel ramtext window.
    `PSX_RECORD_FRAME=4` (14,474 entries) is identical.
  - In an SCPH-1001 LLE boot, the GP0 source key fires for the same 5,596
    commands in both builds.
- **C: `feat/kuseg-linked-exe`** (§5.3, §5.5). Also rewrites the alias
  assertions in `test_kuseg_dispatch_lookup.py`: an alias with no body now
  misses. Closes eight ids: §5.3's seven and `segment-miss` (§5.5).
  Implemented 2026-09-29:
  - The EXE parser keeps the header's segment and the image compiles there;
    dispatch is exact; segment misses are interpreted and recorded (ring,
    TCP `segment_misses`, `dispatch_stats`, exit report). Seeds and config
    sites are checked by physical address and segment (§5.3).
  - The ledger is at two gaps (`segment-variants`, `kseg1-fetch-charge`).
  - Probe: compiled equals Beetle and the interpreter in every result word
    and every cycle-watch interval, on both BIOSes (§7.2).
  - LLE boot against Beetle is identical to B at every hit of every anchor
    (shell entry −126 OpenBIOS, −454 SCPH-1001). BIOS C is byte-identical.
  - R4 (KSEG0): the 50 game shards, the declarations header and the
    `.ranges` manifest are byte-identical; `SLUS_007.97_dispatch.c` changes
    only in `psx_game_find_entry` (the exact-PC compare and its comment).
    All 26 overlay shard sources compiled from B's race captures are
    byte-identical. Fingerprints (12000 frames, cold and warm) are identical
    to B on every column, `mmio` and the `pc`/`wr` locators included. A smoke
    run reaches a live race with 0 dispatch misses and 0 segment misses.
  - The codegen hash changes, so every title's overlay cache recompiles once
    and the runtime refuses every savestate, rewind buffer and boot-state
    cache made before C (the hash is in the state header). A KUSEG title
    whose `game.toml` omits `entry_pc` now gets the header's KUSEG entry,
    which also keys its save slots.
- **D: `feat/segment-variants`** (§5.4). Closes `segment-variants` and
  `kseg1-fetch-charge`. Since #429, D only needs the KSEG1 variants: with B's
  `runtime_pc()`, #429's per-instruction rule charges them (§5.6), and
  `kseg1-fetch-charge` stays open until D lands. Also gives the BIOS emitter a
  KSEG1 variant of SCPH-1001's kernel entry `0xA0000500`, which closes the
  −9 cycles left at that entry in #429's gate (§3.3), and makes the five
  segment-masked return checks exact together: `psx_call_contract` and the
  four in the generated BIOS dispatch loop (§5.5). Implemented 2026-09-29:
  - Segment-qualified seeds compile the direct-edge closure per segment
    through a segment view of the image; dispatch keeps a row per compiled
    segment of a word and looks up the exact PC; jump-table cases in another
    segment than their body are dispatched, in every game and overlay compile
    (§5.4, §5.5). BIOS seeds for a copy window's alias compile BIOS variants
    with exact-PC rows, and a BIOS window entered through another segment's
    alias with no variant is recorded as a BIOS segment miss (§5.4). Seeds in
    a segment that does not map physical memory stop the build.
  - The ledger is at zero gaps; new guard `exact-return-contract`. New tests
    `segment_variants_test` and `bios_segment_variants`; the latter compiles
    the emitted BIOS hit lookup, with and without variants, and queries every
    variant row, every home key's home PC and every other segment's alias of
    it. Two single-site mutation sweeps kill every mutant: 21 sites in D
    (planner, dispatch lookup, jump-table filter, BIOS variant emission, the
    five checks) and 21 sites of the review fixes (seed refusal, alias-group
    declarations, the data-stub row filter, the home jump-table filter, the
    BIOS branch closure and same-segment seeds, the BIOS hit lookup and its
    recording, the key home PCs, the ring's kind and the seed tool's filters).
  - Probe: every run of `probe_run` is native and equals Beetle and the
    interpreter; the segment-miss ring is empty (§7.2).
  - LLE boot: SCPH-1001's first hits are at 0 and its shell entry at −445;
    with #435 both BIOSes are at 0 through the shell entry (§7.2). Neither
    boot records a BIOS segment miss.
  - R4 (KSEG0): the 50 game shards, the declarations header, the `.ranges`
    manifest and the dispatch table are byte-identical to C, and so are all 26
    overlay shard sources compiled from C's race captures. OpenBIOS's full C is
    unchanged; its dispatch C changes in the four exact return checks and
    gains `psx_bios_key_home_pc()` and `psx_bios_hit_body()`. Fingerprints
    (12000 frames, cold and warm) are identical to C on every column, `mmio`
    and the `pc`/`wr` locators included, and an A/A pair is identical;
    against R4 master's pin they show the same split as B and C (#429). A
    smoke run reaches a live race with 0 dispatch misses and 0 segment misses
    of either kind.
  - The codegen hash changes (`eea16175` → `bd3a72ac`: `code_generator.*`,
    `full_function_emitter.*`, `ps1_exe_parser.h` and `cpu_state.h` are
    hashed), so every title's overlay cache recompiles once and pre-D
    savestates, rewind buffers and boot-state caches are refused. SCPH-1001's
    generated C gains the variant and its table. Titles without variant seeds
    get the same game C, except where a table holds another segment's PCs of
    the image's bytes (§5.4); the same applies to overlay shards.
- **E: `feat/overlay-segment-keys`** (§5.7). Includes the runtime acceptance
  case, and the R4 warm-cache check that `segment_alias_interp` reaches 0.
  It also returns a KUSEG-linked title's overlay code to native shards, which
  C sends to the interpreter (§5.3). Implemented 2026-09-30:
  - Capture records each interpreted dispatch's segment (schema v3,
    `dispatch_entry_segments`). `compile_overlays.py` compiles one shard per
    (region, segment with entries), with KUSEG and KSEG1 shards in
    `seg-kuseg/` and `seg-kseg1/` and `S` in their manifests. The loader runs
    a shard only for PCs of its segment (§5.7). The ledger stays at zero gaps
    and gains the guard `overlay-segment-shards`.
  - Tests: the new `overlay_segment_keys`, and extended versions of
    `overlay_segment_gate` (per-segment shards, stale bytes, CPS
    continuations, lazy loads, refused manifests, the native-call contract,
    capture wiring), `overlay_capture_retry_test`, `coverage_vault`,
    `compile_overlays_static_split`, `overlay_init_guard` and
    `overlay_retry_c11`. A single-site mutation sweep of 47 sites kills 42.
    Of the five survivors, three are equivalent (a validated memo key, the
    early reject of unmapped segments, and the region pass's segment context,
    whose canonical PCs are only compared with each other). The other two sit
    in the opt-in idle-skip note and the opt-in shadow diff, which no unit
    test drives.
  - Review fixes (2026-09-30): the static isolated-fragment pass keys its
    images by segment; a dispatch-less record, a forced interior and a
    declared `[[overlays]]` table keep their view; overlay views move config
    code sites into their segment (`overlay_codegen_config()`); two
    segment-routing sites that no test reached (the empty-primary
    reconciliation's and the prior-manifest merge's cache directory) are now
    tested. A second sweep of 17 single-site mutants of these fixes kills all
    17. After them, the probe's shards and R4's game C, OpenBIOS C and every
    shard source and manifest compiled from D's and E's captures are
    byte-identical; a cold R4 smoke run's autocompile builds the same KUSEG and
    KSEG1 shards, and R4 fingerprints (12000 frames, cold and warm) are
    IDENTICAL to E's, locators included.
  - Probe: the new overlay phase equals Beetle and the interpreter in every
    result word and cycle-watch interval, cold and warm, on both BIOSes; warm
    runs its KUSEG and KSEG1 entries natively (§7.2). LLE boot anchors equal
    D's at every hit.
  - R4 (KSEG0): the 53 game C files, OpenBIOS's C and all 52 shard sources and
    manifests compiled from D's race captures are byte-identical to D. The
    codegen hash is unchanged (`bd3a72ac`); no hashed file changes.
    Fingerprints (12000 frames):
    - cold, and warm with D's cache, are IDENTICAL to D, locators included;
    - warm with the new KUSEG and KSEG1 kernel shards is IDENTICAL to warm
      without them, locators included, and to cold with the 501 VBlank
      straddles any warm-vs-cold pair shows;
    - against R4 master's pin, the same #429 split as B, C and D.

    Smoke runs reach a live race with 0 dispatch misses and 0 segment misses;
    warm, `segment_alias_interp` is 0 (§7.2).
  - No existing cache is invalidated, and savestates are unaffected: the
    codegen hash, the ABI and the KSEG0 layout are unchanged, and the capture
    bitmaps are host-only.

## 9. Adjacent gaps (not in scope, recorded)

- **BIU bit 11 does not reach the fetch model.** `memory.c` stores
  `0xFFFE0130` but `psx_icache.c` ignores it. Beetle charges +4 for every
  fetch while the cache is disabled (`CPU_SetBIU`, 484-505). This matters
  only for RAM code run with the cache off.
- **IsC stores are dropped** (`memory.c:1768`). Beetle writes the I-cache tag
  and valid bits through them (`WriteMemory_IsC_misc`, 623-641), so a
  `FlushCache` in psxrecomp does not invalidate the I-cache model. #429's
  Beetle gate measured it: native − Beetle drops by 42 cycles at each of
  OpenBIOS's three boot flushes (−126 at the shell), and by 52 / 149 / 149 /
  95 at SCPH-1001's four (−445, plus the −9 of §3.3, gives −454). Native keeps
  hitting lines that Beetle has invalidated. A local prototype that
  invalidates the model on IsC stores brings OpenBIOS to 0 through the shell
  and SCPH-1001 to −9 (the §3.3 kernel entry). It is its own small PR, #435.
  With PR D's kernel-entry variant and #435 together, both BIOSes are at 0
  through the shell entry (§7.2).
- **BIOS store-PC keys are ROM addresses.** The `strict_translator` stamp is
  the ROM address; the interpreter stamps the runtime PC. Seven `memory.c`
  keys lie in SCPH1001's relocated windows (`bios/SCPH1001.toml`). The
  runtime PC shown for each is what `BiosAddressModel::runtime_pc()` returns:
  - Kernel Part 2, which runs at `0x500`: the RAM filter key `0xBFC10A00`
    (`0x00000F00`) and the Tomba key `0xBFC117E4` (`0x00001CE4`);
  - Shell, which runs at `0x80030000`: the RAM filter keys `0xBFC3EEB4`
    (`0x80056EB4`), `0xBFC405E4` (`0x800585E4`), `0xBFC40788`
    (`0x80058788`) and `0xBFC41C50` (`0x80059C50`), and the GP0 source key
    `0xBFC38B1C` (`0x80050B1C`, `memory.c`'s GP0 write path).

  For these keys a filter fires, or the GP0 write gets a RAM source key,
  only while that code runs compiled. If the same code runs interpreted, the
  store lands and the GP0 source stays the register address. Routing the
  BIOS stamp through `runtime_pc()` (§5.2) without re-keying would silently
  disable all seven. Re-keying them to runtime PCs in the same change also
  makes the interpreted path agree.

  **Re-keyed in PR B.** The SCPH-1001 output stamps each of the seven runtime
  PCs exactly once, where it stamped the ROM key before; the four in-place ROM
  keys (`0xBFC04E90`, `0xBFC04EF0`, `0xBFC05164`, `0xBFC0D634`) do not move.
  The GP0 key is live during boot: frames 1-600 of an SCPH-1001 LLE boot send
  46,850 GP0 commands, 5,596 of them keyed, identically before and after.

  A RAM PC alone is ambiguous: another BIOS, or game code once the region is
  reused, can run a store at the same address, where the old ROM key could
  only match the compiled SCPH-1001 store. So each of the seven keys goes
  through `scph1001_relocated_store(pc, rom)`, which matches only while the
  active image is SCPH-1001 (by CRC) and the RAM word at `pc` is still the
  ROM word it was copied from. That keeps each key to the one instruction it
  named before:
  - OpenBIOS, SCPH-101 and SCPH-5552 never match. SCPH-101 and SCPH-5552
    have no address model, so their kernel (byte-identical to SCPH-1001's)
    and their shells run interpreted and stamp runtime PCs; an ungated RAM
    key would have started firing on them.
  - Game code at those addresses never matches, on any BIOS.
  - The one widening left is intended: SCPH-1001's own instruction now also
    matches when it runs interpreted, as it already did compiled.

  The filters themselves diverge from Beetle; PR B keeps them as they were.
  At the segment probe's spin on SCPH-1001 (disc boot, LLE), Beetle's RAM
  `0x0`-`0xB` holds `00000003 275A0C80 03400008`: the delay-loop scratch word
  and the second and third words of the exception-vector stub. Native holds
  zeros, because game start clears the low scratch
  (`memory_clear_low_boot_scratch`) and the filters drop the later stores:
  Beetle's write log shows the in-place ROM store `0xBFC0D634` writing word 0
  after game start. That is ACCURACY_BURNDOWN axis 4 ("RAM 0x0-0xF boot
  scratch"), to be decided with oracle evidence on its own.
- **Syscall EPC in compiled code** comes from `cpu->pc` (`traps.c:1040`).
  The BIOS emitter sets it; since PR B it sets the runtime PC. The game
  emitter does not set it.
  - Confirmed in PR B: under CPS a compiled game body runs with `cpu->pc ==
    0`, because the entry switch consumes it.
  - The game emitter also ignores `psx_syscall`'s transfer result. The BIOS
    emitter returns on it.
  - A game syscall that reaches the BIOS exception handler (anything but
    Enter/ExitCriticalSection, which `psx_syscall` handles directly) would
    therefore record EPC 0 and continue inline.
  - R4 has two syscall sites, both Enter/ExitCriticalSection.
  - The fix is to emit `cpu->pc = <runtime_pc>; if (psx_syscall(...))
    return;`, as the BIOS emitter does. That changes the generated code of
    every title with a syscall site, so it is its own oracle-backed change,
    not part of B's byte-identical refactor.

## 10. Decisions (2026-09-29)

The owner delegated the four open questions and accepted this document's
recommendations. They are settled; implementation PRs follow them.

1. **I-cache tags follow Beetle, the oracle.** Tags compare the full virtual
   address. Hardware clears bit 31 of KSEG0 fetches first; that difference
   is recorded in ACCURACY_BURNDOWN axis 4 (§2). A move to hardware tags
   would be a separate, oracle-backed change.
2. **Segment misses in static game code interpret loudly until the title is
   regenerated.** They are counted, recorded in the TCP-visible ring and
   taken down the existing clean-text-miss path (§5.5). There is no
   fail-fast mode.
3. **Extra segments are requested through segment-qualified seeds** (§5.4).
   There is no `game.toml` table.
4. **The overlay cache uses per-segment subdirectories** (`seg-kuseg/`,
   `seg-kseg1/`) under the existing cache tag (§5.7). The filename grammar
   does not change.
