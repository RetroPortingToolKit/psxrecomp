# PlayStation enhancement parity

Owner-approved scope, 2026-10-06. Tracking: `beads-eio.3.277`; shared execution
work: `beads-wuhs`. Implementation is in progress. Source presence, a successful
build, a short gameplay check, and owner acceptance are distinct milestones.

## First review pass

Owner steering after the status check: prioritize runnable candidates and human
validation. Use build/configuration checks and focused tests for risky changes;
defer broad automated gameplay and performance matrices until owner feedback.
Windows review candidates come first in the build queue, with native Linux
artifacts following. Missing enhancements and known defects stay explicit.

For each candidate, provide its launcher/path and this short playtest checklist:

1. Launch, start/load, and play for a few minutes.
2. Check ordinary and ultrawide aspect settings: scene edges, HUD and dialogue.
3. Move the camera: look for jitter, seams, missing geometry and bad textures.
4. Pause/resume, change areas and revisit; note stalls and load times.
5. Check audio and existing multiplayer where applicable.

The shared guarded HUD anchor now also supports projection-and-stretch mode.
Title code identifies completed screen-space packets; a common edge/centre
anchor can span separate DMA lists. Native-wide translation, packet guards,
expiry and native 4:3 behavior remain covered by focused execution fixtures.
This addresses the mechanism behind Ape's separate Status backing/text batches;
the current title follows padded packet chains and repeatedly anchors its backing
with the labels. Broader visual review remains open. Tracking:
`beads-eio.3.125` and `beads-uutk`.

## Acceptance target

- Enhanced OpenGL is the normal renderer, with 1080p internal resolution.
- Native scene interpolation targets Display refresh. Temporal image blending
  alone does not meet this requirement. HUD, FMV, menus, pauses and transitions
  need appropriate presentation behavior without advancing gameplay twice.
- PGXP, stable world texture filtering and perspective correction apply where
  useful. Preserve pixel-art/UI treatment and fix seams, jitter and texel bleed.
- Adaptive view covers 4:3 through at least 32:9: world geometry, culling and
  activation, backgrounds, textures and HUD layout. Wider displays may stretch.
- Extend distance/fog and replace unnecessary subdivision when applicable;
  preserve visible content and required game behavior.
- Use resident assets, native decompression and bulk transfers where measured
  loads justify them. Initial startup, transitions and repeat visits all count.
- Qualified HLE is selected in normal builds; a functioning LLE reference is
  separately buildable. Compare required ABI/results/effects, and measure useful
  speed. See [HLE_EXECUTION.md](HLE_EXECUTION.md).
- Preserve and validate existing multiplayer enhancements. Do not add
  multiplayer to single-player titles as part of this campaign.
- Deliver reproducible Windows ZIP and Linux AppImage candidates for the
  primary disc variant. Other variants are explicit follow-up work.
- Owner playtesting supplies final acceptance. No title is marked complete
  merely because its settings expose these options.

## Current Windows review readiness, 2026-10-07

All 15 titles have executable-bound local Windows review launchers in
`F:/Projects/psxrecomp/parity-review-20261006`. `REVIEW.txt` is the current owner
checklist; `receipts/review-readiness.json` records the exact binary identities,
media/BIOS path checks and staged frame-rate defaults. These milestones do not
establish full feature parity or final gameplay/performance acceptance.

Latest owner policy: interpolation **defaults off across the campaign** after
Ape choppiness and THPS2 flickering in owner playtests. Ape is owner-approved
with its interpolation package absent from the shipped catalog. MediEvil I/II
received positive interpolation feedback and keep the option available,
default off. THPS2 exhausted its two allowed repair attempts: the first caused
skater jitter and the second caused severe scene flickering. Its interpolation
package and native plugin are now archived and absent from the current build.
Other packages remain opt-in pending title qualification.

Generic host pacing is deprecated and excluded from all 15 catalogs, along with
generic CD Speed. The MMX4 and old hidden Tomba Fast Loading wrappers used the
same generic detector and are now retained only in development. Tomba 1/2,
MMX6, V8 2nd Offense, MediEvil II and THPS2 retain their title resident/loading families.
The other nine title loaders remain unfinished; geometry HLE, BIOS shell skipping
and AOT caching do not satisfy that requirement. See [HLE_EXECUTION.md](HLE_EXECUTION.md).

