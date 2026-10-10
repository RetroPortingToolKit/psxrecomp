# PSXRecomp — Enhancement-Tier Work (framework-wide)

Faithfulness is the foundation (CLAUDE.md Rule -1); this file tracks the
enhancement layer built on top of it: renderers beyond the software reference,
widescreen, load acceleration, etc. Per-game enhancement ideas live in each
game repo's ENHANCEMENTS.md. Framework bugs referenced here are tracked in
Beads (epic `beads-eio.3`); older ones are still written up in the game repos'
ISSUES.md where they were first found.

---

## R1 — OpenGL renderer (2nd backend): PLAYABLE, flicker root-caused + fixed

**Status as of 2026-07-03** (branch `feat/renderer-finish`, working tree, uncommitted):

- The long-standing intermittent black-frame flicker (MegaManX6Recomp ISSUES.md
  #7 — the reason MMX6 shipped with the software renderer) is **root-caused and
  fixed**. Mechanism (proven via the new present ring + gl_coh_ring correlation):
  `flush_cpu_upload()` merged all pending CPU→VRAM writes into ONE union bounding
  box; a frame with two disjoint uploads produced a union spanning the display
  framebuffers, which the flush painted from the **stale CPU VRAM mirror** (the
  FBO is authoritative under GL) — stomping live frames with black. Two black
  presents per incident (one per double-buffer parity). Software renderer is
  immune (CPU array is authoritative there), which is why it was the safe default.
- Fix: exact pending-rect list (16 rects; merge only when zero uncovered pixels
  are added; wrap-aware GP0(A0) transfers split into up to 4 exact rects;
  overflow → order-preserving flush-all). Merge rule proven by a 20k-randomized-
  rect host unit test (0 stale / 0 missing painted pixels).
- New always-on observability: **gl_present_ring** (every SwapWindow site records
  path taken, src/letterbox rects, glGetError, wall-ms, backbuffer + blit-source
  pixel samples) — the instrument that made the 1:1 black-capture correlation
  possible. Plus a debug-server fix: send_fmt silently truncated >64KB responses
  into unparseable JSON (broke big ring dumps); now heap-formats exactly.
- Validation: ~18-minute MMX6 GL attract soak, ~1600 window captures across 3+
  full attract cycles — **zero isolated black frames, zero GL errors**, no other
  visual anomalies. Tomba1 build-gl rebuilt with the fix, boots clean (full
  Tomba1 attract soak still owed).

**Validation COMPLETE (2026-07-03, both titles):**
- MMX6: ~18-min attract soak, ~1600 window captures, zero isolated black
  frames (agent, ring-correlated).
- Tomba1: 24-iteration/720-capture attract soak (2 flags, both multi-frame
  FMV content cuts) + the definitive pass: 23,807 CONSECUTIVE presents
  (gapless, seq-verified) over a full attract cycle via gl_present_ring —
  ZERO isolated dark presents, zero GL errors. The capture-level flags do
  not exist at the swap level.

**Remaining to close R1:**
1. USER final validation at the MMX6 Rainy Turtloid standing-still spot (the
   original repro; MMX6 build-modern settings.toml is left on
   renderer="opengl").
2. Flip MMX6's shipping default software→opengl + close ISSUES.md #7.
3. The same union-upload bug exists in the Vulkan backend (see R2 item 1).

## R1b — Native-wide (16:9) GL: perf collapse ROOT-CAUSED + FIXED, band flicker FIXED

**Status 2026-07-03** (branch `feat/renderer-finish`, commits f5362f4..8b819eb):

- **The 16:9 perf collapse (Tomba2 3D attract 60→12fps, MMX6 2D attract dips)
  was never a GPU problem.** Stack-sampled (devkitPro gdb) in the wedge: the
  main thread lived in `ws_backdrop_site_kind` ← `exec_one` — under native-wide
  the dirty-RAM interpreter classified EVERY executed instruction as a possible
  backdrop rewrite site; the classifier rescans ±512 bytes on cache miss and its
  256-slot direct-mapped cache (2 hot PCs 1 KB apart collide) thrashed on
  overlay working sets. Squash mode gates the whole path off — that is why
  `ws_nw on=0` restored 60fps while every GPU theory (mirror FBO ping-pong,
  extra prims, present path) failed. The earlier "60ms scene GPU / 70us per
  prim" numbers were CPU-starvation-inflated GPU-timestamp gaps (the GPU idles
  between CPU-paced submissions inside the bracket) — treat GL timer numbers
  on a CPU-bound frame as suspect.
- **Fixes** (dbe7812 + 8b819eb): opcode pre-filter (only addu/or/addi/addiu can
  be rewrite sites) + 8192-slot full-PC-tagged caches + `g_dirty_ram_code_gen`
  invalidation (memory.c) + a per-entry SITE-WORD tag (revalidates the cached
  verdict against the live instruction word — plain-CPU-store overlay reloads
  never hit the page-marking hooks, and a stale verdict fires a GPR rewrite at
  the wrong instruction = guest corruption).
- **Numbers**: Tomba2 GL 16:9 attract (heavy scene, ~640-1100 prims) 72-95ms/frame
  → **17-21ms (p50 17.1ms, ~52-58fps)**; MMX6 GL 16:9 attract locked 16.7ms
  through demo stages (worst 10s window avg 25ms at stage-load transitions).
- **Top/bottom band flicker (MMX6 16:9, user-visible) FIXED** (a0b5843): the GL
  mirror pass scissored the FULL wide surface; the SW reference (`rt_wide`)
  only widens X and keeps the draw-area Y clip. Under MMX6's vertical double
  buffer (draw area alternates y=0/y=240, both bands in ONE wide surface)
  mirror draws bled across the band boundary and presented a frame late as
  edge flicker. Scissor is now full-width X / draw-area Y. Validated with
  0.15s-interval capture bursts: top/bottom 16-row inter-frame instability is
  0.26x/0.21x the scrolling middle band (edges quieter than content).
- **Wide surfaces now carry a DEPTH24_STENCIL8 attachment** (cfa79bb): the
  stencil-less wide FBO was a spec-gray target for the stencil-enabled mask
  fixup passes AND left the PSX mask-bit mirror silently no-op on the wide
  surface. (Its per-pass GPU "cost" measurements that motivated it were later
  shown starvation-inflated; the attachment stays for mask correctness.)
- **New permanent instrumentation**: frame_perf mirror split (GL_TIMESTAMP
  pairs per wide pass: mirror_gpu_ms / canon_gpu_ms / mirror_pass_us), CPU-side
  attribution (cpu_flush_ms, cpu_wide_ms, batches, wide target sets, wide FBO
  creations per frame), and `gl_ws_ablate mode=0..3` (skip mirror / state-only /
  no-FBO-rebind ablations) — the toolchain that exonerated the GPU and named
  the CPU producer.
- **MMX6 wedge incident (RESOLVED to one mechanism, poisoned overlay shards):**
  during the session MMX6 hit fatal wedges — twice in the 16:9 attract (wild
  dispatch 0x21010001 / 0x0C008096) and then DETERMINISTICALLY at boot frame
  2518 (unknown dispatch 0x80095098) at BOTH aspects. Timeline nailed it: the
  overlay self-heal wrote a fresh 001EA000 shard batch at 20:53, exactly when
  attract wedge #1 hit (shards hot-loaded mid-run); every boot after loads
  them and wedges at 2518; quarantining the batch
  (cache/.../cg4_0cec55ab.quarantine) restored clean boots + attract. Most
  plausible poison source: the INTERIM stale-verdict build (dbe7812 before the
  8b819eb word tag) corrupted guest RAM at 16:9, and autocapture snapshotted
  the corrupted overlay bytes into the captures the shards were compiled from.
  Self-heal recaptures with the hardened build. NOT a ws-stack or renderer
  defect. Tomba2 16:9 soaked clean throughout.

## R2 — Vulkan renderer (3rd backend): RENDERS GAMEPLAY AT SPEED, gaps cataloged

**Status as of 2026-07-03** (same branch/tree; `-DPSX_ENABLE_VULKAN=ON`, SDK
1.4.341.1; `feat/vulkan-renderer` turned out to be already merged into master —
only a 26-line build-guard needed salvaging from the retired _wt-vulkan worktree):

- Three bring-up bugs root-caused and fixed this session:
  1. **Boot wedge**: per-pixel GP0 uploads did 2 vkAllocateMemory + 2
     vkQueueSubmit each (driver churn → minutes-long stall, watchdog abort).
     Fixed with GL-style deferred batched uploads.
  2. **Shredded 3D**: draws raced CPU rewrites of the persistently-mapped vertex
     buffer (69.5% pixel divergence vs software at the same guest frame). Fixed
     with sub-allocation cursors + firstVertex bases.
  3. **Semi-transparency order violations** (59.5% divergence): VK still had
     GL's retired whole-batch STP split; ported GL's current two-pass model
     (ordered color pass + color-masked stencil fixup; semi prims isolated).
- Verified (guest-frame-aligned VRAM diffs via the new frameshot.py tool +
  window captures): title pixel-identical to software; attract within 0.62% of
  the GL oracle; **60.5 fps sustained**; vk_perf steady-state ~0-2 allocs and
  ≤6 submits/frame (was thousands). 24-bit FMV present path written (old one
  was provably black) but NOT yet verified on-screen.

**Gap catalog (ranked, low→high effort):**
1. ~~Port the exact-rect pending-upload fix from GL~~ DONE 2026-07-03: exact-rect
   list ported + VK-specific COALESCED flush (one staging pair + two submits per
   flush regardless of rect count; the naive per-rect port re-created the submit
   churn at 0.7 fps — VK pays per-submit where GL pays per-glTexSubImage2D).
   UP_RECTS_MAX=64 on VK so MDEC row-coalesced FMV frames fit in one flush.
   Verified: attract renders correctly at ~51 fps, vk_perf mostly-idle frames.
2. ~~Verify FMV on-screen~~ DONE 2026-07-03: Whoopee logo + intro CG movie render
   correctly on VK (window captures). NOTE Tomba2 movies are 15-bit MDEC->VRAM
   (upload path), NOT depth24 — the depth24 compose path remains no-regression-
   verified only; validate on a 24-bit title (MMX6 opening) later.
3. ~~Cache the FMV present staging image~~ DONE 2026-07-03: persistent image +
   mapped staging keyed by (w,h), freed on resize/shutdown (cpres cache).
4. ~~DS barrier~~ DONE 2026-07-03: explicit stencil-aspect self-barrier
   (late-tests write -> early-tests read|write) at every begin_geo_pass;
   layouts verified against the init transition chain + render pass
   (attachment-optimal throughout).
4b. NEW (minor): flush_cpu_upload allocates 2 stagings per flush — ~16/frame
   during MDEC FMV streaming only (~0 in gameplay). A sync-aware staging ring
   would zero it; low priority.
