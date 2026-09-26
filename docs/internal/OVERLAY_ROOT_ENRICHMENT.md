# Overlay root enrichment: next steps after #386

#386 added `tools/enrich_overlay_captures.py`. It scans each captured overlay
image for likely function starts and writes them to
`static_discovery_entry_pcs`. Nothing runs it automatically yet. This page is
the plan for making it the default and getting more coverage from it, without
any risk of softlocks.

## Where we are

- **Measured on Ace Combat 3** (x6568tank's captures, compile tooling fixed):
  rootless images went from 250 to 1, and capacity skips from 3,028 to 0.
  There were 8 shard failures, all `fragment: no-func-ids`, and all of them
  also appear in the comparison tool. So no new failures.
- **A richer tool finds 47% more starts** (15,008 vs 10,191). That gave +31%
  entries served and +19% covered bytes. It also gave 19 failures instead of
  8, a 38% larger cache and 39% more compile time.
- **Our titles' captures** have no `function_entry_pcs`, only
  `dispatch_entry_pcs`. Enrichment would add:

  | Title | Dispatch entries | Starts added |
  |---|---|---|
  | Tomba | 38 | 764 |
  | Tomba 2 | 178 | 3,316 |
  | Mega Man X6 | 325 | 901 |
  | Ape Escape | 149 | 459 |

## The rule that matters

A shard **failure** is safe. The compiler refuses the piece and it runs
interpreted. The real risk is a wrong start point that **passes** every check.
A walk root is a hard cap, so a false root in the middle of a function splits
that function. The output is valid C that passes every audit, but it behaves
wrong. That is the mid-function-seed softlock class, and it never shows up in
the failure count.

So the failure count is not the safety metric. The goal is **zero
mis-splits**, then as much coverage as possible.

## Plan

1. **No-split guard** (in `compile_overlays.py`).
   - Walk each candidate root without hard caps.
   - A candidate that another root reaches by fallthrough or branch (not by
     `jal`) is inside that function. Demote it to `DISPATCH_INTERIOR`, which
     makes it an alias entry and never a cap.
   - This extends the protection dispatch entries already get to every
     static root.
   - With this guard, a wrong guess can only cost speed, never correctness.
2. **Move enrichment into `compile_overlays.py`.**
   - Call the same root derivation when building `pre_roots`, instead of
     running a separate pipeline step.
   - Add it to the overlay cache/recipe key, so shards built without it are
     never reused.
   - Keep the script for inspecting captures.
3. **Add more root categories, behind the guard.** These are the sources the
   richer tool uses and #386 does not:
   - `jal` edges from the main EXE and other images into the overlay. These
     are real call evidence, and likely the safest win.
   - Frameless starts right after `jr ra` + delay slot. These are riskiest;
     `compile_overlays` itself says this spot can be data.
   - Pointer tables with fewer than 3 entries, and `lui`/`addiu`-built
     pointers.

   Measure each category on its own: roots added, entries served, failures.
4. **Explain the `no-func-ids` failures.**
   - Find which requested entries produce an empty ranges manifest, and why
     they were requested.
   - Fix the request side so nothing unprovable is asked for. Do not
     reclassify these as skips just to reach zero.
5. **Validate on Tomba, Tomba 2, Mega Man X6 and Ape Escape.**
   - Run interpreted-only and enriched-native to the same frame, and compare
     `frame_fingerprint` rings. They must match.
   - Then play and explore areas that load new overlays: levels, menus,
     cutscenes, save and load.

## Status: steps 1 and 2 (beads-eio.3.177)

- **Guard** (`no_split_partition` in `compile_overlays.py`, same rule in
  `FunctionAnalyzer::analyze_exact_entries` for overlay mode). For each root,
  the following candidates its uncapped walk reaches are absorbed together
  (a switch resolves only when all its case labels are inside the walk); the
  run stops at the first unreached root, so a tail call over another function
  keeps its target. Applies to every root source; promoted kernel orphans are
  exempt. Delay-slot candidates are never roots or aliases.
- **Evidence.** A jal or pointer only proves a function start in the image
  that is resident when it runs. STRONG = a prologue, or a jal/table target
  that this image bounds (`jr $ra` before it plus the CFG probe). WEAK = CFG
  probe only; kept only if no possible function start reaches it, and dropped
  if a host's local branch jumps over it. Cross-producer calls are never weak
  evidence.
- **Default on, one code path.** `classify_overlay_seeds` derives the roots;
  `tools/enrich_overlay_captures.py` is an inspection tool built on the same
  functions. `--no-root-enrichment` / `PSX_OVERLAY_ROOT_ENRICHMENT=0` is
  diagnostic only.
- **Cache key.** `tools/compile_overlays.py` is in
  `runtime/codegen_hash_sources.cmake`, so any root-policy change moves the
  cache namespace.
- **Tomba (25 AOT images).** Emitted fallthrough splits: master 57, #386
  enrichment 2,072, guard 0. Fingerprints match master on every value and
  timing field over the 30.7k-frame route and the 32k attract run. Sound
  enrichment adds almost nothing on Tomba: 12.3k of #386's extra roots were
  jump-table case labels inside functions. The way to that coverage is
  resolving those tables, not rooting their labels.
- **Not caused by splits:** master's own 3-4 cycle drift from the interpreter
  at frame 2111. It is unchanged with the guard.
- **Tomba 2 (23 captures, autocompile path).** Two guard gaps found and
  fixed. (1) Roots the region shard leaves out are built in strong-root
  supplement fragments from `dispatch_root` seeds only, so aliases of those
  hosts were served by no shard (127 captured dispatch entries lost). The
  supplement now passes each host's aliases as `interior` seeds, and a single
  root whose aliases break its fragment is rebuilt without them. (2) When
  every host of an absorbed candidate stays interpreted (0x800BDF10 walks
  into data and fails the audit), the candidate is compiled as the root it
  was before the guard. Left: 14 captured dispatch entries at delay slots,
  which the guard keeps interpreted by design (master aliased them).
- **Tomba 2 fingerprints do not match master exactly,** and the cause is
  master's own splits. Frame 6797: master splits 0x8009B0C0 from its
  fallthrough 0x8009B0CC; the split leaves the nested call unit, so master
  takes a CD IRQ inside the loop, while the unsplit function runs as one
  unit and the runtime's nested-unit rule defers the IRQ to its return
  (with `PSX_OVERLAY_UNIT_DEFER=0` in both arms this fork goes away).
  Frame 7169/7363: master splits switch 0x8006D654 at its case label
  0x8006D690; the jump-table exit costs master 2 extra cycles. Values stay
  identical, and every route and attract screenshot is identical.

## Done when

- The no-split guard is in, with a test that a mid-function candidate is
  demoted, not rooted.
- Enrichment is on by default in `compile_overlays.py`, and part of the
  cache key.
- The four titles match interpreted-only fingerprints and pass a play check.
- The `no-func-ids` cause is known and fixed at the source.
