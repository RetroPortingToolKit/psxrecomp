# Axis 5 — SPU (Audio) Accuracy Findings

Cross-reference of our SPU implementation against the in-tree Beetle/Mednafen
oracle and nocash psx-spx.

- **Our impl:** `runtime/src/spu/spu.c` (799 LOC) + `runtime/src/spu/spu_shadow.c` (float
  resample shadow, opt-in, verify-only — irrelevant to hardware accuracy).
- **Oracle:** `beetle-psx/mednafen/psx/spu.cpp`
  (1664 LOC, `PS_SPU`).
- **Spec:** nocash psx-spx "Sound Processing Unit (SPU)".

The file's own header comment is candid (`spu.c:1-8`): *"Reverb, noise, sweep
volumes, and IRQ timing are not modeled yet."* That summary is accurate and is
the headline of this axis. We are a **compact register + direct-ADPCM voice
mixer**, not a hardware-faithful SPU.

---

## 1. What our implementation does

### Implemented (and mostly faithful)
- **24 voices** — `SPU_VOICE_COUNT 24`, mixed in `spu_render` (`spu.c:521-599`).
- **ADPCM block decode** — `decode_block` (`spu.c:298-344`): 28-sample blocks,
  shift/filter header, 4-tap → 2-tap Pochobit filter coefficients
  `f0/f1 = {0,60,115,98,122}/{0,0,-52,-55,-60}`. **Matches Beetle's `Weights[]`
  exactly** (`spu.cpp:297-305`). Loop-start flag (0x04) latches `repeat_addr`;
  loop-end flag (0x01) handled in `voice_next_sample`.
- **ADSR envelope** — `calc_vc_delta` (`spu.c:110-142`) is a **verbatim port** of
  Beetle's `CalcVCDelta` (`spu.cpp:187-225`); `adsr_run` (`spu.c:146-212`) mirrors
  `PS_SPU::RunEnvelope` (`spu.cpp:478-537`) including the four phases, the
  exp/linear `log_mode`/`dec_mode` selectors per phase, the `uoflow_reset` clamp,
  and the decay→sustain transition at `sustain_lvl = (Sl+1)<<11`. Rate decoding
  (Ar 0x7F, Dr<<2, Rr<<2, Sr) matches `CacheEnvelope` (`spu.cpp:436-458`). **This
  is the strongest part of our SPU.**
- **Key-on / key-off** — `key_on` resets envelope to Attack/level-0
  (`spu.c:464-482`, matches `ResetEnvelope` `spu.cpp:460-467`); `key_off` →
  Release preserving level (`spu.c:488-497`, matches `ReleaseEnvelope`
  `spu.cpp:469-475`). KEYON clears ENDX bit (`spu.c:479`), correct per psx-spx.
- **ENDX latch** — `endx_latch` set on loop-end block, readable at 0x1F801D9C/9E
  (`spu.c:635-640`), cleared by KEYON. Functionally correct for poll-driven
  music engines.
- **CURVOL read-back** — voice reg 6 returns live `env_level` (`spu.c:646-651`),
  matching Beetle's `case 0x0C: return ADSR.EnvLevel` (`spu.cpp:1299-1300`). Good;
  this is what lets BIOS pick free voices.
- **CD audio input** — separate 44.1 kHz stereo ring (`spu_cd_audio_push`,
  `spu.c:262-287`); mixed when SPUCNT bit0 set, scaled by CD vol regs
  (`spu.c:561-569`).
- **DMA / transfer** — `spu_dma_write` (word) + manual 0x1F801DA8 fifo write +
  0x1F801DA6 transfer-addr latch (`spu.c:682-706`).
- **Pitch** — per-voice pitch register applied as a 12-bit-fractional phase
  accumulator (`spu.c:453-460`).

### Stubbed / missing / wrong (detailed in §2)
- **Reverb engine — entirely absent.** No all-pass/comb, no `RunReverb`, no
  reverb register set (dAPF/IIR/ACC/FB/MIX coefficients), no reverb work area.
