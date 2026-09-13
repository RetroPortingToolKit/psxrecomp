# SIO card workaround audit — 2026-09-13

Status: full removal implemented; owner-confirmed Ape Load Game and targeted
regression checks pass. Owner authorized integration after the review checkpoint
on 2026-09-13. This document records the pre-merge validation evidence;
the associated PR and Beads issue record the landed commit.
Beads: beads-eio.3.154.

Tracker notes are saved locally. Dolt push still fails because the central
remote's referenced data ref is missing; this task did not repair the tracker.

## Scope and acceptance

Owner requires no per-game correctness workaround, including an opt-in. Ape
Escape must work on the common implementation, and other titles must not be
broken to achieve that. Integration authorization is limited to this no-hack
method. Other PRs are review-only and game repository pins remain untouched.

Base: upstream master 85cd26f05c44999731f6b3320fb8a871fba68e9b.
Branch: fix/sio-card-no-title-hacks-20260913.

Integration refresh found one additional upstream commit, 7025bb5f, changing
rewind keyboard aliases in main.cpp. It does not overlap the card changes;
the final integration is refreshed onto that base and rechecked before merging.

## Findings

- 251456ee introduced the Ape repair; 1bbda282 ungated it on August 9.
- The default-enabled repair reads fixed Ape RAM addresses, injects IRQ7 and
  overrides guest I_MASK writes. Synthetic unrelated RAM can trigger this
  after an absent-card probe.
- Turning PSX_APE_CARD_UNSTICK off does not remove SELECT-time card ACK
  fabrication, INTC-pending ACK requeueing, or one-transition clock walking.
- The original SPU I_STAT clearing was already removed in 3592a19e on August
  17. It is not a current defect; the candidate preserves that correction.
- The cited ApeEscapeRecomp/tools/ape_memcard_loadtest.py and
  docs/APE_MEMCARD_LOAD.md are absent in the current game checkout and were
  not found in its tracked path history. Boot is not a load-path acceptance test.

Independent mechanism references inspected: local Beetle frontio.cpp
DoDSRIRQ/Write/Update and irq.cpp IRQ_Assert/IRQ_Write. SELECT deassertion
cancels pending DSR pulses; SIO does not write the INTC mask or wait for an
INTC acknowledgement before setting its own IRQ latch. A live Beetle retail
comparison has NOT yet been performed.

## Candidate changes (uncommitted)

- Delete the fixed-Ape-address repair, environment switch and I_MASK override.
- Remove its interrupt-check and GPU VBlank pump callers.
- Replace the handoff ring's fixed game RAM probes with SIO hardware state.
- Cancel pending card ACK on SELECT deassertion instead of flushing/requeueing it.
- Do not queue ACK events based on I_STAT.7.
- Consume elapsed shift/ACK deadlines without a per-call one-transition cap.
- Add production-SIO synthetic regression coverage and CTest registration.
- Delete the unused ChangeThread-deferral helper, its timer state and arming
  calls. Repository search confirmed that the helper had no callers. Correct
  the two old test-clock comments that incorrectly described it as load-bearing.

There is no runtime environment getter or opt-in path left. The old variable
name remains only in CTest setup to prove setting it to 1 cannot reactivate
the repair. The fixed addresses remain only in the reproducer and two comments
about a separate, retained exception-register-restoration fix in interrupts.c.
Device-only diagnostic rings are retained; they cannot repair guest state.

This is not a claim of complete SIO hardware accuracy. Other timing/device
semantics remain subject to the joint validation gate.

## Evidence and limits

Local evidence root: F:/Projects/psxrecomp/_validation-sio-card-20260913.

Production SIO, Clang 22.1.8 -O2, synthetic clock/card and unrelated RAM:

| Scenario | Master | Candidate |
| --- | --- | --- |
| Absent-card reset, repair enabled | 2 failures / 3 checks | 3 / 3 pass |
| Absent-card reset, repair disabled | 3 / 3 pass | switch deleted |
| SELECT before pending ACK, repair disabled | 2 failures / 4 checks | 4 / 4 pass |
| Existing INTC IRQ7, repair disabled | 2 failures / 3 checks | 3 / 3 pass |

Candidate results above were executed with the old environment variable set to
1, demonstrating that it cannot reactivate the removed repair. The coarse-clock
scenario also passes: a single advance consumes both shift and ACK deadlines.

The final cleaned source builds all 57 registered runtime test executables.
Full runtime CTest: **83 executed passes, zero failures, one skipped test and
two disabled tests** (CTest reports 84 including the skip). This includes all
four new scenarios, the existing card protocol test and SPU interrupt-ownership
test. Skipped: exit_critical_section_test. Disabled: sio_dualshock_rumble_test
and spu_fidelity_test. Netplay and the optional hardware GL readback test were
not registered in this dependency configuration and are not counted as passes.

