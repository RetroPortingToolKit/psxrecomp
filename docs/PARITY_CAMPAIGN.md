# PlayStation enhancement parity

Owner-approved scope, 2026-10-06. Tracking: `beads-eio.3.277`; shared execution
work: `beads-wuhs`. Implementation is in progress. Source presence, a successful
build, a short gameplay check, and owner acceptance are distinct milestones.

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

## Starting inventory and remaining work

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

The owner authorized one Sol worker per title in bounded waves. First wave:
Ape Escape, Spider-Man 2, and Vigilante 8. Shared changes are owned by the
coordinator. At most two compile jobs and one game process run at a time;
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

The campaign framework checkpoint is `04f871f0`. It also includes:

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
