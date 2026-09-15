# TAS stack: objective fixes and validation checkpoint

Date: 2026-09-15. Central tracker: `beads-eio.3.164`.

## Outcome

The reproduced test/build failures are repaired locally. The full enabled
recompiler/TAS and runtime suites pass. Fresh Tomba, Mega Man X6 and Ape Escape
builds pass bounded cold/warm runs; Ape's actual Load Game route displays the
populated save list without a game-specific card workaround.

This is not approval to merge the entire experimental runtime stack. Exact
source-profile retail replay qualification remains outstanding. The owner has
approved committing/publishing the corrections. Local commits are prepared;
publication onto the contributor's PR branches is blocked by GitHub permissions.
Nothing has been merged or changed on a contributor's branch.

## Branch and review scope

- Worktree: `F:/Projects/psxrecomp/_review-tas-stack-20260915`.
- Local branch: `fix/tas-regression-gates-20260915`.
- Correction base: `f7652e2b82cdd474c40d6d27a05ab4ccbf47795d`, the complete
  submitted stack. Corrections are appended, without rewriting that history.
- The owner approved the human-audit checkpoint. Prepared code commits:
  `65fd320f` (test gates), `0ac5bb84` (runtime CI), `637c0e36` (dead card option).
- Upstream master checked: `193a60b805e1eaa853129d6ccf63022440d4b143`.
  Its tree exactly matches stack base `5968162f`; there is no current tree-level
  rebase conflict to resolve. This does not imply runtime equivalence of the
  later stack slices.