Fresh recompiler, retail SCPH1001 BIOS and Ape game generation/build completed.
The candidate Ape executable also builds. A missing retired pump caller in
gpu.c was caught at link time and removed, followed by a successful link.
The initial direct-emitter BIOS build lacked its fingerprint stamp. Running
the canonical tools/regen_bios.sh subsequently produced the genuine stamp;
the missing-stamp warning is resolved, not suppressed. SCPH1001 generation
emitted 1309 functions, with one pre-existing interpreted exclusion and one
skipped unsupported-COP1 function; this is not a zero-fallback coverage claim.

Master-with-flag-off was explored on two bounded live runs using copied cards,
screenshots and TCP rings. Those runs are INCONCLUSIVE for Load Game: no
confirmed complete card-load route was captured. Attract/title/scene images
and boot card reads must not be represented as successful save loading.
The headless input/card route still needs qualification; an initial isolated
config omitted the game's locked analog controller table and was corrected.

### Ape Load Game: qualified with the owner

The owner supplied the route: title screen, Up, Load Game, X. On the preserved
master baseline with the repair ON, route-baseline-up.png shows the populated
save selector. Its handoff ring contains actual unstick/nest-pulse/mask-hold
events, confirming that the baseline exercised the repair. This is not merely
an enabled-but-idle control.

On the no-hack candidate, the owner accidentally selected New Game first.
After relaunching the same binary (PID 51960, port 4591), the owner reported
"it looks good!". Captured and visually inspected
route-candidate-user-confirmed.png shows Wlau1 100%, Wlau2 and File3 83.3%,
and File4 No Data. This directly qualifies completion of the reported Checking
screen, not just boot or a transaction counter.

Associated captures: route-candidate-user-confirmed-txns.json (721 closed
transactions), -handoff.json (hardware-state-only entries), and -freeze.json
(172 complete card reads, zero dirty-RAM aborts, SIO IRQ not pending at capture).
The single BASCUS-94423SYS card file contains gameplay save slots, as the
screenshots demonstrate; its filename does not imply settings-only content.

The user-tested binary has SHA256
A23179DF711CEB82AA15801CC1E0DF01658E6F34DB8FA3E04DB3092102E984AD.
After that confirmation, only the unused deferral helper/state and obsolete
comments were removed. The final build is psx-card-validation-final.exe,
SHA256 2958F8AE9B038234AA3FD4B000E78C51EB98315B6D97820D950DAA487031610D.
It was built under a different name to preserve the user's open test window.
The final-cleanup executable has not itself received another manual Load Game
test; the confirmed executable already lacked the opt-in and all active repair
paths.

### Cross-title regression

Fresh Tomba and MMX6 codegen/builds succeeded against the same candidate.
Initial cold runs: Tomba 7014 frames and MMX6 7013 frames, both exit 0, zero
kernel-blessing mismatches and zero dirty-RAM aborts. Inspected screenshots
show Tomba's intro FMV and MMX6's title/logo. These are bounded boot/FMV/title
smokes, not proof of gameplay or game-level save loading. Reports live under
<title>/results/sio-removal-cold/report.json in the evidence root.

Final-cleanup builds and longer warm-cache smoke reruns also pass:

| Title | Final frame | Exit | Inspected milestones |
| --- | --- | --- | --- |
| Tomba | 11002 | 0 | Intro FMV, title, attract FMV replay |
| MMX6 | 11001 | 0 | Title/loading transition, in-level attract demo |

Both final reports have zero kernel-blessing mismatches and zero dirty-RAM
aborts. Reports and screenshots: <title>/results/sio-removal-final-warm.
No input was injected by these smoke runners; the MMX6 level is an attract
demo, not a player-controlled gameplay test.

All title validation uses the retail SCPH1001 LLE BIOS with the existing shared
runtime scheduler, copied cards and isolated generated output/cache/configs.
UI, netplay, rewind, PGXP and Vulkan are disabled in these validation builds.

Original game cards and dirty source checkouts are outside the mutation scope.
Only copied cards under the evidence root were used. Original Ape card hashes
were rechecked after the owner's test and are unchanged:

- card1.mcd: 8D116A5E2D7F29F35EF9D3D8724B8CFFE85ED6C5870C198515AB59FC67A06262
- card2.mcd: 7706C7D43EDAF8CB7618E574F03457105153E3BDC196DB803A600AD96A8F58E8

Baseline processes exited. The owner's candidate window was left untouched;
it had also exited by the final check. Automated smoke runners closed only
their own child processes; no listeners remain on ports 4591-4593.

## Remaining limits / integration gate

- No full title playthrough, actual save/write/reload round trip, Tomba/MMX6
  in-game card-menu acceptance, live Beetle comparison or netplay validation
  has been performed. Do not convert smoke coverage into those claims.
- Tomba2 and game repository pins are untouched. PR361's other runtime changes
  are not included in this isolated candidate.
- Final smoke reports and screenshots have been inspected, and the owner
  authorized merging after this post-build checkpoint. No opt-in workaround and no
  merging a known cross-title regression. The reported Ape Load Game blocker
  is now owner-confirmed clear;
  that does not assert complete SIO hardware accuracy or zero regression risk.