5. ~~Native-wide (16:9) compositor~~ DONE 2026-07-03 (0b23ea3): per-base_x wide
   surfaces (RGBA8 color + OWN stencil image + framebuffer on the SHARED render
   pass — every pipeline works unchanged, and the mask-bit stencil mirror is
   real on the wide surface from day one). The mirror is a second render pass
   appended to the SAME one-shot CB as each flushed batch (no extra submits);
   u_xoff/u_xhalf push constants (pre-plumbed in the shaders) carry the
   translation/wider clip. Mirror scissor = full-width X / DRAW-AREA Y (the GL
   band-bleed lesson applied from day one). wide_clear via ClearAttachments
   (color + stencil=bit15); full-screen overlay rects suppress the batch mirror
   and draw one full-wide-width rect (margins dim/fade). GPU-direct present
   blits the displayed band letterboxed at (4*wide_w : 3*native_w);
   vkb_render_wide_display readback backs the facade + debug dump. vk_perf
   gains wide/wclr counters. VALIDATED (Tomba2 build-vk, PSX_WS_FORCE_2D=1 —
   master Tomba2 has no sprite-tag hooks): 16:9 attract presents full-width
   (mine-cart demo captures), locks 60fps on normal scenes (frame_period p50
   16.68ms), ~51-57fps on the heavy semi-prim scene (29 wide passes/frame);
   4:3 unregressed (p50 18.1ms on the 452-flush semi-isolation scene — the
   pre-existing item-7 profile, wide counters 0). Margins show 4:3-culled
   geometry only — full margin content needs the cull-widened overlay cache
   (cg4_0cec55ab.ws-experiment, not installed on master).
6. SSAA scale >1 unvalidated on VK.
7. Semi-prim isolation perf (one draw per semi triangle; same cost as GL today).

**Validation targets:** MMX6 + Tomba2, agent does initial (window-capture series
+ frame-aligned cross-backend diffs), user does final.

---

## R3 — Validation sweep + merge (2026-07-03, USER-DRIVEN)

Full 3-title x 3-config playthrough validation (agent launched each config windowed
+ confirmed on-screen via window_capture; user drove input + gave the verdict):

| Title  | OpenGL 4:3 | Vulkan 4:3 | OpenGL 16:9 |
|--------|:----------:|:----------:|:-----------:|
| Tomba1 | PASS       | YELLOW     | PASS        |
| MMX6   | PASS       | YELLOW     | YELLOW      |
| Tomba2 | PASS*      | PASS*      | FAIL*       |

\* Tomba2 = experimental / not production-ready; not merge-blocking. (Vulkan 16:9
was intentionally skipped — user directive: all 16:9 in OpenGL only.)

Findings (tracked for the next work cycle):
- **OpenGL 4:3 is the strong, shippable path on all titles.** This is the win.
- **Vulkan 4:3 = YELLOW everywhere** (why it ships hidden/experimental):
  - Tomba2: sluggish FMV, minor speech-audio fidelity loss, in-game slowdown
    (possibly progressive), text boxes don't dismiss, HUD weapon icon renders in
    all 3 slots (only center should show).
  - Tomba1: minor horizontal bar artifact at top of the load screen; dwarf
    village sluggish (the known overlay-heavy scene; perf only, renders correct).
  - MMX6: significant slowdown in Rainy Turtloid's RAIN AREA even at 4:3 —
    VULKAN-SPECIFIC (on GL 4:3 it is subtle at most). Renders correct; perf only.
- **OpenGL 16:9:** Tomba1 clean PASS. MMX6 YELLOW (rain-area lag evident at 16:9;
  same Rainy Turtloid perf hot-spot). Tomba2 FAIL — widescreen does NOT engage
  (renders 4:3 pillarboxed in the wide window + slow); expected, master Tomba2
  has no sprite-tag ws hooks (ws work parked at 4:3).

**Policy gates confirmed already in-tree (no code change needed for the merge):**
- Vulkan is hidden by default: the shared launcher offers only
  Software<->OpenGL; PSX_ENABLE_VULKAN defaults OFF (runtime.cmake);
  runtime downgrades renderer=vulkan -> opengl when not compiled (main.cpp:2463).
  Vulkan stays a dev/CLI-only backend (--renderer vulkan on a VK-enabled build).
- Widescreen carries an EXPERIMENTAL tag in the launcher.

**MERGED to master 2026-07-03** (runtime-only; codegen hash unchanged, no regen).
Game pins bumped to the new psxrecomp master. No release builds cut (user directive).

**Open follow-ups (next cycle):** VK perf (MMX6 rain-area, dwarf village, Tomba2
in-game — likely the CPU-bound dirty-RAM ws classifier path and/or VK per-submit
cost); VK correctness (Tomba2 persistent text boxes + 3-slot HUD icon; Tomba1 load
bar artifact); Tomba2 widescreen sprite-tag hooks; MMX6 Rainy Turtloid perf even on
GL 16:9; and the pending MMX6 shipping-default flip software->opengl to close
MegaManX6Recomp ISSUES.md #7 (GL validated this session).

---

## W1 — Tomba2 16:9: USER-FLAGGED visible issues queue (2026-07-06, NOT STARTED)