- **Noise generator — entirely absent.** No LFSR, no `RunNoise`, NON register
  read but never used in the mix.
- **Pitch modulation (FM) — absent.** PMON register stored but never applied.
- **Volume sweeps — absent.** `direct_volume` (`spu.c:232-244`) explicitly drops
  sweep mode ("Sweep mode is not modeled; use the magnitude as a direct volume").
  No `SPU_Sweep` class equivalent. Main volume also uses `direct_volume`.
- **SPU IRQ (address match) — absent.** No `IRQAddr` compare, no `IRQ_Assert`.
- **Capture buffers — absent.** No CD-left/CD-right/voice1/voice3 writeback to
  SPU RAM at 0x000/0x200/0x400/0x600.
- **SPUSTAT — hardcoded** `0x0400` (`spu.c:627-628`), ignores SPUCNT mirror, IRQ
  flag, DMA/transfer state.
- **Timing model — host-audio-driven, not guest-clock-driven** (see §2.1).

---

## 2. Specific discrepancies vs Beetle `spu.cpp` + psx-spx

### 2.1 Timing model: host-pull vs guest 768-cycle clock (ARCHITECTURAL)
- **Beetle:** `UpdateFromCDC(clocks)` (`spu.cpp:761-1072`) advances a
  `clock_divider` seeded at 768 (`spu.cpp:105`); one stereo output sample is
  produced every 768 SPU clocks, lock-stepped to the guest CPU/CDC timeline.
  Voice decode, envelope, sweep, noise, reverb, and capture all clock once per
  guest sample.
- **Ours:** `spu_render(out, frames)` is called from the **SDL host audio pump**
  (`main.cpp:630`), driven by how many frames the host audio queue needs — not
  by guest cycles. Envelope/pitch/decode are stepped once per *output* frame
  inside `spu_render` (`spu.c:542-589`), decoupled from guest time.
- **Consequence:** envelope/release *rates* drift relative to the game's sense
  of time (a release programmed in "guest samples" elapses in "host frames"),
  voices advance whenever the host asks for audio rather than when the guest
  clocks the SPU, and there is no deterministic, oracle-comparable per-sample
  timeline. This is the root reason a sample-stream diff vs Beetle (§4) will not
  line up phase-accurately even where the per-voice math is correct. This is the
  single largest structural divergence.

### 2.2 Reverb — completely missing (player-audible)
- **Beetle:** full reverb — `RunReverb` (`spu.cpp:652-730`) with FIR up/down
  resamplers (`Reverb4422`/`Reverb2244`, `ResampTable[20]`), the IIR/comb/all-pass
  network (`IIR_INPUT_A/B`, `IIR_A/B`, `ACC`, `FB_A/B`, `MDA/MDB/IVB`), reverb
  work-area wrap (`Get_Reverb_Offset` `spu.cpp:577-585`), per-voice reverb-send
  via `Reverb_Mode` (EON, `spu.cpp:884-888`), and the full reverb register set
  written through `ReverbRegs[]` (dAPF1/2, IIR_ALPHA/COEF, ACC_COEF_A..D,
  FB_SRC/ALPHA/X, IIR_SRC/DEST, ACC_SRC, MIX_DEST, IN_COEF).
- **Ours:** none of this exists. EON is read into `SpuGlobalState.eon`
  (`spu.c:748-749`) for debug only; reverb registers (0x1F801DC0–0x1F801DFF) are
  stored in `spu_regs[]` but never read. `ReverbVol`/`ReverbWA` ignored.
- **Impact:** every game that uses SPU reverb (most — ambience, halls, caves,
  menu chimes) loses its reverb tail. Audible "dry" sound.

### 2.3 Noise generator — missing
- **Beetle:** `RunNoise` (`spu.cpp:733-759`) clocks the 15-bit LFSR per the
  noise-frequency field of SPUCNT (`(SPUControl>>8)&0x3F`); voices with
  `Noise_Mode` bit set output `(int16)LFSR` instead of ADPCM (`spu.cpp:854-855`).