- [Original upstream PR 361](https://github.com/RetroPortingToolKit/psxrecomp/pull/361)
  is closed. [PR 366](https://github.com/RetroPortingToolKit/psxrecomp/pull/366)
  is already merged.

The remaining draft stack is:

| Draft | Slice | Reviewed head |
|---|---|---|
| [Alexbeav #32](https://github.com/Alexbeav/psxrecomp/pull/32) | Source timing core | `033ba4f4` |
| [Alexbeav #33](https://github.com/Alexbeav/psxrecomp/pull/33) | TAS/replay tooling | `51e63d01` |
| [Alexbeav #34](https://github.com/Alexbeav/psxrecomp/pull/34) | GPU source projection | `bd885702` |
| [Alexbeav #35](https://github.com/Alexbeav/psxrecomp/pull/35) | Controller/card replay | `ab8e73a0` |
| [Alexbeav #36](https://github.com/Alexbeav/psxrecomp/pull/36) | CD-ROM/MDEC source work | `732bc3f1` |
| [Alexbeav #37](https://github.com/Alexbeav/psxrecomp/pull/37) | Campaign documentation | `f7652e2b` |

The authenticated `mstan` account has `push=false` on `Alexbeav/psxrecomp`;
fork PRs 32 and 37 report `maintainerCanModify=false`. Direct updates require
write access to the fork. An upstream handoff branch and linked PR notes are
the alternative awaiting owner direction; no force-push is appropriate.
Relative to master this local branch contains the contributor's runtime work;
only the **new local correction diff** is test/CI/tooling/documentation work.

## Objective corrections

1. Remove obsolete `test_sio_card_repair.c`, which referenced deleted state and
   asserted that the retired hack injects IRQ7/I_MASK. Keep the four production
   SIO no-hack scenarios, including the environment-variable resurrection guard.
   The deleted obsolete fixture is recoverable from Git.
2. Supply the inactive-route input callback to four isolated SIO fixtures;
   restore missing CPU-boundary, exception-context and MDEC link seams in other
   isolated fixtures. These helpers are not linked into production games.
3. Keep assertions enabled for test executables in optimized builds, recursively
   including nested test directories. Add a compile/run regression verifying
   assertions really execute, production still has NDEBUG, and the fixture-only
   aggregate does not build production. All 212 inspected production runtime/
   oracle compile commands remain free of `-UNDEBUG`.
4. Fix the GNU GPU dot fixture's linker configuration, the missing host-path
   include, the stale SYS02 return-value expectation, and stale fast-forward and
   v7/v8 snapshot source checks. No device expectations were relaxed to conceal
   a production failure.
5. Add `psxrecomp-test-fixtures` and make the TAS CI workflow also build/run the
   runtime suite without retail assets. The new sequence passes locally with
   **no linked BIOS backend**. Hosted GitHub CI has not run these commits.
6. Remove the dead replay `--legacy-card-repair` option, its environment write,
   profile arguments and documentation. Tests check its absence and retain
   rejection of unqualified card models. Old external replay commands must drop
   that argument. Runtime card behavior, card-image size/hash validation and
   source-profile identity gates are unchanged.

No local edits to `runtime/src`, `runtime/include`, `recompiler/src`, generated
C, release pins, or original game saves were made.

## Automated validation

| Surface | Result |
|---|---|
| Recompiler/TAS, GNU 16.1, UCRT Windows, Release, CHD enabled | 161/161 enabled tests pass |
| Runtime fixtures, GNU 16.1, Release | 118/118 enabled tests pass |
| Fresh no-BIOS runtime fixture aggregate and CTest sequence | Build passes; 118/118 enabled tests pass |
| New Release assertion/aggregate regression | Passes with GNU 16.1 and Clang 22.1.8 |
| Whitespace/error check | `git diff --check` passes |

Three pre-existing recompiler tests remain disabled: `interpreter_perf_guards`,
`runtime_perf_diag_guards`, `overlay_pair_dedup_runtime`. Runtime keeps the two
pre-existing disabled tests `sio_dualshock_rumble_test` and `spu_fidelity_test`.
Disabled/skipped/unconfigured tests are not counted as passes. Optional netplay
dependencies, hardware GL readback and a live independent Beetle oracle were
not configured for this run.

Local test evidence is in `build-review/Testing/Temporary`,
`build-runtime-review/Testing/Temporary`, and `build-runtime-ci/Testing/Temporary`.

## Fresh game validation

Artifacts and reproducible scripts:
`F:/Projects/psxrecomp/_validation-tas-stack-20260915`.

All three games and SCPH1001 were freshly generated, then built using Clang
22.1.8 / Ninja / RelWithDebInfo. BIOS HLE was off. No old overlay cache was copied;
cold runs generated ABI-24 overlays, and warm runs reused the resulting cache.
All inherited PSX environment overrides were cleared. No source-replay profile,
card workaround, IRQ suppression, fingerprint bypass or manual generated-C edit
was used. These headless runs exercise software video and guest audio generation,
not audible host playback or the enhanced renderer.

| Title / run | Final frame | Native overlay dispatches | Inspected visible coverage |
|---|---:|---:|---|
| Tomba cold | 11,002 | 910,366 | Opening FMV and title |
| Tomba warm | 11,010 | 1,076,585 | Opening FMV and title |
| MMX6 cold | 11,015 | 1,103,542 | Opening sequence and gameplay attract/demo |
| MMX6 warm | 11,009 | 1,206,539 | Opening sequence and gameplay attract/demo |
| Ape cold | 11,008 | 990,427 | Opening sequence and title |
| Ape warm + Load Game | 11,007 | 16,071,049 | Title, Up, X, populated save list |

Every run: exit code 0, zero kernel mismatches, zero dirty-interpreter aborts,
and nonzero guest SPU output. These are bounded liveness/correctness checks,
not a performance comparison or full-game certification.

Ape evidence: `ape/results/candidate-warm-load/load-settled.png` and `report.json`.
The list shows Wlau1 100%, Wlau2 83.3%, File3 83.3%, and File4 No Data. The original
cards retain their pre-test hashes. The isolated writable card1 copy changed five
bytes confined to sector 63 during this testing. This
write has not been attributed against an exact-master control, so this run is
**not** a save/write/reload persistence qualification. Do not overwrite original
cards with these test artifacts.

Test executable SHA-256:

```text
tomba 742ce2341c059a2c06fb9b3acd1c8d84fb77dfdd09f533a93c6e3c0756cf7c4f
mmx6  1ae381d0cda638908842453b8709a770256a5dd918d9e958d289f4afa96a40f2
ape   b090a1f07e8334309408efc8712ff0af01e7ffdc839abf67ae2cac1b7dfcae44
```

BIOS SHA-256:
`71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3`.
Fresh BIOS emitter fingerprint:
`914f43179654baabb1e48785b181ceda8f323a7eae662c458f3ca6e6fee00c29`.

## Remaining work and merge judgment

- Publish the approved correction commits through an authorized branch, then
  attach the change summary and prerequisites to the draft PRs. Direct fork
  access is currently unavailable. Run hosted CI on the chosen integration
  branch. The corrections are low game-regression risk: production code and
  flags are unchanged. Retired replay CLI arguments are the intentional
  compatibility change to call out.
- Keep the experimental runtime drafts as drafts until fresh integrated Tekken,
  Pepsiman and Biohazard source-profile replays establish their declared input,
  return-clock, RAM and card identities. Historical campaign passes are not
  evidence for this integration. The exact retail assets/reference sets were not
  located in the checked local game/download/PSX-library locations.
- Biohazard's historical one-cycle/terminal-RAM mismatch remains an explicit
  unresolved qualification issue, not something to waive based on these smokes.
- Full gameplay, audible FMV/audio sync, save/write/reload, enhanced renderers,
  rewind/netplay and Tomba 2 were not qualified here. A fresh exact-master
  comparison is required before attributing any subtle difference to this stack.

The remaining timing/fidelity questions are objectively testable with the right
assets and independent traces. They do not inherently require a subjective
opinion from the contributor. Human play remains useful for audiovisual quality
and broader gameplay acceptance, not as a replacement for identity checks.

## Suggested local follow-up order

1. Compare Ape's isolated card sector-63 write with a freshly built exact-master
   control using the same initial card and route. Preserve both output cards and
   traces; do not attribute the write to this stack before that comparison.
2. Exercise a save/write/clean-exit/relaunch/load route on isolated card copies.
3. Obtain the exact owned retail images and independent replay/reference sets,
   regenerate from the final integrated source, and run Tekken, Pepsiman and
   Biohazard identity gates. Do not bypass historical-fingerprint rejection.
4. Address any first divergence with a reduced objective fixture; retain draft
   status while a claimed profile has an unexplained mismatch.

The fixes were tested on the complete stack, not every intermediate draft
slice. Do not put the complete-stack head onto an earlier split branch: that
would import later runtime changes into its review. If corrections are moved
into earlier slices, apply only the relevant changes and revalidate each slice.

Tracker notes are saved centrally; Dolt push remains blocked by the pre-existing
missing remote data ref. No tracker initialization or unrelated repair was done.