All review configurations retain 1080p OpenGL and available view/distance/filter
improvements. A live THPS2 resolution query reported the 1080p preset, 5x effective
raster scale and 1200 internal lines from its native 240; no GPU clamp. This is
rendering resolution, not higher-resolution original textures. THPS2's initial
music playback is owner-approved after the shared GetlocP position fix;
its title resident asset loading is also owner-approved, default on.
THPS2 interpolation is disabled and hidden after both repair attempts failed;
no third attempt is authorized. Work proceeds serially by title. `GAME-PATHS.txt`
lists the executable paths; earlier review preferences are backed up.

The owner suspects background system contention/long uptime and will perform
final review after restarting. Further performance comparisons on this session
are deferred. Human-review launchers set `PSX_RENDER_PASS_VERIFY=0` and
`PSX_GL_PERF=0`, restoring their parent environment. Jersey already compiles GPU
diagnostics out. Synchronous GPU profiling is an additional possible source of
measurement overhead, not an established cause of the reported hitching.

| Title | Current first-review milestone | Main remaining gaps |
|---|---|---|
| Tomba! | Actual actor/camera replay, 1080p, resident kit and recent Any% FMV work; bounded native state checks pass | Exact native candidate owner approval; combined 0.17.0-alpha minor release, broader view/load/package review |
| Tomba! 2 | USA actual native replay; short moving-scene state checks pass; resident loading retained | Full 32:9, stable filtering, broad view/load coverage; Italian variant later |
| MMX4 | Actual native scene replay; moving attract check passes; fresh historical BIOS capture | Broader view/overlay coverage, loading HLE |
| MMX5 | Actual native scene replay; moving attract check passes; original-disc AOT | Broader view, full 32:9, loading HLE |
| MMX6 | Native replay/resident candidate includes current widescreen PR543 / `458e6ece` | Current combined-build owner gameplay/load/audio review |
| Ape Escape | **Owner-approved with interpolation off and hidden**; reviewed binary unchanged, other enhancements retained | Final packages; independent GitHub #18 and expanded streaming/activation remain separately tracked |
| Vigilante 8 | Title `95fa269` native fix, framework `4b754210`; main-thread replay and allocated terrain packet support; Sand Factory correctness check passes | Occasional taxing-scene hiccups; earlier slot01 unlocated; load gain not established; partial AOT |
| V8 2nd Offense | Built native replay/resident candidate and fresh captures | Broader distance/subdivision/performance/load review |
| Tsumu | OpenGL/1080p, PGXP/filtering and actual native first-puzzle replay; localization retained | 4:3 only, loading HLE and broader coverage |
| THPS2 | Owner-approved music and default-on resident loading; 1080p and distance retained; interpolation rejected after two attempts and removed | Broader level/visibility and existing multiplayer review; interpolation archived |
| Jersey Devil | Built native replay/renderer candidate, 13 native pairs | Wider view/scene/load review; loading HLE absent |
| Spider-Man | Built native replay/PGXP candidate, fresh historical shards | Broad view/scene/load review; loading HLE absent |
| Spider-Man 2 | Native packed-vertex HLE and bounded rooftop replay; persistent local review candidate | Broader levels, load coverage and display-refresh cadence |
| MediEvil | Built native replay/PGXP/filtering/distance/subdivision candidate, static original-disc AOT | Resident-loading adapter and broad gameplay/view review |
| MediEvil II | Built native fidelity/resident PP20/level candidate, static original-disc AOT | Broad combined-build gameplay/view/load review |

Shared `77f3a909` retains the shown generation plus two pending generations
under the existing total 256 MiB image budget. Ape's old single-pending path
accepted 340 phases but presented only 24 native images in a six-second sample;
the queue fix presented 491 native images / 502 swaps at about 59.4 guest Hz.
Those presentation counts include reused phases; they are not unique-frame FPS
or proof of 165 Hz delivery. Separate exact rollback checks and focused LLE/HLE
command/provenance fixtures pass. Owner cadence review remains necessary.

V8's main-thread binding initially refused its extended terrain packets because
the shared OT parser only accepted main RAM. `4b754210` accepts complete allocated
GPU-DMA aperture ranges while retaining malformed-packet, allocation-end and
BIOS guards. The mixed RAM/aperture fixture and 13-pass live exact state check
pass. Separate short redraw/feature-off samples are diagnostic evidence only;
they do not locate or resolve the earlier owner slideshow checkpoint.

Windows review readiness, final owner acceptance, missing features and native
Linux/final release packaging remain separate. Nothing has been published.