- **Ours:** NON is stored to `SpuGlobalState.non` (`spu.c:746-747`) but never
  consulted. A noise-mode voice in our mixer still plays whatever ADPCM its
  address points at — wrong waveform entirely.
- **Impact:** drums/cymbals/explosions/wind that use the hardware noise channel
  play as garbage ADPCM or silence. Game-dependent but common.

### 2.4 Pitch modulation (FM / PMON) — missing
- **Beetle:** `PhaseModCache = FM_Mode & ~1` (`spu.cpp:796`); a PMON voice's
  `phase_inc = Pitch + ((int16)Pitch * (prev voice PreLRSample))>>15`
  (`spu.cpp:907-914`) — the previous voice's output modulates this voice's pitch.
- **Ours:** PMON stored to `SpuGlobalState.pmon` (`spu.c:744-745`) but pitch is
  always the raw register (`spu.c:453-455`). No `PreLRSample` chain.
- **Impact:** FM-synth instruments (some music engines, sound effects) play at a
  fixed pitch instead of the intended sweep/vibrato.

### 2.5 Volume sweeps — missing (player-audible)
- **Beetle:** `SPU_Sweep` (`spu.cpp:228-288`) clocks an exponential/linear sweep
  per voice L/R and for main volume when bit15 set, else uses
  `(Control&0x7FFF)<<1` as the static volume (`spu.cpp:891-897, 1059-1066`).
- **Ours:** `direct_volume` (`spu.c:232-244`) takes the magnitude of sweep-mode
  values as a fixed level and otherwise treats the field as signed direct
  volume. No clocking, no ramp. **Note also the scale mismatch:** Beetle's static
  path is `(Control&0x7FFF)<<1` (15-bit volume → 16-bit) and the voice mix is
  `>>15` (`spu.cpp:878-879`); ours clamps to ±0x3FFF and mixes `>>14`
  (`spu.c:558-559, 241-242`). Net per-voice gain differs by roughly a factor of 2
  and ignores the sign of negative volumes in sweep mode.
- **Impact:** any fade-in/fade-out done via volume sweep (extremely common for
  music note envelopes layered on top of ADSR, and for whole-mix fades) is
  replaced by a static level — notes pop in/out instead of swelling, and overall
  balance is off due to the gain-scale mismatch.

### 2.6 SPU IRQ (address match) — missing
- **Beetle:** `CheckIRQAddr`/`RunDecoder` assert `IRQ_SPU` when the decode/read/
  write address equals `IRQAddr` and SPUCNT bit6 is set (`spu.cpp:309-317,
  366-374, 539-549`); cleared when bit6 cleared (`spu.cpp:1242-1246`).
- **Ours:** `IRQAddr` register is stored in `spu_regs[]` (0x1F801DA4) but never
  compared; no interrupt is ever raised; SPUSTAT bit6 (IRQ flag) is hardcoded 0.
- **Impact:** games/music drivers that use SPU IRQ for streaming/loop
  synchronization (sample-accurate retriggering, some XA/stream engines) will
  never get their interrupt and can stall or mistime audio.

### 2.7 Capture buffers — missing
- **Beetle:** writes CD-DA L/R to SPU RAM 0x000/0x200 and voice1/voice3 output to
  0x400/0x600 each sample, advancing `CWA` (`spu.cpp:870-874, 1012-1013, 1031`).
- **Ours:** no capture writeback. SPU RAM only ever receives DMA/fifo writes.
- **Impact:** games that read the capture region (some reverb/echo tricks,
  oscilloscope visualizers, a few music engines that re-read voice output) get
  stale/zero data.

### 2.8 SPUSTAT (0x1F801DAE) — hardcoded
- **Beetle:** `SPUStatus = SPUControl & 0x3F`, plus IRQ bit (0x40), plus the
  capture-half bit and DMA/transfer-mode bits (`spu.cpp:823-827`).
- **Ours:** always returns `0x0400` (`spu.c:627-628`). The low-6-bit SPUCNT
  mirror, IRQ flag, and transfer-busy/half bits are absent.