User-prioritized alongside the P0 crash work (ISSUES.md #8). None of these have
been investigated yet — this section is the work queue + every known lead.
All Tomba2, GL 16:9, worktree `_wt-tomba2-ipr` / `Tomba2Recomp` build-t2.

### W1.1 — Black background columns in 16:9 (P3; incl. beach area)

- **Symptom:** huge black regions where the 2D far backdrop should fill the wide
  margins (user Image 1: large black sky scene); also a ~24px black column in
  beach-village at game-x 85–109.
- **Class:** 2D far-backdrop doesn't cover the wide FBO edges.
- **Prior art / leads:**
  - `[widescreen.bg2d]` config (recompiler/src/config_loader.cpp:699).
  - Memory `ws_backdrop_preload.md` (Tomba1): far-backdrop void = early
    sprite-tagged 0x65 tile grid; fix = centre-stretch gated preload.
  - Memory `ws_draw_census_8c.md` (Tomba1 8C scene): void was GTE-3D driver
    FUN_8004db3c; fixed via depth-gated un-squash. The **8C draw-census ring**
    is the attribution instrument (always-on; query, don't arm).
- **Next step:** per-scene attribution — which producer draws the sky strips,
  and why the wide margins get no tiles. Attract may cover some scenes; the
  beach needs navigation (user drive or save).

### W1.2 — 4:3 object culling visible at wide edges (P2 cull widening)

- **Symptom:** objects pop in/out at the 4:3 boundary in 16:9 (world objects
  culled by game code against 4:3 screen extents).
- **Ghidra cull sites already identified** (tomba2_ram.bin; overlay addresses —
  validate per scene variant before wiring):
  - `FUN_8003e030`: sltiu 0x140 @ 0x8003E228
  - `FUN_80069b6c`: addiu +0xE6 @ 0x80069B84 + sltiu 0x1CD @ 0x80069B8C
  - `0x80110A08`: addiu +0x80 / sltiu 0x101
- **Fix shape:** `[widescreen.cull]` bias_sites/range_sites per-game config
  (enhancement-tier per-game shims are legitimate here). Prior art: Ape
  bring-up used per-game cull imms (0x181) + signed idioms (slti/bltz)
  (memory `ape_widescreen_bringup.md`).

### W1.3 — Dialogue-box text tearing in 16:9 (P4)

- **Symptom:** full-width dialogue text splits with gaps (user Image 2: "Water
  cam…e out from th…e faucet").
- **Cause known:** gpu.c `ws_nw_hud` thirds — left/right-third sprites are
  anchored apart for HUD proportion; full-width dialogue glyphs tear at the
  third boundaries. Needs a smarter anchor (e.g. detect wide text rows /
  dialogue-box association) rather than blanket thirds.

### W1.4 — Beach-area framerate (P1 perf; also user-flagged)

- 2D isometric scenes run 0.42–0.70× (beach/village worst). The convergence
  blockers are FIXED (autocapture futility backoff + entry-based coverage in
  tools/compile_overlays.py — see handoff 2026-07-06); the campaign was
  interrupted by ISSUES.md #8 and should resume after it: expect 2D scenes to
  climb past 0.85 as coverage converges. Residual axes if short of ~1.0:
  per-block pump overhead (psx_check_interrupts ~2.6M calls/s), attract
  cycling. Measure ONLY with two freeze_check snapshots over a known wall
  interval (executed throughput = d(psx_cycle_count) − d(cycles_skipped));
  phase_profile is a ring read and returns instantly.
- CAUTION: any pace numbers taken in diff mode before ISSUES.md #9's redesign
  lands are garbage (the wedged shadow silently disabled all native dispatch).

## L1 — Load-time-toward-0 burndown (2026-07-14, ACTIVE — guinea pig: Tomba 1)

Full analysis + gates/kill criteria: `docs/LOAD_TIME_ZERO.md` (this branch,
`spike/load-time-zero`). ChatGPT consult merged (thread "PSXrecomp
workspace"). Already-settled items are NOT re-tried: turbo_loads (shipped,
~2x), disc_speed divisor 4x/instant (proven unsafe — MMX6 VSync-callback
wedge), yield pumps r1/r2 (proven fatal — green-thread corruption), BIOS CD
HLE (rejected — no landmine, no host win). The frame:
`wall = guest_time x host_cost_per_guest_sec`; under turbo the window is
emulation-throughput-bound, so the safe axis is host cost (multiplies with
turbo), the risky axis is guest time.

**Prioritized burndown (most agnostic + most likely beneficial first):**

- [x] **L1.0 — E0 `load_probe_v2`: load-window decomposition on Tomba 1.**
  100% agnostic, zero risk, prices every bet below. Split a real pig-load
  window into guest time (seek / sector cadence / per-sector processing /
  explicit waits) and host time (native code / decompressors / interp /
  CD-event machinery / SPU / GPU / pump). Existing rings first
  (freeze_check, cdrom_bursts, dirty_ram_stats per_pc, phase_profile);
  extend rings only where attribution is blind. Decisive question: why
  only ~2x during a presentation-suppressed window? Thresholds: decomp
  ≥~40% host → shards serious; ≤~8% → kill shards; CD/event/SPU machinery
  dominates → L1.2; wall-clock limiter found → fix that first.
- [ ] **L1.1 — Turbo hardening.** (a) re-validate SDL pump under a live
  burst (fix appears in-tree: pump precedes the turbo early-return);
  (b) audio at the HOST SINK only (drop excess samples, crossfade on
  exit; never guest state); (c) root-cause MMX5 dev-tools+turbo 0xE10
  boot wedge (foundation timing bug).
- [x] **L1.2 — Event-horizon acceleration + batched device ticking.**
  Provably side-effect-free poll/idle regions jump to the next scheduled
  observable event with exact cycle credit + identical event ordering;
  devices advance to deadlines instead of per-block ticks. Attacks the
  ~2x ceiling directly; class-level, all titles inherit. Gate set from
  L1.0's poll/idle share. Shipped: deadline-based device servicing, six Tomba
  wait sites, and proof-gated generic idle skipping. Default-off cross-game
  smoke validation passed on MMX5 and MMX6. Kill: <10% gain or ONE event-order
  divergence.
- [x] **L1.3 — Load-path overlay coverage (killed by gate).** L1.0 measured
  zero in-window interpreter instructions, so there is no coverage win to buy.
- [x] **L1.4 — Data shards (rejected/quarantined): verify-only SHADOW
  mode, then replay.** Gated on L1.0 (decomp ≥~20-25% host share).
  Correctness bar: temporal write visibility — replay sound only if
  IRQs-off across the window OR duration < next observable event.
- [x] **L1.5 — Authentic drive backlog (killed as acceleration).** Passive
  deadline-vs-exposure probe measured 1,304/1,304 data sectors available on
  their exact scheduled cycle, then the intentional fixed 5,000-cycle INT1
  presentation delay. Zero early/late sectors, holds, pending/lost INT1s, or
  overwrites: there is no artificial lateness for backlog/catch-up to remove.
- [x] **L1.6 — Seek-only latency probe (killed on Tomba).** The measured
  New Game window issued zero seek commands. Read-start latency was only
  7,676,928 / 298,130,657 cycles (2.57%); pause latency was 8.11% but is an
  authentic CPU-visible ordering contract, not a safe seek-speedup target.
- [ ] **L1.7 — Phase-2 doors (open only with cause):** per-title read
  speedup with XA/CDDA/MDEC exclusions; decompressor HLE (only via L1.4
  failing for a named reason); load-transition state cache (the only
  true near-zero; needs thousands-of-frames differential validation).

**Live status / decision ledger (Tomba 1, 2026-07-14):**

| Item | Verdict | Evidence / measured result |
|---|---|---|
| L1.0 decomposition | DONE | 761 sectors, ~9.5 s baseline window; zero in-window interp; host/static execution dominated. |
| L1.1 turbo hardening | IMPLEMENTED ON TOMBA; CROSS-GAME SMOKE PASSED | SDL pump remains before every turbo return; 4-frame engage + 6-frame release debounce passed live QA. Opt-in host-audio sink advances canonical SPU state while discarding only accelerated host output; Tomba listening QA passed after 1,100,752 discarded SPU frames. MMX5 and MMX6 debug-tools builds booted through loads into live gameplay with normal visuals/audio; the historic intermittent MMX5 0xE10 root cause remains a separate long-run investigation. |
| L1.2 event horizon | IMPLEMENTED; CROSS-GAME SMOKE PASSED | Production cycle advancement already batches device service at event/MMIO deadlines. Six configured PsyQ CD-wait sites delivered ~27% + ~9% stages. Generic idle-loop skip then cut warm bursts 0.53->0.34 s and 2.19->1.73 s, with 4,599 skips / 765M guest cycles and zero CD overwrites. Strictly per-game opt-in; MMX5/MMX6 exercised the default-off compatibility path successfully. |
| L1.3 overlay coverage | KILLED | Interpreter share was zero in the measured load window. |
| L1.4 asset/data replay | REJECTED FOR NOW | `FUN_8003EF50` replay produced title/game texture corruption despite zero verifier failures: v1 temporal verifier is unsound. Data shards default off and artifacts removed. |
| Configurable warm CD routes (L1.7 read-speed branch) | ACCEPTED, STRICTLY PER-GAME OPT-IN | Framework accepts up to 16 strict LBA routes with mismatch fallback and consumer-paced IRQ/DMA. Only data-read cadence accelerates; XA/CDDA, seek, and motor timing remain authentic. Tomba multi-route regression: 3 matches, 1,944 accelerated sectors, zero overwrites. Legacy singular config is deprecated. |
| L1.5 authentic backlog | DONE / KILLED AS ACCELERATION | 1,304/1,304 sectors exact-deadline; INT1 exposure exactly +5,000 cycles; zero holds, pending/lost, or overwrites. |
| L1.6 seek-only probe | DONE / KILLED ON TOMBA | Automated New Game: 0 seeks across 792 data sectors. Read-start latency was 2.57% of the data span, below the 10% gate. Pause was 8.11% but remains authentic because early completion is a known race/wedge class. |
| L1.7 state cache / broader HLE | DEFERRED | User excludes savestates; decompressor replay failed correctness. |

Method, every experiment: measure first via always-on rings; start flag-gated;
one per session; kill criterion written before code. `idle_skip`, warm CD routes,
and the turbo host-audio sink are all strictly per-game opt-in. MMX6 remains the
next deeper corpus/soak target after its successful boot/load/gameplay smoke.

---

## W2 — Tomba 1 (SCUS-94236) 16:9: HUD at the true wide corners (DEFERRED 2026-07-10)

User ask: in native-wide 16:9, re-anchor the HUD to the wide corners
(repositioned, never stretched). First attempt shipped and was REVERTED the
same day (game.toml `nw_hud_corners` back to false; the framework machinery
stays, inert + A/B-able). What was learned, so the next attempt starts ahead:

- **Mechanism reused:** `[widescreen] nw_hud_corners` (gpu.c `ws_nw_hud_shift`
  thirds translate, from the MMX4/5 campaign) + a new tag-title scoping: polys
  and lines never shift (world/characters), rect-family prims shift only when
  UNTAGGED. TCP `ws_hud_mode {"tag_rects":0|1}` A/Bs the tagged-rect gate live.
- **What worked:** vitality gauge + life-counter (untagged rects, fully inside
  one outer third) anchored flush to the wide corners, world untouched.
- **Failure 1 — composite tear (same class as W1.3):** the in-world dialogue
  box ("It's locked...") is a composite of untagged rects SPANNING zone
  boundaries: its left/right end caps sit in the outer thirds and get pulled
  to opposite screen edges while the text stays centred — the box visibly
  splits. A blanket per-prim thirds rule cannot ship; it needs composite-group
  awareness (e.g. group prims drawn adjacently in the packet arena / same OT
  bucket, shift a group only if the WHOLE group fits in one zone).
- **Failure 2 — AP counter immobile:** the AP composite (0x65/0x67 rects,
  x≈220-292, y≈8-12) renders through the TAGGED sprite funnel (0x8005E08C),
  so the untagged-only gate skips it. Census now records a `tagged` column
  (gpu.c WsCensusEntry) — verify with `ws_census`. Lifting the gate via
  ws_hud_mode shifts it, but then tagged world-anchored rects (collectible
  sprites) near edges would shift too — needs the same group/HUD-band
  discrimination as Failure 1 or a per-title HUD packet arena range
  (`nw_left_hud_packet` exists for exactly this; needs Tomba's HUD arena
  addresses from a census session: HUD prims live in the 0x000Bxxxx/0x000Cxxxx
  double-buffered packet arenas alongside everything else, so the range must
  come from finer addresses).
- **Groundwork landed on `feat/ws-2d-scene-pillarbox`:** census `tagged`
  column, `ws_hud_mode` live A/B, and the untagged-rect scoping that makes
  `nw_hud_corners` safe to experiment with on tag titles.

---

## G1 — Sub-pixel vertex precision + perspective-correct textures (issue #92)

PS1 polygon jitter ("wobble", "bouncing lines") and warped floor/wall textures
have one root cause each, both in the fixed-point geometry pipeline:

- **Jitter.** The GTE computes the projected screen position in 16.16, then
  saturates it to an integer pixel when it pushes SXY. The fraction is thrown
  away. A slowly moving mesh therefore snaps its vertices between whole pixels
  and the model shimmers.
- **Texture warp.** The GPU interpolates UV *affinely* across a triangle, with
  no 1/z term, so a large floor or wall polygon's texture swims as the camera
  moves.

Both are addressed as **opt-in, visual-only** enhancements. The PS1-visible GTE
SXY FIFO stays integer and fully faithful — a game's own post-projection
screen-bounds culls and any SXY readback see exactly what hardware produces.
Nothing about guest state changes; the correction lives entirely on the host
render path.

### Configuration

```toml
[video]
geometry_correction   = true   # sub-pixel vertex precision (kills the wobble)
perspective_texturing = true   # perspective-correct UVs on world polygons
supersampling         = 2      # REQUIRED for geometry_correction to be visible
```

Both default **false** (the faithful floor). They are independent — a title may
want stable geometry without changing texture mapping. Settable per-game in
`game.toml` and per-player in `settings.toml` (the player's file wins, and a
launcher save round-trips both keys rather than dropping them).

`geometry_correction` needs `supersampling >= 2`: at native resolution the
corrected position rounds back to the pixel it started on, so there is nothing
to see. The runtime prints a note at startup when it is on at scale 1.

### How it works

The recompiler emits GTE commands as calls to a single runtime entry point
(`gte_execute`), which both the compiled backend and the dirty-RAM interpreter
share — so unlike an interpreter/dynarec emulator there is no dispatch hook to
add, just one funnel to instrument.

1. **`runtime/src/gte.cpp`** — RTPS/RTPT keep the discarded 16.16 fraction in a
   side cache keyed by the packed SXY word it rounded to (`geom_note`).
   Saturated (off-screen) projections are rejected: they carry no usable
   sub-pixel information.
2. **SWC2 provenance** — the recompiler, strict translator, dirty-RAM interp,
   overlay ABI (v14) and fallback interp all call
   `gte_precision_store_word(addr, reg)` when a projection register is stored to
   guest RAM, recording *which RAM address* a projection landed at. Perspective
   texturing only fires when all three of a triangle's position words came from
   such a store at that exact DMA packet address — which preserves the
   association through ordering-table reordering and rejects CPU-built UI and
   2D sprites outright. A plain `sw` to a tracked address invalidates it.
3. **`runtime/src/gpu.c`** — `prepare_precise_triangle()` /
   `prepare_texture_triangle()` look the packet up per triangle and hand the
   result to the renderer facade as sideband state for the next draw
   (`gr_set_precise_triangle` / `gr_set_perspective_triangle`).
4. **All three renderers consume it.** Software uses the fractional positions
   in its supersampled mirror; OpenGL and Vulkan take them as float vertex
   positions directly. For perspective UVs both GPU backends carry a per-vertex
   `a_q` weight and emit clip coordinates pre-multiplied by `w = 1/q`, so the
   hardware's own perspective divide interpolates a `smooth` UV varying while
   the affine `noperspective` one stays available. **`a_q == 0` (the default)
   makes `w` exactly 1.0 and selects the affine varying — the pre-feature
   pipeline, unchanged.**

Save states and speculative native-validation passes drop host-only provenance
(`gte_precision_timeline_invalidate`, `gte_precision_speculative_begin/end`) so
a rewind can never resurrect a stale projection.

Perspective-correct world polygons retain their authored UV coordinates and
inclusive atlas bounds. The shared `gpu_uv.h` sampling policy applies mirrored
sprite compensation only to affine primitives. Integer screen rounding can
otherwise make one half of a world quad appear axis-aligned and shift its UVs
by one texel while the other half stays put. This caused diagonal breaks and
camera-dependent flicker in MediEvil II's Museum doorway trim. OpenGL/Vulkan
use the shared policy; software uses the same stable world bounds. The
`gpu_uv_test` fixture covers the captured doorway packets, camera rounding,
shared edges, mirrored/forward sprites and texture-page wrapping.

### Validation story

By construction this feature *diverges* from stock hardware output, so the
Beetle oracle cannot be the judge of the corrected frame. What the oracle still
pins is the part that must not move: **with both flags off the output is
byte-identical to the pre-feature build**, and guest-visible GTE state is
identical either way (the SXY FIFO is untouched in both). That reduces
validation to (a) an off/off pixel-identity check against the oracle, and
(b) human A/B of the on/off frames on a 3D title.

`gte_geometry_correction_hits()` and `gpu_texture_correction_hits()` report how
many vertices/triangles were actually corrected — the "is this doing anything
on this title" counter, and the thing to check first when a title shows no
visible change.

### Provenance

The GTE side cache, SWC2 provenance tracking, GP0 triangle preparation and the
software-renderer consumption path were contributed by **Kareem Olim (kem0x)**
in [PR #14](https://github.com/mstan/psxrecomp/pull/14) and parked in commit
`2ceaf5a` (see `docs/internal/upstream/kem0x-pr14-projection-perspective.md`),
disabled pending generic setters. This work adds the opt-in configuration, the
renderer-facade seam, and OpenGL + Vulkan support.

### Status / next

- **Done:** config plumbing (game.toml + settings.toml + launcher seed
  round-trip), renderer-facade sideband, software / OpenGL / Vulkan consumption,
  `[video]` plumbing unit test.
- **Open:** per-title A/B validation. Ape Escape is the obvious first 3D
  subject (Tomba 1/2 and MMX5/6 are largely 2D, where neither knob does much).
- **Open:** launcher (recomp-ui) toggles. The keys round-trip through
  `settings.toml` today, but there is no UI row yet — that lives in the
  recomp-ui repo.

### G1.1 — MEASURED REGRESSION: partial coverage cracks meshes (2026-08-05)

**User verdict on Ape Escape: "little lines jittering everywhere — visually
this is worse."** Confirmed and root-caused. `geometry_correction` must not be
presented as usable in its current form.

Measured on Ape Escape (OpenGL, supersampling 2, 213-frame window, via the new
`geom_correction` TCP command):

| | per frame |
|---|---|
| GP0 draw commands | ~316 (≈400–600 triangles) |
| vertices given sub-pixel positions | ~114 (≈38 triangles) |
| triangles given perspective UVs | ~1.9 |

**Under 10% of the scene is corrected.** `prepare_precise_triangle` is
all-or-nothing per triangle, so every boundary between a corrected triangle and
an uncorrected neighbour is a seam: one edge moved sub-pixel, the other stayed
on the integer grid. Because the geometry cache is direct-mapped and keyed on
the *rounded* position, which triangles win changes frame to frame — so the
seams move. That is exactly the reported jitter.

### G1.2 — What the reference implementations actually do

Both vendored emulators (`beetle-psx/pgxp/`, `duckstation/src/core/cpu_pgxp.cpp`)
implement PGXP the same way, and it is **not** what is parked here:

1. **A complete shadow of the dataflow.** A `PGXP_value {x,y,z,flags,value}` per
   32-bit word of RAM + scratchpad (Beetle mirrors all 2 MB; DuckStation the
   same), plus a shadow per CPU GPR and per GTE register.
2. **Propagation through every instruction.** Beetle registers **47 CPU hooks** —
   LW/LH/LB/LWL/LWR, SW/SH/SB/SWL/SWR, ADD(I)(U)/SUB(U)/AND/OR/XOR/NOR/SLT(U),
   SLL/SRL/SRA(+V), MULT(U)/DIV(U), MFHI/MTHI/MFLO/MTLO, LUI. DuckStation carries
   the same set. High precision therefore survives any route the game takes from
   GTE output to the GP0 packet.
3. **`Validate(value)`.** Every shadow read checks the tracked `value` against
   the *actual* current word and drops the shadow on mismatch. This is what stops
   stale precision from corrupting geometry.
4. **The value-keyed cache is only a LAST-RESORT FALLBACK**, and even then it is
   fully direct-indexed — Beetle `vertexCache[0x800*2][0x800*2]`, DuckStation
   2048×2048 — so distinct screen positions **never collide**; it is gated on an
   ambiguity flag (`gFlags == 1`, "only one value was recorded at this position")
   and it disables perspective (`valid_w = 0`) because its w is untrustworthy.

**The parked implementation is only item 4, degraded**: an 8192-entry *hashed*
table (unrelated positions collide) with *no* ambiguity check and *no* primary
path. A vertex can therefore inherit a different vertex's fraction. That is the
design defect, not a wiring bug.

### G1.3 — What matching them costs in a static recompiler

The emit mechanism already exists — `gte_precision_store_word(addr, reg)` is
emitted at SWC2 sites today — so this is an extension, not new machinery:

- per-word shadow of RAM + scratchpad (~12 MB), per-GPR and per-GTE-reg shadows;
- propagation hooks at ~47 instruction classes, emitted in `code_generator.cpp`
  and `strict_translator.cpp`, mirrored in `dirty_ram_interp.c` and
  `psx_interpreter.c`, and forwarded through the overlay ABI (another bump);
- `Validate()` on every shadow read;
- GPU-side lookup keyed on the packet word with a `value ==` check, with the
  corrected fallback cache last.

**The static-recompiler-specific cost:** in an interpreter these hooks are a
runtime branch. Here they are emitted C on the hot path, so they bloat generated
code and cost speed *even with the feature off* unless they are gated at
CODEGEN time — i.e. a separate generated flavour, which touches the build matrix
and every title's regen. That is the real decision, and it is why this cannot be
a runtime-only toggle like the rest of the `[video]` block.

### G1.4 — DECISIVE: position-keyed lookup cannot work (measured, 2026-08-05)

The cheap fix was tried and **measured to be insufficient**, which settles the
direction. The hashed table was replaced with the references' exact
direct-indexed one (one slot per reachable SXY, no collisions) plus their
ambiguity gate. Ape Escape attract demo, cumulative:

| outcome | count | share |
|---|---|---|
| lookups attempted | 4,860,057 | — |
| **hit** | 253,679 | **5.2%** |
| miss — never recorded at that position | 82,253 | **1.7%** |
| miss — **ambiguous** (several DIFFERENT projections rounded to that pixel) | 4,524,125 | **93.1%** |

**93% ambiguous, 1.7% unrecorded.** The tracking is not failing to *reach* the
vertices — it reaches almost all of them. The rounded screen position simply is
not a unique key: in a dense 3D scene most pixels have several distinct vertices
projecting onto them, so no position-keyed lookup can tell which sub-pixel
fraction belongs to the packet being drawn. That share is irreducible; a bigger,
faster or smarter table cannot move it.

It also explains why the first attempt looked *so* bad. Without the ambiguity
gate those 93% were not misses — they were silently answered with **another
vertex's fraction**. The gate makes the feature safe (wrong fractions are no
longer applied) but drops honest coverage to ~5%, so meshes still mix corrected
and uncorrected triangles and still crack. Visually confirmed on Ape Escape:
thin dark seams tracking across characters and floor in the intro/attract.

**Conclusion.** Only PGXP's primary path — precision travelling *with the data*
through the CPU dataflow, so a GP0 word's provenance is known exactly rather
than guessed from where it landed — produces a clean result. A coverage gate or
a better cache is ruled out by measurement, not by argument. `geometry_correction`
must not ship until value propagation exists.

### G1.5 — CORRECTION to G1.4: the ambiguity gate fixed the cracking

**G1.4's closing claim ("meshes still mix ... and still crack, visually
confirmed") was wrong, and was not verified.** User observation on the exact-
table + ambiguity-gate build, at window resolution: *"in game is looking pretty
nice ... whatever is up doesn't have the lines issue."* Same scene that was
visibly torn before. The seams are gone.

**Why the wrong claim was made — the instrument was blind.** Verification used
the TCP `screenshot`, which resolves native 15-bit VRAM. Geometry correction
exists ONLY in the supersampled mirror (that is why it needs `supersampling
>= 2`), so it is erased before a native capture is taken. Those captures showed
clean frames no matter what the player saw, and the conclusion then leaned on
counters instead. Fixed by adding `screenshot_hires`, which routes the same
present path as the window. **Anything that lives in the hi-res mirror must be
verified with `screenshot_hires`; a native screenshot cannot see it.**

**What this means technically.** The visible defect was *ambiguity*, not
coverage:

- A wrongly-attributed fraction displaces a vertex by up to a full pixel in an
  arbitrary direction — a large, obvious tear that moves as winners change.
- Missing coverage only leaves a sub-pixel (<1px) mismatch where a corrected
  triangle meets an uncorrected one — not visually objectionable.

So the ambiguity gate removed the harm. Coverage (~5% of lookups) now bounds the
*benefit*, not the damage: the feature is safe but only lightly effective. Full
value propagation remains the path to a large improvement — it would take
coverage toward total — but it is no longer a prerequisite for shipping
something that does not hurt.

**Still to verify first-hand** with `screenshot_hires`, before any of this is
called done: an A/B of the same frame with the knob off vs on, at
supersampling >= 2.

### G1.6 — G1.5 RETRACTED: both titles crack. Ambiguity gating is not enough.

User verification on the exact-table + ambiguity-gate build, **Ape Escape AND
Tomba 2**: thin seams across meshes in both. G1.5 claimed the gate had fixed the
cracking; it had not. G1.4's original conclusion stands.

**What went wrong in the analysis, twice:** each conclusion was drawn from a
SINGLE observation instead of a controlled A/B — first from native screenshots
that could not show the defect at all (G1.5), then from one favourable in-game
frame that happened not to expose it. A scene where the corrected ~5% of
triangles do not border a visible silhouette looks clean; that is not evidence
the seams are gone.

**Settled position.** Ambiguity gating removed one failure mode (vertices
inheriting a neighbour's fraction) but coverage of ~5% still mixes corrected and
uncorrected triangles within a mesh, and those seams ARE visible. Partial
coverage is not shippable at any ratio short of near-total, because the crack is
a property of the boundary, not of the magnitude of the error.

`geometry_correction` therefore stays OFF and must not be offered as usable
until precision propagates with the data (G1.2/G1.3). The launcher rows, the
`[video]` plumbing, the renderer-facade seam, `geom_correction` and
`screenshot_hires` all remain valid groundwork for that work.

**Method rule for the next attempt:** no conclusion about this feature from a
single frame. Same frame, same scene, off vs on, captured with
`screenshot_hires`, on at least two titles.

### G1.7 — ⚠ G1.6's MECHANISM IS UNCONFIRMED. Read this before trusting G1.1-G1.6.

Two problems with everything above, both found only after G1.6 was written.

**1. The line signature does not match the stated mechanism.** G1.4/G1.6 explain
the artifact as cracks between corrected and uncorrected triangles. Such cracks
would be SHORT seams tracing mesh silhouettes. The reported artifacts on both
titles are **long, straight, scene-spanning lines** — cyan diagonals crossing
Tomba 2's terrain, dark diagonals crossing Ape's. That is the signature of a
vertex landing far from where it belongs (a stretched/degenerate primitive), or
of stray line primitives — NOT of sub-pixel boundary mismatch. The coverage
numbers in G1.4 are real, but they were fitted to the wrong picture.

**2. THE CONTROL WAS NEVER RUN.** At no point was it confirmed that these lines
are ABSENT with both toggles off on these builds. Every conclusion in G1.1-G1.6
assumed the feature caused them. Two other large changes landed underneath this
work and are equally plausible causes:
  - the framework jumped **92 commits** (Ape's pin 3c67a52 -> current master);
  - recomp-ui jumped **64 commits** (bb62af1 -> origin/master), forced because
    the old pin could not compile against current framework master at all.

**Do this first, before any further analysis:** same scene, same spot, both
toggles false, `supersampling = 2`, captured with `screenshot_hires`. If the
lines persist, this is not the geometry feature and G1.1-G1.6 are describing
something that was never happening.

Candidate to check if the feature IS implicated, given the signature: a stale
sub-pixel override surviving to a later primitive. `glb_draw_*_triangle` calls
precise_consumed() only on the GPU path — the `!s_raster_ok` software-fallback
branch returns EARLY without clearing s_pc_valid, so an override could be
applied to a primitive it was never computed for. That would displace a vertex
arbitrarily and produce exactly these long stretched lines.

### G1.8 — RESOLVED by isolation: perspective textures SHIP, geometry correction DOES NOT

The control run and the two isolation runs were finally done, on Ape Escape,
OpenGL, supersampling 2, same scene each time:

| geometry_correction | perspective_texturing | result |
|---|---|---|
| off | off | **clean** (the control — establishes the framework/UI jumps are NOT the cause) |
| **on** | off | **lines** |
| off | **on** | **clean** |

**`perspective_texturing` is good and should ship.** It is unaffected by the
coverage problem: it only alters UV interpolation inside a polygon whose
provenance is fully proven, so a polygon either gets perspective UVs or keeps
the PS1's affine ones. Neither outcome moves a vertex, so adjacent polygons
cannot disagree about a shared edge and nothing can crack.

**`geometry_correction` must not be offered.** It moves vertices, and at ~5%
coverage a corrected triangle meets an uncorrected neighbour along a shared
edge — the seams in the isolation run trace polygon boundaries, confirming
G1.4's mechanism (and retiring the "long scene-spanning lines" reading in G1.7,
which misjudged the artifact). It cannot be fixed by tuning: it needs the full
PGXP dataflow of G1.2/G1.3 to reach coverage where meshes move as one.

**Recommended disposition:** keep the `perspective_texturing` setting and its
launcher row; withdraw the `geometry_correction` row until value propagation
lands (leave the setting readable from game.toml/settings.toml so the work
stays testable, but do not present it to players).

### G1.9 — Disposition EXECUTED (2026-08-05): this is the shippable line

G1.8's recommendation was carried out. This branch
(`feat/pgxp-perspective-textures`, renamed from `feat/pgxp-geometry-correction`)
is the line intended for review and eventual merge.

| setting | default | player-reachable? | verdict |
|---|---|---|---|
| `perspective_texturing` | false | **yes** — Display row in the launcher | ships |
| `geometry_correction` | false | **no** — `game.toml` / `settings.toml` only | known broken, hidden |

`geometry_correction` is deliberately still *compiled in*. It was not excised,
because the setting is only worth keeping if the code behind it still runs, and
G1.8 asked for it to stay testable. What was withdrawn is the launcher row — the
only surface through which a player could have reached it. Combined with the
false default, there is no path by which someone who is not editing a toml by
hand can turn on a feature we know cracks meshes.

**Standing constraint for future sessions.** Do not add a `geometry_correction`
control to any settings surface, and do not flip its default, until the coverage
problem is actually solved — meaning the census reports something far better than
the measured 5.2% hit / 93.1% ambiguous, on a real title, with the OFF control
run first. Partial coverage is not a partial feature here; it is a visible
defect, which is the whole finding of G1.1–G1.8.

The withdrawn row, the full post-mortem, the mechanism that would fix it, and the
re-test protocol are preserved on `park/pgxp-geometry-correction` (see its G1.9).

### G1.10 — PGXP value-propagation engine LANDED, phases 0+1 (2026-08-15)

The G1.2/G1.3 mechanism — precision travelling WITH the data — now exists
(`feat/pgxp-dataflow`, tracked as `beads-eio.3.48`). **Clean-room**: the
vendored `duckstation/` tree is CC BY-NC-ND (NoDerivatives) and `beetle-psx/`
is GPL, both incompatible with this project's PolyForm Noncommercial license,
so NO code was ported from either; they remain black-box behavioral oracles
only. The engine implements the publicly documented technique from psx-spx +
our own G1 analysis.

**What exists now:**

- `runtime/src/pgxp.cpp` + `runtime/include/pgxp.h`: per-word RAM+scratchpad
  shadows (~10 MB, lazily allocated, fail-closed), per-GPR (+HI/LO) and
  per-GTE-data-reg shadows. Each `PGXPValue` records the sub-pixel 16.16
  screen X/Y, the projected SZ depth, per-half validity flags, and the exact
  guest word it describes. **The one safety invariant: a shadow is only
  believed after validating that word against reality.** Overwrite-and-
  validate only — no side-effect modelling, so DMA/memcpy/untracked writers
  need no hooks at all (their stale shadows fail validation at use). The
  plain-store `gte_precision_invalidate_word` calls in memory.c are removed
  as redundant under this model (a small store-path win).
- Hook funnel (`runtime/include/pgxp_hooks.h`): 5 entry points
  (`psx_pgxp_load/store/alu/muldiv/cop2`) taking the raw instruction word.
  Generated code will call them via `PGXP_*()` macros that expand to nothing
  unless compiled with `-DPSX_PGXP=1` — the G1.3 codegen-time gate, as a
  preprocessor define over ONE generated tree (no flavour drift, base objects
  byte-identical). `OverlayCallbacks` gained an appended-last `PGXPHooks*`
  table (NULL-tolerant); the ABI bump arrives with the Phase-2 emitter change.
- Both interpreters (`dirty_ram_interp.c`, `psx_interpreter.c`) call the
  hooks directly on loads/stores/COP2 transfers and the precision-carrying
  arithmetic (moves, add/sub, or-merge, 16-bit shifts, LUI, HI/LO). Ops that
  merely destroy precision are deliberately NOT hooked — validation already
  drops their stale shadows.
- `pgxp_cpu_mode` (tier-2 arithmetic propagation, default off, matching the
  references' default) and `pgxp_tolerance` (sub-pixel clamp, default off)
  are new `[video]` keys and live-tunable via the TCP `pgxp` verb, which also
  live-toggles `geometry`/`texture` for same-scene A/B (the G1.6 method rule
  no longer needs a restart between arms).
- GPU consumption is now PER-VERTEX (`pgxp_get_precise_vertex`): dataflow
  shadow first (validated against the actual packet word), the G1.4 exact
  ambiguity-gated position cache second (never carries depth), native
  integers last. Two safeguards on every precise vertex: truncation agreement
  (integer part must equal the native 11-bit parse) and the tolerance clamp.
  Mixed precise/native triangles are correct — a native vertex sits exactly
  where the uncorrected pipeline put it, bounding any seam to < 1px of
  sub-pixel fraction (the G1.1 cracks came from wrong-vertex substitution and
  all-or-nothing triangles, both gone). Geometry and perspective UVs now
  share one provenance source and compose per triangle.
- Census: `geom_correction` grew a `pgxp` object (dataflow_hit /
  fallback_hit / native / value_mismatch / trunc_reject / tolerance_reject /
  w_valid). The G1.9 gate now reads: dataflow-hit share must be dramatically
  above the 5.2% position-cache ceiling before any launcher surface returns.
- Tests: `pgxp_test` (roundtrips, half-word semantics, validate-on-read,
  DMA-overwrite rejection, repack arithmetic, suppression bracket,
  safeguards) + `gte_register_access_test` updated to the precise
  invalidation contract. 33/33 enabled runtime tests green.

**Not yet (Phase 2+):** emitter macros at the ~47 sites, the `-DPSX_PGXP`
dual link targets + flavor-namespaced shard caches + ABI bump, BIOS regen,
and the Ape/Crash/Tomba2 census + `screenshot_hires` A/B campaign. Until the
emitter lands, compiled code feeds the engine only through the v14 swc2
sites, so dataflow coverage on a running title comes from interpreted code
paths; the full-coverage census gate is a Phase-2/3 measurement.

Incidental fixes while landing this: the per-target `psx_game_version.txt`
`file(GENERATE)` collision that broke every fresh single-config configure
(runtime.cmake once-only stamp), and `overlay_capture_retry_test` not
compiling under the SDL3 default (SDL_SetMainReady gone + inverted SDL_Init
check — it now uses `psx_sdl_init`).

**First real-title census (Ape Escape, 2026-08-15) — the G1.9 gate is
PASSED.** Worktree build against this branch (`_wt-ape-pgxp`,
`-DPSX_PGXP_VARIANT=ON -DPSX_DEBUG_TOOLS=ON`, game C regenerated), memory-mode
only (`pgxp_cpu_mode` off), boot → title → attract, per-10s windows:
**79–90% dataflow hits** (vs the 5.2% position-cache ceiling of G1.4),
fallback ≤0.4%, `value_mismatch` 0, `trunc_reject` ~0, and every dataflow hit
carried a usable depth. The residual native share is 2D HUD/sprite content,
which is exactly what must stay native. Still to run before any ship surface:
the windowed `screenshot_hires` off/on A/B (headless has NO hi-res mirror —
`scale:1` fallback, the same blind-instrument trap G1.5 documented), on Ape +
Crash with Tomba 2 as the 2D negative control; the hook-overhead FPS measure
(pgxp build, feature off, vs base); and pgxp-flavour (`_f2`) overlay-shard
autocompile validation. Validation-build notes: title Release builds default
`PSX_DEBUG_TOOLS=OFF` (no TCP), and a title launched without `--game` binds
the framework default port 4370.

**Interactive visual isolation, USER-VALIDATED (Ape Escape, in-game,
2026-08-15).** Same scene, one toggle at a time, player observing live:
OFF = baseline vertex jiggle; geometry ON unclamped = jiggle largely gone but
sparse hairline background-bleed seams (a <1px disagreement along a long
shared edge between a corrected triangle and an uncorrected neighbour — the
THPS2 line class, NOT wild vertices; both reported artifacts vanished with
the toggle off); geometry ON with `pgxp_tolerance = 0.5` = **seams gone**,
only sub-half-pixel misalignment remains. 0.5 is therefore the shipped
default (config_loader.h), live-tunable via the `pgxp` TCP verb. Known
tooling defect found on the way: `screenshot_hires` produces a tiled/black
PNG at 768x480 scale-2 windowed (row-pitch bug in the hires readback) —
the census + the player's own captures carried the session; fix it before
the formal Crash/Tomba2 A/B.

### G1.11 — PGXP as a title's default: hook build, mod at session start, dataflow only, exact projection (2026-10-01)

Ridge Racer Type 4 turns PGXP on by default. Getting there took framework
changes within the elective PGXP enhancement. With PGXP off, nothing changes
for any title. With PGXP on, its correctness fixes apply without a new option:
the mod arms PGXP (item 2), hook builds default to dataflow-only precision
without a tolerance clamp (item 3), and fully precise axis-aligned quads take
the triangle path (item 4).

**What R4 got from G1.10 as shipped: nothing.** Base flavor, PGXP mod on:
0% dataflow hits, 3.6% position-cache hits, 0 of 282k textured triangles with
perspective UVs. R4's course renderer copies SXY out of the GTE with
`mfc2`/`sw`/`lw`/`sw`, and only `swc2` is shadowed in the base flavor. The
hook flavor already tracks all of it, with no R4-specific hooks: R4's near
polygon subdivision interpolates in 3D on the GTE and projects every
sub-vertex with RTPT.

1. **`psxrecomp_add_game_runtime(... PGXP)`** builds the title's one runtime
   as the hook flavor (`-DPSX_PGXP=1`, overlay flavor 2, no `_pgxp` suffix, no
   clone). Generated C already carries the `PGXP_*()` sites, so a committed
   `generated/` does not change. The option is parsed, so it can no longer be
   swallowed by an earlier multi-value argument. Test:
   `game_runtime_pgxp_test` (the real function body under `cmake -P`).
2. **The PGXP mod arms at session start.** `psx.enhancement.pgxp`'s
   activation used to arm the corrections, and main.cpp's renderer setup,
   which runs after activation, then re-applied the `[video]` baseline and
   switched them off. The activation now records a request
   (`pgxp_mod_request`), and the renderer setup's session arming
   (`psx_pgxp_session_arm`, `runtime/src/pgxp_session.cpp`) takes it, reads
   and clears it, and arms `[video]` OR the request. The
   `PSX_GEOMETRY_CORRECTION` / `PSX_PERSPECTIVE_TEXTURING` /
   `PSX_PGXP_CPU_MODE` / `PSX_PGXP_CULLING` env overrides still win. Because
   the arming takes the request, a later session whose plan is empty (netplay
   clears every mod, default-on ones included) cannot inherit it.
   `reset_mod_owned_presentation()` also clears it. A title that wants PGXP
   on by default overrides the builtin manifest at the same id and version
   with `default_enabled = true` (MOD_PACKAGES.md). It must not use
   `[video] geometry_correction` for that, because netplay does not clear it
   (see `pgxp_mod_only`, G1.12). Tests: `pgxp_session_test` runs the real
   arming against the real mod runtime, the builtin plugin and the builtin's
   own manifest (default-on override, netplay clear with and without the
   reset, the off switch, options, env overrides). R4's `r4_pgxp_boot`
   boots the built runtime and fails if main.cpp stops calling the arming.
3. **Automatic hook-build defaults and `[video]` overrides** (game.toml;
   live over the TCP `pgxp` verb). A runtime built with `PSX_PGXP=1` selects
   `pgxp_tolerance = -1.0` and `pgxp_position_fallback = false` when those keys
   are absent. The runtime and overlay ABI enforce the matching hook flavor.
   Base builds keep 0.5 and true for their limited dataflow coverage. Explicit
   title tuning wins independently for each key, including Ape's validated
   clamp. PGXP remains elective; exact-projection shadows remain separate:

   ```toml
   [video]
   pgxp_tolerance = -1.0            # hook-build default: no clamp
   pgxp_position_fallback = false   # hook-build default: dataflow only
   pgxp_preserve_projection = true  # default false
   ```

   - `pgxp_position_fallback = false` ("dataflow only", the reference
     implementations' default) skips the G1.4 position cache and stops
     `geom_note` filling it. At hook-build coverage the cache only hands
     unrelated 3D fractions to CPU-built 2D polygons: on R4 (tolerance off,
     cache on), 30% of the HUD tachometer needle's vertices (rms 0.86 px).
   - `pgxp_preserve_projection = true` shadows the exact projection of each
     vertex, from the unshifted MACs and a true divide
     (`pgxp_project_precise`), instead of the GTE's integer IR1/IR2/SZ3 path.
     Only sf=1 vertices with no IR, SZ3 or divide saturation qualify; the rest
     keep the IR path. Guest SXY, MAC0 and FLAG are untouched (the GTE oracle
     test runs RTPS/RTPT with it on). The exact position can sit below the
     guest integer or more than a pixel above it, so truncation agreement
     becomes the window -4 < precise - native < 5 px. A packet half at or
     beyond the GTE saturation limit (-1024 / 1023) must still agree exactly.
     RTPS applies the same window: an exact projection outside it is not
     shadowed, and the vertex keeps the IR path's shadow, which always
     agrees, so it still draws precise instead of being rejected to native
     (`ppp_window_fallback` counts these; `pgxp_ppp_accept`). The exact math
     is skipped while the hooks record nothing (speculative passes). The
     tolerance clamp now measures the offset both ways. One non-visual
     consumer: `[widescreen] precise_nclip` takes its branch sign from the
     shadows, so in a title that sets both, that sign follows the exact
     projection too.
   - Keep `pgxp_tolerance` at its 0.5 default only for low-coverage titles
     (the G1.10 Ape result). At hook-build coverage the clamp is harmful: on
     R4 it keeps 26% of vertices, leaves 52% of drawn triangles mixed (a
     4K capture shows a hairline across the road) and shakes more than stock
     (tables below). The old universal default stayed 0.5 to avoid changing
     every title that already arms PGXP through `[video]`. This historical
     default is now retained only in base builds; hook builds automatically
     disable the clamp unless a title supplies an explicit value.
4. **Fully precise axis-aligned textured quads skip the 2D rectangle
   shortcut.** `gp0_exec_textured_quad` draws a quad whose integer vertices
   and UVs form an axis-aligned rectangle with `gr_draw_textured_rect`, at the
   native position. A world quad seen straight on (R4's building facades)
   lands there too, and against its precise neighbours that opened an
   L-shaped background seam at 4K. When PGXP is correcting and all four
   vertices are dataflow-precise (`pgxp_probe_precise_vertex`), the quad is
   drawn as its two triangles (`rect_bypass`). A rectangle with only some
   corners precise, such as a CPU-built sprite with one corner copied from
   SXY, keeps the shortcut and counts as `rect_partial`. Drawing it as
   triangles would mix precise and native vertices. R4 has none: 0 in every
   run below.

Observability: `geom_correction`'s `pgxp` object reports the switches,
`ppp_produced` / `ppp_window_fallback`, geometry-corrected triangles as
`tri_precise` / `tri_mixed` / `tri_native` (the G1.1 crack exposure),
`rect_bypass` / `rect_partial`, and the NCLIP counters of G1.12.

**What `tri_mixed` can and cannot see.** It counts drawn triangles with one
or two precise vertices: a shared edge whose vertex is precise in one
triangle and native in its neighbour. Under preserve-projection that
disagreement is up to the window, several pixels, not a fraction. A vertex
is decided per packet word, so two triangles that share a projected vertex
make the same decision. `tri_mixed` is blind to the other crack classes: an
all-precise triangle beside a game-built 2D one, and a gap where the game
culled a face (G1.12). Zero mixed triangles is a statement about those runs,
not a proof of no seams.

**R4 measurements.** Helter Skelter Grand Prix from a grid savestate, Cross
held, 60-frame lead, 360-frame window, native internal resolution, macOS
arm64, every run 0 dispatch and 0 segment misses. "Dataflow" is the share of
GPU vertex lookups that took a validated shadow.

| build / settings | dataflow | position cache | drawn triangles mixed | perspective UVs |
|---|---|---|---|---|
| base flavor, mod on, framework defaults (R4 before) | 0% | 3.6% | 8.9% | 0 / 282k |
| hook flavor, framework defaults (tol 0.5, cache, IR path) | 25.7% | 0.002% | 51.6% (152k / 295k) | 100% |
| hook flavor, tol off, dataflow only, exact projection | 99.88% | 0 | 0 / 312k | 100% |
| **R4 shipped** (the above plus precise culling, G1.12) | **99.91%** | 0 | **0 / 389k** | **100%** |

The vertices left native are the HUD needle (CPU-built from a sin/cos table)
and vertices at the GTE saturation limit far off-screen, which draw where the
hardware puts them. Other content with R4's shipped settings:

| run | dataflow | drawn triangles mixed | truncation rejects |
|---|---|---|---|
| boot through the menus, Grand Prix setup and car select | 96.6% (rest 2D UI) | 0 / 1.83M | 0 |
| attract demo, 3000 frames | 95.2% (rest 2D UI) | 8 / 1.49M | 12 |
| 2P VS, pad 1 driving, 360 frames | 100% | 0 / 467k | 0 |
| 2P VS, widescreen Fit, both pads driving, 600 frames | 99.992% | 149 / 730k (0.02%) | 174 |

Every truncation-rejected vertex in the per-vertex dump of the last run
(208 census vertices) has a packet half at the saturation limit. Of those, 94
come from the effect emitter (store PC `0x800173F0`). The rest come from the
course and near-subdivision stores (`0x80064284..90`, `0x80065B00..08`). No
in-range vertex was rejected, so the 149 mixed triangles each have a corner
clamped far off-screen. The exact-minus-native offsets of shadowed 1P
vertices measured -1.87 .. +2.60 px (R4: H = 290).

Frame-to-frame vertex wobble (`dE`: rms change of drawn-minus-exact between
consecutive frames along tracked vertices, native px; x9 = 4K output px; the
reference is the exact projection; slow start from the grid, 152 frames,
43k tracked steps; near = SZ3 < 1000):

| mode | err | dE | dE near | dE at 4K |
|---|---|---|---|---|
| native (PGXP off) | 0.834 | 0.321 | 0.460 | 2.89 |
| hook flavor, tol 0.5, IR path | 0.807 | 0.374 | 0.545 | 3.36 |
| tol off, IR path | 0.075 | 0.025 | 0.052 | 0.23 |

At speed (360-frame lead): native 0.417, tol 0.5 0.504, IR path 0.032. With
preserve-projection the drawn position is the reference itself, so its row
would read 0 by construction, not by measurement. It only shows that no drawn
vertex fell back to the IR path or native in that window. What is
established independently is that the exact projection is right: the
`pgxp_test` and `gte_register_access_test` oracles check it against the GTE
inputs.

**Visible changes.** Polygons stop wobbling and road, wall and sign textures
stop bending (visible at every scale for textures, above native for
geometry). Far thin features draw at their true size: distant lane dashes and
the start line appear in the mirror, and the START!! board at the far end of
the straight looks smaller. The first frame or two after a savestate load
draw without PGXP (shadows are dropped on load, R4 builds packets a frame
ahead). Before G1.12, the far road beyond the start gantry and over crests
drew as strips with sky and backdrop between them, at 4K and in 2P. That was
a culling gap, not texture minification.

**Not covered.** Semi-transparent mono quads still take their own rectangle
path (UI boxes; none seen in R4's 3D); a PGXP depth buffer; savestates of the
hook and base flavors are not interchangeable; above native resolution the GL
native pack comes from the corrected high-resolution surface, so a title that
reads VRAM back to the CPU would see it (R4 issued no GP0 C0 read from boot
through a 2P race). Vigilante 8: 2nd Offense does: in battle it reads one
VRAM pixel with GP0 C0 about every third frame (`c0_history`, now an
always-on ring), and the reads that land in the drawn frame return the
corrected pixels, so PGXP on and off write different RAM values (same PC
path and cycles; measured over 3000 frames from one savestate). Netplay
therefore draws its CPU-authoritative VRAM faithfully
(`sw_set_faithful_authority`: no PGXP override, no texture filter), so
peers agree whatever their shadows or Display filter.

### G1.12 — Precise culling, the mod as a title's one switch (2026-10-01)

**The crack G1.11 did not see.** With every drawn vertex precise, R4's far
road still broke into a ladder of strips with sky between them, beyond the
start gantry and over crests, at 4K and in 2P. Same frame, PGXP off: solid
road. R4's road emitter (`0x80066268`) projects a row with RTPT, runs NCLIP
on both triangles (`0x8006630C`, `0x8006636C`) and skips the row when
neither is front-facing (`slt`/`xor`/`beqz` at `0x800663AC`). Far rows whose
native height rounds to 0 have MAC0 = 0 and are skipped. Natively their
neighbours meet at the same integer y, so nothing is missing. With PGXP the
neighbours sit at their exact y, 0.7 px apart at Helter Skelter's crest, and
the skipped row is now a visible gap. `tri_mixed` could not see it: every
drawn vertex was precise, and the missing face draws no triangle at all.

**The fix: NCLIP takes the sign PGXP draws.** A third PGXP option,
`culling`, belongs to the `psx.enhancement.pgxp` package and is off in the
builtin. A title's override turns it on, and the player can switch it off on
the Mods page. While geometry correction is armed, `gte_nclip` computes the
exact determinant of the three SXY register shadows
(`pgxp_gte_nclip_precise`). It uses them only when all three are live,
describe the current register words, and pass the same truncation agreement
and tolerance the GPU applies to them. When that sign is nonzero and differs
from the integer MAC0's, NCLIP returns the exact sign, with magnitude equal
to the rounded exact doubled area, at least 1. FLAG keeps the integer
result's overflow bits; matching signs leave MAC0 untouched. This is
DuckStation's "PGXP culling" with a minimal footprint.

This changes guest-visible MAC0, and so the game's control flow, which is
why it is opt-in at three levels:
- only the mod arms it, and there is deliberately no `[video]` key for it;
- a netplay session never arms it (`PSXPgxpSessionConfig.netplay`), even
  when the published netplay plan carries the mod (content negotiation,
  MOD_PACKAGES.md) and even from `PSX_PGXP_CULLING`: the shadows it reads
  are host-only, and a rollback load drops them on one peer only;
- it needs geometry correction (`psx_pgxp_session_resolve`);
- it is held off in every pass that is compared against another execution:
  the speculative and replay passes, which record no shadows, and the whole
  overlay shadow diff (`psx_overlay_shadow_diff_active`), including its
  interpreter pass, so both passes see the integer MAC0.

`[widescreen] precise_nclip` (`gte_nclip_precise_bltz`) compares against the
MAC0 the guest read, so it stays consistent. Live toggle: TCP
`pgxp {"culling":0|1}`; validation override: `PSX_PGXP_CULLING`.

Counters, kept whenever geometry correction is armed, culling on or off:
- `nclip_precise`: NCLIPs with an exact determinant;
- `nclip_disagree`: of those, the exact sign is nonzero and differs from the
  integer one. Each is a face the game culls or keeps on a winding the drawn
  face does not have, the crack class `tri_mixed` misses;
- `nclip_corrected`: of those, MAC0 replaced.

On R4 (1P window above), 178,896 of 623,784 NCLIPs disagree (28.7%). With
culling the game emits 24.5% more triangles (389k against 312k); in 2P the
figure is 21.6%.

**Measured on R4** (same-frame 4K A/B from the research frame gate, Helter
Skelter start straight frame 200, crest frame 420, 2P VS start frame 150): the
ladder and the backdrop blocks are gone, and the far road is continuous with
its lane dashes. In places it reads better than PGXP off, whose integer rows
smear. Thin lateral features that the integer test dropped now draw at their
true size: start-grid lines across the road, the namco plate under the
START!! board.

- **Packet headroom.** R4 builds packets bottom-up in two buffers of 141,176
  bytes (`0x800ADD10` / `0x800D0488`, set at `0x80021064`, no overflow
  check). Sampling every second frame over 40 s drives, the high-water mark
  was 62.6 → 74.3 KB in 1P and 63.8 → 83.4 KB at the 2P VS start (PGXP
  without → with culling), against 137.9 KB usable below the buffer's top-down
  region (60% at peak). A title with longer draw distance must re-measure.
- **Guest identity.** `fp_identity`, 12000 frames with no input (boot,
  menus, attract race), cold overlay cache, fresh memory cards. The stock
  build (framework before this work, base flavor), the hook build with the
  mod disabled, and the
  hook build with PGXP on but culling off are IDENTICAL pairwise, with no
  tolerance. With culling on, the run diverges at frame 10319, the first
  frame of the attract race, in RAM and scratchpad writes only (`sp`, `sc`,
  `wc`, `ws`): the packets differ. `cyc`, `mmio` and `mc` agree through frame
  12000, so the game's register traffic, including its flips and DMA kicks,
  is unchanged. 2P VS from the start, both pads driving: one flip every 2.0
  VBlanks with culling and without.
- **Cost.** CPU ms per emulated frame (headless, turbo, 1P, native, 600-frame
  samples, three interleaved rounds, load average about 7):

  | build | median | vs stock |
  |---|---|---|
  | stock (base flavor) | 4.37 | |
  | hook flavor, PGXP off | 4.66 | +7% |
  | hook flavor, PGXP on, culling off | 5.02 | +15% |
  | hook flavor, R4 shipped | 5.07 | +16% |

  In real time under R4's default stack (internal resolution Match display:
  10x, 4800 lines; widescreen Fit; frame rate at the 120 Hz display,
  interpolated; three interleaved 8 s samples, load average 6 to 12), guest
  VBlank/s and frame time:

  | | PGXP off | PGXP, no culling | R4 shipped |
  |---|---|---|---|
  | 2P VS, both pads driving | 59.3 (16.84 ms) | 58.8 (17.12 ms) | 56.6 (18.42 ms) |
  | 1P race | 59.3 (17.00 ms) | 56.1 (18.64 ms) | 57.0 (18.14 ms) |

  Most of the 2P cost is the extra triangles, about 1 ms of GL scene time at
  that scale: the GL renderer's cost is per primitive. In 1P the two PGXP
  arms are within noise. On this branch 2P draws no interpolated frames, so
  it was not measured with them.

**`[video] pgxp_mod_only` (game.toml, default false).** For a title that
ships PGXP through the mod (R4): the mod becomes the one switch. The
`[video] geometry_correction` / `perspective_texturing` / `pgxp_cpu_mode`
values, game.toml's and the player's `settings.toml`, are not applied, and
the launcher hides its Perspective textures row
(`has_geometry_precision = 0`). Without it, that row showed "off" while the
default-on mod corrected textures, unticking it did nothing, and ticking it
kept texture correction on after the player switched the mod off. Netplay
then runs with PGXP fully off.

### G1.13 — CPU-built vertices: the GTE as a multiplier, and word copies (2026-10-06)

**Symptom.** Ape Escape with the PGXP mod: thin light/dark lines along the
ground's grid edges at high internal resolution (bead beads-eio.3.274; present
on the shipped f7f0ad10 pin, not a regression). Isolation on one savestate:
PGXP off, or `geometry` off with texture correction on, removes them; texture
correction off, nearest filtering, frame smoothing off do not. The
`pgxp_tri_ring` showed whole ground cells drawn native next to precise
neighbours and decals (0.9 px apart on the same projected vertex).

**Two producers dropped precision.** The `pgxp_store_ring` pinned both:
1. *Subdivided near cells* (0x8001DB74). Edge midpoints are the projection of
   the 3D midpoint, computed on the CPU: CTC2 packs two tracked X (or Y)
   halves into a light-matrix row, MVMVA weights them by the vertex depths,
   MFC2 reads MAC, ADDU adds a rounding bias, DIV by the depth sum, MFLO, SH.
   CTC2 / CFC2 had no hook and MULT/DIV recorded imprecise results.
2. *The terrain grid* (0x80044B00) copies projected corners with the
   unaligned-copy idiom `lwl/lwr` + `swl/swr` on aligned addresses, which the
   engine treated as untrackable.

**Fix (cpu-mode).** A scalar tier: registers carry an optional full-range
precise value beside their half shadows. CTC2 / CFC2 are hooked (codegen 18;
the BIOS emitter fingerprint now covers `pgxp_hook_emitter`), MVMVA recomputes
its rows from the precise matrix halves / IR / translation and leaves scalars
on MAC1-3 / IR1-3, and MFC2, ADD/SUB(I)(U), shifts, MULT/DIV, MF/MT HI/LO and
OR-moves carry them; SH / SW turn a scalar that fits back into a coordinate
half. The guest rounded that arithmetic its own way, so such halves are
marked derived (`PGXP_F_DX/DY`): the GPU believes them within (-1, +2) px of
the guest integer and the tolerance clamp ignores the guest's own rounding
pixel. LWL at byte 3 / LWR at byte 0 (and SWL / SWR likewise) move the whole
word and copy its shadow. Result in the Fossil Field scene: every ground
triangle dataflow-precise, `tri_mixed` from ~10% of triangles to 474 of 2.05M,
seams gone (`test_pgxp` pins the sequence).
### G1.14 — PGXP renderer: depth buffer, perspective-correct colour, seam expansion (2026-10-06)

**Audit against the references.** Read from DuckStation's published source
(`github.com/stenzek/duckstation`, CC BY-NC-ND 4.0, so behaviour only, no
code: `src/core/cpu_pgxp.cpp`, `src/core/gpu.cpp` precise polygon path,
`src/core/gpu_hw.cpp` `DrawPrecisePolygon` / `SetBatchDepthBuffer` /
`CheckForDepthClear` / `IsPossibleSpritePolygon`, `src/core/gpu_hw_shadergen.cpp`
vertex depth, `src/core/settings.cpp` defaults). GooseStation
(`tehrzky/goosestation_nx`, GPL-2.0) is a patch set over a pinned DuckStation
(skip engine, runahead PGXP state); its PGXP rendering is DuckStation's.

| Feature | DuckStation | psxrecomp before | now |
|---|---|---|---|
| Geometry correction (sub-pixel vertices) | dataflow shadows, `GetPreciseVertex` | yes (G1.10/G1.11, hook flavor 99.9% on R4) | same |
| Culling correction | `PGXPCulling`, default on | yes, mod option (G1.12) | same |
| Texture correction | perspective UV via w | yes | same |
| Colour correction | `PGXPColorCorrection`, default off | no (Gouraud affine) | **yes**, `pgxp_color_correction`, key F10 |
| Vertex cache | `PGXPVertexCache`, default off | position cache (`pgxp_position_fallback`) | same |
| CPU mode | `PGXPCPU`, default off | `pgxp_cpu_mode` (tier-2) | same, live key F12 |
| Preserve projection precision | `PGXPPreserveProjFP` | exact projection (G1.11) | same |
| Tolerance | `PGXPTolerance` -1 | `pgxp_tolerance` | same |
| Depth buffer | `PGXPDepthBuffer`: per-vertex w as depth, LEQUAL, only for polygons whose w differ (3D) and opaque unless `PGXPTransparentDepthTest`; cleared on drawing-area change and when average z rises by `PGXPDepthThreshold` (4096) | no | **yes**, `pgxp_depth_buffer`, key 9 |
| 2D polygons | sprite mode for non-3D precise polygons; `PGXPDisableOn2DPolygons` draws invalid-w polygons native | unproven vertices native per vertex; precise axis-aligned quads bypass the rect path (G1.11) | same; 2D never tests/writes depth |
| T-junction / seam handling | none for polygons (line expansion only) | none | **seam expansion**, `pgxp_seam`, key F11 |

**Depth buffer.** gpu.c passes each triangle's SZ when all three vertices
are dataflow-precise (`gr_set_depth_triangle`). The GL backend draws an
opaque one with a LEQUAL test and write; depth is 1 - 2*256/(SZ+256) in NDC,
linear in 1/z, so a plane's depth interpolates exactly in screen space. 2D,
unproven, semi-transparent polygons, lines and rectangles neither test nor
write. Depth mode is a batch key of both GL batches and rides in every
high-resolution-window / native-wide replay command. Clears are a colourless
depth-only draw through the flat batch (so every surface clears in painter
order): before the first depth triangle after a drawing-area change or a
fill, and when the average SZ rises by `pgxp_depth_threshold` (4096, as
DuckStation). Two bugs found on R4 while landing it: a clear at window depth
1.0 can be clipped by the far plane (now inside it), and R4's one-native-pixel
line quads joined a depth batch and wrote near depth along every road and
wall edge (lines now always break the batch). On R4 the threshold clears ~25
times a frame; without it far beams vanish behind the previous view's depth.

**Decals keep painter order.** A plain LEQUAL depth (DuckStation's choice,
no bias) cut R4's lane markings into the road: they are separate polygons
drawn after it, a hair off its plane once SZ is quantised per vertex. A 3D
batch is now two passes: the colour pass tests with its depth pulled toward
the camera by 2% of the distance (`PSX_PGXP_DEPTH_TOL`) and writes no depth;
a colourless pass then writes the true depth. A later surface within the
tolerance wins as in painter order; anything clearly behind stays occluded.
Inside one batch the order is painter's. On slots 1-3 at 10x the image
matches depth-off within 96 / 36 px (tunnel) and the markings are whole.
A near lane dash was still cut at the bottom of the screen (behind the
tachometer, owner report): near the camera SZ quantisation and R4's near
subdivision disagree by more than the tolerance. Triangles with any vertex
nearer than SZ 1024 (`PSX_PGXP_DEPTH_NEAR`) now stay out of the depth buffer
(painter order), the tolerance gains an absolute 48 SZ, and every displayed
frame starts with a cleared depth buffer. Slots 1-4 at 10x: depth on vs off
differ by 801 / 92 / 25 / 0 px, none in the tachometer area.

**Perspective-correct colour.** Gouraud colour on 3D triangles interpolates
with 1/SZ (textured triangles with perspective UVs share their w).

**Seam expansion.** At internal scale > 1 a vertex that lies on its
neighbour's edge only to the PS1's precision leaves a hairline onto the
background (T-junctions of R4's subdivided near polygons; tunnel walls).
Opaque 3D triangles move each edge outward (mitred, limited at sharp
corners): `fine` = 1 output px (`PSX_PGXP_SEAM_PX` tunes it), `wide` = half a
native px. UVs, colour, q and SZ are extrapolated with the barycentric
coordinates of the new corners (perspective-correct where the attribute is),
so textures do not slide. `wide` closes larger gaps but smears edge texels
and thickens silhouettes (beam undersides at the R4 tunnel entrance); `fine`
is the recommended setting. Both widths apply only above 1x, and only to
triangles the depth buffer tests: with `pgxp_depth_buffer` off, and for
near-camera triangles kept in painter order, a widened edge would draw over
its neighbour, so those never expand. `gl_frame_gen_test` pins both (seam
without depth draws exactly like no features; at 1x depth+seam like depth).

**R4 (hook flavor, 10x headless-opengl, Helter Skelter tunnel, savestates
at the entrance and inside).** Thin-feature pixel counts (features narrower
than half a native pixel against both neighbours, HUD excluded):
entrance base 1524 / PGXP 3560 / +depth+colour 2070 / +seam fine 1410-1518;
inside base 2323 / PGXP 539 / +depth+colour 540 / +seam fine 541-547. The
remaining counts are mostly texture detail; the visible base cracks along
the tunnel walls are gone with PGXP and the residual short ones with `fine`.
Dataflow 99.86%, mixed triangles 0, 0 dispatch / segment misses; guest pace
equal with the features on and off.
Diagnostic: `PSX_PGXP_TRI_LOG=<file>` logs every triangle (depth mode, x y
SZ) while `<file>.on` exists.

**Off costs nothing; Smooth motion matches.** gpu.c sends
`gr_set_depth_triangle` only while a PGXP renderer feature is on
(`gl_renderer_pgxp_render_wanted`), so with them off the render thread
records no extra `RTH_DEPTH` per triangle. The SZ rides in the textured
vertex's unused colour alpha (negative when present), so the vertex stays 26
floats for every title. The setters sync with a live render thread
(`GL_RT_SYNC`) like their neighbours. A depth-mode change ends a textured
batch as its own reason (`batch_diag` entry 8). Smooth-motion in-between
frames replay `RTH_DEPTH` and test depth like the real frame, from their
own clear: `gl_frame_gen_test` draws overlapping PGXP triangles out of painter
order and requires the in-between frame at phase 1 to equal the real one,
with each feature (depth, colour) alone and together; each changes the real
image there.

## IR1 — Internal resolution presets (Native … 8K) and the GL scale ceiling (2026-09-26)

**What the player gets.** Settings → Display → **Internal resolution**: Native,
720p, 1080p, 1440p, 4K, 5K, 8K, Match display. A preset is a target height and
resolves to an integer scale over the title's reference height
(`[video] resolution_reference_lines`, 240 by default): 3x, 5x (1200 lines,
area-resolved to 1080), 6x, 9x, 12x, 18x. It is a Settings row, not a mod,
because it needs no game hooks; it is presentation-only, so it also applies in
netplay (each peer its own). Keys and precedence: `docs/config_schema.md`.

**Why the old ceiling was 4.** The software renderer keeps a CPU mirror of
1 MiB·S², so it caps at 4, and every backend inherited that cap. OpenGL keeps
the hr surface on the GPU; its real limits are the driver's texture,
renderbuffer and viewport sizes (the surface is `1024·S × 512·S`) and memory.
The GL backend now clamps at context init to those limits and a 2 GiB budget,
logs the clamp, and steps down instead of failing (a failure used to drop the
whole backend to software). Measured on an Apple M4 (`4.1 Metal - 91.7`): all
three limits are 16384, so the full-VRAM surface stops at 16x.

**Fixes that only bite above 1x** (all gated on S > 1, so native is
byte-identical):

- Lines: `glLineWidth(S)` is capped at 1 on core profiles (macOS reports
  `GL_ALIASED_LINE_WIDTH_RANGE` 1..1), so every line was one hr pixel thick.
  Lines are now quads one native pixel thick that include both endpoints, like
  the software rasterizer.
- Present: a supersampled source more than 1.25x the output is area-resolved
  (bilinear taps over the pixel's footprint) instead of one tap, and the UV
  inset is half a texel, not half a native pixel (which cropped (S-1)/2 texels
  per edge).
- The copy scratch is sized to the largest copy and the mask stencil is
  rebuilt in tiles over the primitive bbox union, so a large S does not cost
  another full-size surface.
- The CPU present path under GL presents at 1x (its readout is native) and its
  staging buffer no longer grows with S².
- The macOS game window gets a high-pixel-density drawable when the player
  picks anything above native; without it the drawable is in points and the
  compositor stretches it, throwing half the resolution away.

**Verification.** `gl_scale_invariance_test` renders one GP0 scene on a hidden
real GL context at 1, 2, 3, 5 and 9x and requires identical guest-visible VRAM
(the pack of the hr surface) at every scale, one-native-pixel lines at
internal resolution, and a 32x request that stays on GL at the driver clamp.
In a running game, `video_info` reports the requested and effective scale and
the drawable; `screenshot_hires` reads the hr FBO at full size.

### IR2 — True 8K past a 16384 texture limit: the high-resolution window (2026-09-26)

8K is 18x at the usual 240-line reference, and a full-VRAM surface at 18x is
18432 px wide: over the 16384 limit of Apple's GL (and Intel, MoltenVK). The
M4 returns `GL_INVALID_VALUE` for it. A title only needs the displayed frame at
18x, so past that limit the GL backend splits the job:

- The **authoritative** VRAM stays the ordinary hr surface at **1x**. It is the
  native renderer unchanged, so pack, CPU readback, render-to-texture sampling
  and VRAM copies are exactly the native results.
- A **presentation-only** surface at S covers the displayed columns,
  W = [x0, x1) × all 512 rows (R4: x 0..319, 5760×9216 at 18x, 405 MiB). Every
  GPU write that touches W is mirrored into it: textured and flat batches,
  lines (as quads), fills, uploads and the depth24 clear, and VRAM copies
  (hi→hi when the source lies in W, otherwise the 1x source upscaled). It has
  its own stencil, rebuilt from alpha like the hr surface.
- W starts empty and grows, rounded to 64 columns, the first time a display
  rectangle outside it is presented, seeded by upscaling the 1x content. The
  present, hold-last, interpolation capture, `screenshot_hires` and the
  native-wide centre read W.
- When the union of the displayed rectangles no longer fits one surface
  (side-by-side 512-wide buffers at x 0 and 512 need 18432 px at 18x), W
  becomes up to four **tiles**, each its own surface over a column range
  (there: one per buffer, 9216×9216 and 648 MiB each). Every mirrored write
  goes to each tile it touches, so a game flipping between horizontally
  adjacent buffers presents every frame at S instead of alternating with 1x.
  A display that no tile can hold within the GPU limit, the memory budget
  (all tiles together) or the four-tile cap presents at 1x, with a log line.
  Everything drawn inside a tile, and axis-aligned rects, fills, copies and
  uploads across a tile edge, match the single surface byte for byte; a
  sloped primitive that crosses a tile edge is clipped there by GL, which
  can move its interpolated colour by one step or its coverage by a subpixel
  along it.
- A VRAM copy into W stages its S-scaled source in a scratch of its own, in
  column chunks that fit the GPU limit (910 columns at 18x on a 16384 GPU),
  walked in memmove order so an overlapping copy still reads pre-copy
  pixels. Staging textures record a new size only after the driver accepted
  it; a request past the limit is refused with a log line.
- The window's copies of textured, flat and line draws are **queued** and
  replayed into it in one pass at the next sync point: anything that changes
  what they sample (a pack of the raw mirror, an upload, the depth24 clear),
  any other write to the window (fill, copy, upload, stencil rebuild), and
  every read of it (present, capture, growth). Mirroring each batch as it was
  flushed switched framebuffers twice per batch; on Apple's GL (Metal
  underneath) each switch ends a render pass, which measured ~80 us, 10 ms
  per R4 frame (8K ran at 21 fps). Queued, the flush CPU cost is 0.6 ms per
  frame and 8K holds R4's 60 Hz present cadence on an M4.

It engages only when the full-VRAM surface cannot hold the requested scale;
below that (up to 16x on the M4) nothing changes. `PSX_GL_HIRES_WINDOW=0/1`
disables or forces it. `PSX_GL_MAX_DIM=N` lowers the GPU limit the backend
plans with (never raises it), to check a layout on a smaller GPU.

**Evidence.** `gl_scale_invariance_test` forces the window at 2, 3, 5 and 9x
and requires the frame at internal resolution to be byte-identical to the
full-VRAM surface at the same scale (copies inside, into and across the
window edge, fills and uploads across it, mask set/check, all four blend
modes), and the guest-visible VRAM to be identical to 1x; it also runs the
window at 18x. Its side-by-side runs flip two 512-wide buffers: one surface at
5x and two tiles at 9x under `PSX_GL_MAX_DIM=8192` must equal the full-VRAM
surface, including copies between the buffers and 1000-column copies staged
in chunks; at 18x both buffers must read back at S with two tiles. In R4 on an M4, the 8K preset reports
`effective_scale 18, internal_lines 4320, hr_scale 1, hires_fbo 5760x9216`, and
`screenshot_hires` in a race is 5760×4320 with the rear-view mirror present.

## DD1 — Opt-in draw-distance clamps (2026-10-01)

`[[draw_distance.clamp]]` (docs/config_schema.md) lists a title's
ordering-table range guards. While a trusted mod has called
`psx_mod_set_draw_distance_clamp(1)`, the guard's depth index is clamped to
the last safe slot before the guard runs, so far geometry the game would drop
stays in the farthest bucket (drawn first, under everything nearer) instead
of popping in later. Raising the limit instead can push a biased primitive
past the end of the table. Off by default and at every session start;
identity when off; main executable only (overlay code keeps its own code);
native code and the dirty-RAM interpreter agree
(`draw_distance_codegen_test`, `draw_distance_interp_test`). First user: R4's
course renderers (RidgeRacerType4Recomp, Max Detail).

### Opt-in camera-plane clipping for native-wide textured worlds

`psx_mod_set_native_wide_near_clip(1)` complements horizontal projection
recovery. A large face can remain visible while a corner crosses the camera
plane: unsigned SZ, the capped H/Z divider and saturated SXY cannot represent
that face. PGXP now optionally carries the signed homogeneous projection from
RTPS/RTPT through exact full-word copies. CPU arithmetic, partial writes,
stale words and timeline invalidation discard the association; sandbox
rollback restores it with the rest of the precision shadow.

Opted-in native-wide OpenGL sessions clip proven textured faces at depth 1
and the visible bounds before perspective division. UV and color attributes
follow each intersection, and ordinary GPU batches, painter order, masking,
texture filtering and canonical/wide readback remain in use. The existing
guest GTE registers, gameplay state and title culling branches do not change.
Software/CPU-authoritative rendering, 4:3 and missing provenance use the
original path. The larger PGXP value increases the full 8 MiB RAM shadow
from 40 to 72 MiB; projection calculations are disabled unless a title opts in.

MediEvil II's title hallway provided the camera-crossing reproduction. The
GL authority/order fixture passes 197 checks at 1x and 4x; provenance tests
cover negative depth, stale packets, identical partial writes, rollback and
mode guards. Live captures remove the previously observed triangular holes;
the owner confirmed the hallway walls stay intact. Broader map coverage
remains separate from this reproduction.
