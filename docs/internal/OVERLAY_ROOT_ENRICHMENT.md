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

## Done when

- The no-split guard is in, with a test that a mid-function candidate is
  demoted, not rooted.
- Enrichment is on by default in `compile_overlays.py`, and part of the
  cache key.
- The four titles match interpreted-only fingerprints and pass a play check.
- The `no-func-ids` cause is known and fixed at the source.