## Historical starting inventory and remaining work

This records implementation evidence found during the initial repository audit,
not full-game qualification. Framework baseline is `7b253941` (CODEGEN 18).

| Title | Existing evidence / starting point | Work still to qualify |
|---|---|---|
| Tomba! | `3c8d36e`; custom native renderer, shared resident kit | Native interpolation replacing blending, broad resident coverage, full adaptive view and packages |
| Tomba! 2 | `5fc54c8`; shared seamless USA loading | Native interpolation replacing blending, view/residency coverage and packages; Italian variant later |
| Mega Man X4 | `51f5f44`; earlier widescreen/blending | Native interpolation, current renderer/filtering, load path and view coverage |
| Mega Man X5 | `c8fde08`; earlier widescreen/blending | Native interpolation, current renderer/filtering, load path and view coverage |
| Mega Man X6 | master `3b4b5dd`; native interpolation; resident/mod branch `3bf0205` | Integrate resident work with current framework; all applicable defaults, views and packages |
| Ape Escape | release `a8e219a`; native interpolation, private native packet producer | Shared build-selected HLE, gameplay interpolation qualification, startup/load service and packages |
| Vigilante 8 | fidelity/packaging checkpoint `3591161`; OpenBIOS Ski Resort gameplay, adaptive 4:3–32:9, PGXP, 1080p and stable filtering checked | Native interpolation, shader batching performance, load throughput, Linux package and broader gameplay; donor content excluded |
| Vigilante 8: 2nd Offense | `e1133d10`; native replay, PGXP, filtering, resident load-floor improvement (~307 to 45 frames in one case) | Backgrounds, subdivision/distance and remaining performance cases; full validation |
| Tsumu | `9fa07da`; English localization, older software/4:3 baseline | Applicable enhanced rendering, presentation, adaptive view and load audit |
| Tony Hawk's Pro Skater 2 | `6890a0da`; native replay, extended distance, existing multiplayer | Wider visibility and distance limits, single/split/network play checks, load audit |
| Jersey Devil | `a3958a3`; native replay across banks | Full visual/view/residency matrix and current candidate packages |
| Spider-Man | `66412b4`; native replay and PGXP | Full view/texture/load coverage and current candidate packages |
| Spider-Man 2 | `60c8e80`; PGXP, motion-adaptive image blending | Native interpolation under implementation, then view/load and package qualification |
| MediEvil | enhancement branch `84202de`; native replay, PGXP, filtering, 3x distance, subdivision bypass | Resident loading adapter, full view and gameplay coverage, packages |
| MediEvil II | master `6816fc0`; quality reference `32ea083`; 5x/1080p, resident PP20/levels | Preserve reference features and validate full game; one measured load improved 8.30 to 1.38 seconds |

## Vigilante 8 content parking

The owner abandoned cross-game assets. These local branches retain that work:

| Branch | Preserved checkpoint |
|---|---|
| `archive/v8-v10-content-20261006` | `8a68555` |
| `archive/v8-n64-arena-20261006` | `ea3a006` |
| `archive/v8-renderer-and-media-20261006` | `8169c75` |

The earlier sequel archive at `f3e4ab1` is also retained. The active
`feat/v8-fidelity-parity-20261006` branch removes donor importers and converter
tests. Fidelity, rendering performance and loading speed are its only focus.

## Shared work and validation receipts

The framework now has build-selected implementation families, implementation
manifests, snapshot/network execution compatibility, and a stateless native GTE
entry for bulk services. It is not a measured speedup by itself. Ape's packet
producer is the first integration consumer.

The initial six targeted CTests pass: execution profile linking and source
identity, reference/enhanced GTE contracts, reference/enhanced snapshot handoff
and atomic rejection, and network content gating. Production main/GTE/snapshot
translation units compile, including production netplay with networking enabled.
Ape, Spider-Man 2 and Vigilante 8 Windows runtime candidates have linked;
useful title HLE performance comparisons are still required.

Baseline framework tests also exposed three existing stale fixtures:
`gl_texture_filter_test`, `gpu_gp0_history_guard_test`, and
`savestate_status_protocol_test`. Their wiring is repaired and all three now
pass, including the hidden real OpenGL filtering probe. A complete suite run
against the new worktree remains outstanding.

## Execution and promotion