- **Impact:** BIOS/driver code that polls SPUSTAT for transfer-ready or IRQ
  status reads a constant; mostly tolerated because `spu_dma_ready` always
  returns 1, but it is not faithful and can mis-sequence transfer-mode changes.

### 2.9 ADPCM shift==invalid handling differs (minor)
- **Beetle:** `shift > 12` → `shift = 8; CV &= 0x8888` (`spu.cpp:406-410`,
  preserves only the sign nibble of each sample).
- **Ours:** `if (shift > 12) shift = 12` (`spu.c:310`) and decodes normally.
- **Impact:** rare (valid streams never set shift>12), but a malformed/edge block
  decodes to different samples than hardware.

### 2.10 "End without repeat" garbage-decode model differs (minor)
- **Beetle:** on loop-end without loop bit, sets `CurAddr = LoopAddr`, forces
  envelope to Release **and** EnvLevel=0 immediately (`spu.cpp:341-353`).
- **Ours:** sets Release but keeps the current level decaying, and keeps decoding
  forward through whatever follows in RAM (`spu.c:398-413`, documented as
  "garbage masked by dying envelope"). Audibly close but not bit-identical, and
  the forward-decode (vs Beetle's jump to LoopAddr) reads different SPU RAM.

### 2.11 IgnoreSampLA / manual LoopAddr write — not modeled (minor)
- **Beetle:** a manual write to voice LoopAddr (reg 0x0E) sets `IgnoreSampLA`,
  preventing the loop-start flag from overwriting the program-set loop address
  (`spu.cpp:1149-1152, 383-387`).
- **Ours:** `repeat_addr` is always overwritten by the 0x04 flag (`spu.c:330`)
  and initialized from reg 7 at key-on (`spu.c:471`); a mid-sample manual loop
  override is lost.
- **Impact:** music engines that set a custom loop point after key-on loop to the
  wrong place. Game-dependent.

---

## 3. Prioritized fix list (player-audible impact first)

1. **P0 — Implement reverb (§2.2).** Largest universally-audible gap. Port
   Beetle's `RunReverb` + `SPU_Sweep`-independent reverb register decode +
   `Get_Reverb_Offset`/work-area + per-voice EON send + `ReverbVol` mix. Wholesale
   port from `spu.cpp:564-730` is the faithful path.
2. **P0 — Implement volume sweeps (§2.5) and fix the gain scale.** Port
   `SPU_Sweep` (`spu.cpp:228-288`), drive per-voice L/R and main/global volume
   through it, and reconcile the `>>14` vs `>>15` / `<<1` scaling so absolute
   levels match hardware. Affects every note envelope and every fade.
3. **P1 — Implement the noise generator (§2.3).** Port `RunNoise`
   (`spu.cpp:733-759`) + LFSR, and select LFSR output for `Noise_Mode` voices in
   the mix. Needed for correct percussion/SFX.
4. **P1 — Move to the guest-clock timing model (§2.1).** Drive SPU sample
   generation from a 768-cycle divider off the global guest-cycle counter
   (mirror `UpdateFromCDC`), buffering output for the host audio pump, instead of
   pulling per host-callback. Prerequisite for deterministic oracle diffs and for
   correct envelope/reverb/noise *rates*. Aligns with FAITHFUL_TIMING_PLAN.md
   (devices on the global guest-cycle counter).
5. **P1 — Implement SPU IRQ address match (§2.6).** Compare decode/transfer
   addresses against `IRQAddr` under SPUCNT bit6 and raise IRQ_SPU; reflect in
   SPUSTAT bit6 and clear on bit6 clear. Needed for stream/loop-sync engines.
6. **P2 — Pitch modulation / PMON (§2.4).** Add the `PreLRSample` chain and
   PMON-gated `phase_inc` (`spu.cpp:907-914`).
7. **P2 — Capture buffers (§2.7).** Write CD-DA + voice1/voice3 to
   0x000/0x200/0x400/0x600 with `CWA` advance.
8. **P2 — Real SPUSTAT (§2.8).** Compose from SPUCNT mirror + IRQ + transfer
   state instead of the constant.
9. **P3 — Edge cases:** shift>12 handling (§2.9), end-without-repeat
   LoopAddr jump + immediate EnvLevel=0 (§2.10), IgnoreSampLA manual-loop
   override (§2.11).

The whole-file faithful path (per Rule -1 / RULE -1: no surgical per-game
patches) is to **port Beetle's `PS_SPU` core wholesale** — decode, envelope,
sweep, noise, reverb, capture, IRQ, SPUSTAT — driven by the guest clock, and keep
only our integration shims (event ring, CD ring, shadow tap, debug snapshots).
The current file already shares the exact decode filter and ADSR math with
Beetle, so a wholesale port is low-risk and removes the entire "what's faithful"
guessing surface in one pass rather than re-deriving each subsystem.

---

## 4. Validation method

Use the two-process harness (CLAUDE.md §16): `psx-runtime` on **port 4500**
(this is the Tomba2 build per MEMORY/`build-t2`; generic BIOS build is 4370) and
`psx-beetle` on **port 4382/4380**. Ring-buffer-first per global rules — never
arm-then-capture.

1. **SPU output sample-stream diff (the primary audible test).**
   - Add an **always-on SPU output ring** on both sides capturing the final
     stereo sample stream (post-mix, pre-host-resample). Beetle already has
     `IntermediateBuffer[4096][2]` (`spu.cpp:76-77, 1048-1055`) — expose it as a
     ring over the debug protocol. Our side: tap `spu_render`'s `out_stereo`
     (or better, the proposed guest-clock sample producer) into a matching ring.
   - Drive both to the same scene (same disc, same input script), then query both
     rings for the same guest-frame window and diff sample-by-sample. Expect:
     today, large divergence once reverb/noise/sweep-driven content plays;
     success = bounded error within ADPCM/round-off tolerance after the P0–P1
     fixes. Note the §2.1 timing model means phase alignment requires the
     guest-clock fix (P1) before a tight diff is meaningful — until then, compare
     **per-voice envelope/pitch** (below) and **spectral/energy envelopes** rather
     than raw sample phase.

2. **Per-voice envelope + register state diff (deterministic, available now).**
   - Both sides already emit a frame-stamped **SPU event ring**
     (`psxrecomp_beetle_spu_event` KEYON/KEYOFF/END_STOP/END_LOOP — `spu.c:87-102`
     and `spu.cpp:337-359, 931-958`). Diff KEYON/KEYOFF voice+addr+env timelines
     between ports: this validates voice allocation, loop/end detection, and
     envelope level at transitions without needing sample-phase alignment.
   - Compare `spu_get_voice_state` / `spu_get_global_state` (`spu.c:716-755`)
     against Beetle's `GetRegister` (`spu.cpp:1507-1580`, GSREG_* incl. ADSR_LEVEL,
     reverb regs, FM/NOISE/REVERB_ON, sweep volumes) per voice, per frame, for:
     EnvLevel, Phase, CurAddr, Pitch, and (once implemented) sweep Current, LFSR,
     ReverbCur. ENDX/BlockEnd compare directly.

3. **Subsystem-specific oracles after each fix.**
   - **Reverb:** with a reverb-heavy scene, compare the reverb work-area region of
     SPU RAM (read via debug RAM read on both sides) and the `ReverbCur` pointer;
     then the reverb-send mix energy.
   - **Noise:** pin `LFSR` and `NoiseDivider`/`NoiseCounter` step-for-step against
     `RunNoise` after seeding identical SPUCNT noise-freq.
   - **IRQ:** set `IRQAddr` in a known scene and confirm both raise IRQ_SPU at the
     same guest frame (interrupt ring).
   - **Capture:** read SPU RAM 0x000/0x200/0x400/0x600 on both sides and diff.

4. **Regression guard.** Keep the ADSR `calc_vc_delta` byte-identical to Beetle's
   `CalcVCDelta` (already true) and add a unit test that drives both through the
   full rate range 0..127 in each phase, asserting identical (increment, divinco)
   pairs — this is the one part proven faithful and must not regress during the
   wholesale port.
