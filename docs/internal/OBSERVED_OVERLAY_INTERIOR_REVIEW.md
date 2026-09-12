# Observed overlay interior recovery (WO-3)

Review date: 2026-09-12. Baseline: master `89db8168` (after PR #348).
Implementation branch: `fix/observed-overlay-interiors`.
Tracking: `beads-eio.3.145`.

## Verdict

The RoomLib brief identifies a real shared-framework coverage failure, but
parts of its explanation and proposed remedy are too broad. The failure is
reproduced without Parasite Eve assets, and independently in saved Tomba and
Mega Man X6 captures. This branch repairs the selective recovery path; it does
not import a game's entire seed file or promote every observed PC to a root.

The five reported Parasite Eve addresses and its performance figures are not
independently verified. The owner does not have the underlying capture bytes,
game configuration, or seed file from that run.

## What actually fails

`classify_overlay_seeds()` initially recognizes a valid dispatched-to PC as
`DISPATCH_INTERIOR` when it lacks a callable boundary. It then requires exact
reachability from a discovered host before emitting that interior as a shared
alias. Rejecting a hostless shared alias is intentional: treating every such
address as a new walk root can truncate its real host.

However, `make_interior_fragment_job()` reconstructed its candidate set from
only the surviving classifications. Consequently a rejected, executed dispatch
entry never reached the already-existing isolated fragment compiler. With no
other strong evidence or explicit `--force-interior`, the miss persisted.

The fix retains dispatch evidence after the existing address, instruction-start,
and producer checks, independently of later shared-host ownership. Only entries
also present in execution evidence gain this automatic recovery. Trailing
delay-slot guard words are readable but cannot become new fragment entries.
The shared root/alias classification is unchanged.

Recovery still requires an exact, current-byte native F identity, not merely
containment in an existing compiled range. Publication uses the existing isolated
compiler, generated-code/entry audits, guarded ranges, ABI checks, deterministic
failure memo, byte-variant recipe merging, and candidate-capacity limits.
Diagnostics distinguish shared-seed rejection from a retained fragment demand;
an empty shared root set is no longer described as necessarily data-only.

## Corrections to the brief

- Runtime capture JSON deliberately writes `function_entry_pcs: []`. The offline
  classifier derives entries later. That raw empty field alone does not prove
  that a PC was dropped or is absent from a DLL manifest.
- Existing classification diagnostics already list exclusion reasons. The gap
  was that a legitimate shared-alias rejection could also erase the independent
  recovery demand; the new diagnostic makes the two outcomes explicit.
- Known game/decomp seeds are not sufficient authority for every byte variant at
  a reused RAM address. Blindly unioning them into all overlapping captures can
  nominate data and create excessive compilation work.
- A CRC match proves byte identity, not that classification or generated control
  flow is correct. The existing compiler and publication audits remain essential.
- This change deliberately retains the execution-evidence gate. A split-schema
  capture with missing/empty `executed_pcs` (and no `observed_pcs` fallback) does
  not gain speculative interior recovery from `seeds` or dispatch metadata alone.

## Validation

### Automated, asset-free checks

- The new synthetic test first failed on master: valid, interpreted dispatch
  entry, no shared host, empty seed output, and `recovery_job=None`.
- It passes on this branch. Negative cases cover ordinary interpreted PCs,
  static-only/legacy seed nominations, missing execution evidence, invalid entry
  words, unaligned/out-of-range PCs, producer padding, and guard-only words.
- Real CPS CLI compilation recovers a hostless entry despite an empty shared
  root set. Duplicate records produce one shard. A repeated run writes no DLL or
  manifest. Changing guest bytes requires a new guarded identity; revisiting
  either compiled variant is a no-op.
- Fresh Windows x64 Clang 22 Release recompiler build succeeds. All 65 enabled
  recompiler CTests pass, including the new checks inside
  `aot_overlay_discovery`; three pre-existing tests remain disabled. The real
  artifact tests also exercise native MinGW GCC.
- An additional 35 overlay-tool Python tests pass. No runtime/emitter source or
  game configuration changes are part of the patch.

The first CTest run in the fresh worktree hit the stale-tag guard because the
runtime's generated hash header was absent. Installing the identical header
produced by the canonical recompiler build fixed the test setup; the subsequent
full run passed. Both emitters/headers use codegen hash `30317be3`.

### Real captured-byte comparison

Six saved captures were compiled into separate empty baseline/fix caches with
the production CPS compiler path. Shared seed output was identical in all six.

| Capture corpus | Previously dropped executed entries | Exact entry misses, before → after | DLLs, before → after |
| --- | --- | --- | --- |
| Tomba (2 captures) | `8005CDDC`, `8005CDE0`, `8005CDE4`, `8005CDE8` | 4 → 0 | 2 → 6 |
| MMX6 (4 captures) | `801EC8D8`, `801EC8E8`, `801EC91C`, `801EC92C` | 4 → 0 | 4 → 8 |

Every first/repeated invocation exited successfully. Each fixed corpus adds
four required isolated shards once; a second invocation adds nothing and leaves
all DLL/manifest hashes unchanged. These results demonstrate bounded behavior
for these corpora, not a universal compilation-cost bound for every game.

### Live regression

Eight headless runs used SCPH-1001 LLE, authentic game configuration, isolated
cards/caches, screenshots, and TCP diagnostics: Tomba and MMX6, baseline and fix,
cold and warm caches. Each passed 11,000 frames and exited with code zero. All
eight reported zero interpreter aborts, zero candidate overflows, and 32
completed card reads. Screenshots show Tomba's opening movie and MMX6's movie,
title, and attract-mode gameplay. Early Tomba frame-1500 screenshot requests
reported the guest display disabled on both builds; later captures succeeded.

The native runtime/game executables were identical copies of the already-built
master binaries, with separate test configurations selecting the baseline or
patched Python overlay producer. The recompiler was freshly built for the fix.
Rebuilding unchanged game/runtime code is not necessary for this Python-only
change; the tests exercise newly generated DLLs against the matching ABI.

The warm MMX6 baseline records 21 interpreter entries across the four recovered
PCs; the patched warm run records none at those PCs. The warm Tomba runs retain
the same interpreter counts at the four Tomba PCs, although guarded current-byte
cache entries now exist. Thus this is a Tomba compile-coverage fix plus a live
non-regression result, not evidence of a Tomba speedup or a claim that every
runtime fallback path now uses those entries. Total timing/counter differences
across asynchronously compiled runs are not a controlled performance benchmark.

Local evidence is under `F:/Projects/psxrecomp/_validation-roomlib-interiors/`:
`corpus-report.json`, `validate_capture_corpus.py`, and the baseline/fixed
per-game `results/retail-{cold,warm}/report.json` and screenshots. No game assets,
captures, generated DLLs, or user saves belong in this PR.

## Merge risk and remaining limits

The source change is limited to evidence retention, fragment scheduling, and
diagnostics. It does not change shared function partitioning, generated-code
semantics, timing, runtime admission, or the overlay ABI/cache tag. Existing
guarded artifacts remain reusable, and new demands are checked independently of
primary-cache hits.

Residual risk is newly exercised native code exposing an existing translator or
runtime problem that interpretation previously hid, plus additional first-time
compile work for genuinely executed entries. Existing bounds/audits mitigate but
do not eliminate those risks. These tests support review/merge of the scoped fix;
they do not establish complete gameplay or hardware-oracle equivalence.

Not covered: the original PE room transitions/performance, Tomba2, full
playthroughs, cross-platform live execution, or proving every possible captured
dispatch can be compiled. No merge or downstream pin bump is part of this review.