The original plan used bounded title workers; the current owner instructions
require solo work and no worker resumes or delegation. Shared changes remain
coordinator-owned. At most two compile jobs and one owned game process run at a time;
validation uses isolated saves and ports. Preserve personal sessions and saves.

For each title record disc identity, framework/game revisions, actual applied
features, rejected cases, screenshots and gameplay paths, load/frame metrics,
build profile and manifest, package hashes, and owner feedback. Shared
mechanisms belong in psxrecomp; disc addresses and asset contracts belong in
the game. A missing or inapplicable feature needs an explicit reason.

Candidate branches and local commits are authorized. Public source pushes,
merges and releases are not part of the current authorization. Beads stays open
through owner acceptance. The central Dolt remote currently fails to push due
to a missing remote data ref; local issue updates remain durable.

Use `tools/qualify_render_passes.py` during a known playable scene, with replay
verification enabled. It records before/after counters, the supplied executable
hash and its execution manifest. Zero replay, image blending, disabled replay,
counter resets or verification failures cannot produce a passing sample. This
is one evidence window; visual coverage, load speed and owner acceptance remain
separate checks. Use `tools/load_probe.py` for passive load-window measurements.

Run correctness and performance windows separately. `PSX_RENDER_PASS_VERIFY=1`
adds synchronous GPU readbacks plus state hashing. After recording correctness,
relaunch without it and use `--purpose performance`. The tool rejects a
performance sample with active verification. Spider-Man 2's initial verified
replay took 28-35 ms; verification off measured 19.531 ms, of which 19.176 ms
was guest draw. That evidence points to title geometry processing rather than
shared GPU checkpoint/capture as the remaining throughput bottleneck.

The shared presenter now honors HOLD for native phase generations instead of
crossfading neighboring phase images. Phase selection tests and the real OpenGL
probe pass, including red/blue native images that must stay distinct
under HOLD while explicit blend mode still mixes them. Title requalification
on this correction remains required.

The expanded GL probe now passes 509 checks. Shared commit `7c9e13ec` carries
nearest/bilinear/stable selection per primitive, preserving sharp untracked UI
while batching filtered world geometry. Across all three texture depths, native
and 5x rendering and native-wide margins match the original filter-boundary
submission exactly. Its opaque fixture goes from eight batches to one; mask
checking stays at eight and semitransparency at nine. Vigilante 8's measured
filter-transition pressure motivated this work; game speed is still being
compared on an isolated shader-only framework checkout.

The campaign framework includes these shared checkpoints:

- `8fe08921`: pre-materialized generated reference bodies build through include
  wrappers, avoiding duplicate Ninja rules without maintaining copied LLE code.
  The profile fixture checks both profiles, source-specific flags, local
  includes, automatic changed-body identity and relocation.
- `88730937`: render replay refuses SIO/CD/MDEC/SPU MMIO reads before their
  uncheckpointed side effects and suppresses I_STAT's SIO tick. The real
  transaction fixture verifies rollback/recovery; production memory, replay
  and debug translation units compile. This is separate from correcting Ape's
  unsuitable broad draw boundary, which reached a real CD command/poll loop.
- `04f871f0`: generation- and address-guarded packet precision capture/restore,
  including derived coordinate flags, projection and absent shadows. The new
  fixture and existing PGXP suite pass. Ape's smaller draw span uses this to
  preserve recorded prefix/tail packets only when packet ownership and link
  layout remain compatible; that title candidate still needs gameplay checks.

Vigilante 8's earlier Windows candidate was staged twice byte-identically:
`Vigilante8PSXRecomp-qualified-candidate-windows-x64.zip`, SHA-256
`060d8ef2195ca43162564870675b6b15ef2e49147d758e7cebf228cb62b08558`.
It excludes retail disc/BIOS and player saves, retains audited native AOT pairs,
and passes execution identity/catalog/ZIP checks. It predates the shader
batching comparison and is not final parity acceptance. No Linux candidate or
measured V8 load-speed gain is claimed.

The shader-only V8 comparison did eliminate filter-change splits and reduced
CPU flush time, but it did not establish a GPU/FPS improvement. Even the first
window's similar primitive counts hid different textured fractions; later
windows and camera images diverged. Host precision/mod-renderer state around
the saved checkpoint needs investigation. The earlier b0/d8 title candidate
remains qualified, with the 7c executable and observations retained separately.
The subsequent fixed-work renderer probe establishes the batching mechanism's
benefit, but does not replace a comparable title performance sample.

The probe compiled the exact b0 and 7c renderer sources at O3 and interleaved
baseline/candidate/candidate/baseline runs on an RTX 3080 Ti. Each scenario
draws the same 800 textured triangles at 5x with 32:9 margins, with 32 warm-up
and 64 timed frames. GPU queries exclude shader compilation; canonical and
wide output hashes match in every run. No game or compiler was active; the
receipt also records background host activity.

| Fixed workload | Baseline GPU ms | Batched GPU ms | Batches before / after |
|---|---:|---:|---:|
| All tracked stable filtering | 0.491272 | 0.505192 | 1 / 1 |
| Stable world plus 20% untracked nearest | 10.165968 | 0.423384 | 320 / 1 |
| Alternating nearest, bilinear and stable | 24.188984 | 0.359664 | 800 / 1 |
| All stable with mask checking | 0.895328 | 0.929272 | 1 / 1 |

These synthetic medians show large savings when filter changes split otherwise
compatible work, with small costs for homogeneous work. They do not establish
V8 FPS or negate its unmatched gameplay samples. Reproduction inputs and
receipts are private under `_build-parity-render-perf-20261006/`:
`prepare_filter_benchmark.py`, `run_filter_benchmark.py`, and
`filter-fixed-work/{comparison,host-inventory}.json`.

Packet precision now also has an explicit relocation operation,
`pgxp_restore_relocated_word_shadow`. It retains checkpoint-entry generation
and packed-value validation, preserves projection/derived flags, journals the
destination, and leaves the source unchanged. An absent receipt clears the
destination's shadow. The same-address API stays strict. The caller must own
and validate both packet ranges and the relocation mapping, write the guest
word and supply its current value; this service never reads device memory.
Focused relocation/rollback cases and the existing PGXP suite pass (2/2).
Ape needs this because real clipping changes core packet counts: its captured
tail has 25 packets and two ordering-table head insertions. Relocation does
not by itself qualify that game's interpolation.

Shared regular-file identity hashing now uses Windows BCrypt or available
Linux OpenSSL, with the portable SHA-256 implementation as fallback. Failed
native initialization, update or finalization restarts the entire file;
I/O failure leaves the caller's digest untouched. The existing C SHA context
and CHD decoded-sector fingerprint path are unchanged. This host utility
preserves exact identity in both execution profiles; it is not a guest HLE
selection or a shortcut that skips content verification.

The Windows native/portable suites, mod runtime, resident-loading and PGXP
session CTests pass (5/5). Linux OpenSSL and portable builds pass 68 and 67
checks respectively, including Unicode paths, SHA block/read-chunk boundaries,
known vectors, partially consumed native failures and unreadable inputs.
An isolated interleaved six-sample Windows measurement on V8's 257,200,608-byte
data track matches Python's SHA-256 in every sample: portable median 0.656533 s,
native median 0.130985 s (5.012x for hashing). Receipts are private under
`_build-parity-render-perf-20261006/{validate_hash,linux-hash-validation,benchmark_hash}.json`.
This saves about half a second for that operation, not the entire multi-second
launch delay observed on the busy host. Overall startup still needs attribution.

## Historical title preparation milestones

Tomba! source preparation is local commit `a46ca8ab` on
`feat/tomba1-parity-hle-20261006`, retaining the newer resident kit at
`3c8d36e`. It adds ENHANCED/REFERENCE resident selection, bounded queue
telemetry, OpenGL/1080p defaults and executable-bound staging. Metadata and
six decoder tests pass. The existing presentation still blends images;
native interpolation, current-build loader contracts, view coverage and
both platform packages remain outstanding. Its `docs/PARITY_HLE.md` records
the actual loader/draw candidates and capture route.

Mega Man X6 source preparation is local commit `5c991f60` on
`campaign/mmx6-parity-hle-20261006`, retaining resident/native-rendering
base `3bf0205`. It adds ENHANCED/REFERENCE resident selection, 1080p and
display-refresh interpolation defaults, and final-byte package binding.
Three configuration tests and script syntax checks pass. Its archives are
uncompressed; the native work accelerates loading rather than decompression.
Historical native interpolation and adaptive 32:9 evidence were separate:
combined current-build coverage, actual caller results, audio/transitions,
fresh AOT and Windows/Linux artifacts still need qualification. See the
title's `docs/PARITY_QUALIFICATION.md` and `BLOCKING_LOADER_CONTRACT.md`.
