# Axis 5 — MDEC (Motion Decoder / FMV) accuracy cross-reference

Scope: `runtime/src/mdec/mdec.c` (our impl) vs the in-tree Beetle/Mednafen oracle
`beetle-psx/mednafen/psx/mdec.cpp`, plus nocash
psx-spx "Macroblock Decoder (MDEC)". READ-ONLY analysis; no source/build touched.

References below cite `mdec.c:NN` for ours and `mdec.cpp:NN` for Beetle.

---

## 1. What our implementation does

Single file: `runtime/src/mdec/mdec.c`. State in one `static MDECState mdec` struct.

Implemented (not stubbed):
- **Command dispatch** — `begin_command` (`mdec.c:381`) / `execute_command`
  (`mdec.c:357`). Decodes `cmd>>29`: 1=DECODE, 2=SET_QUANT, 3=SET_SCALE.
  Output flags latched from the command word: bit15 (`>>25`), signed (`>>26`),
  depth (`>>27 & 3`).
- **SET_QUANT** (`mdec.c:359`) — copies 64 luma bytes, plus 64 chroma bytes
  when `cmd&1`, else mirrors luma into chroma.
- **SET_SCALE** (`mdec.c:370`) — loads 64 int16 IDCT scale-table entries.
- **DECODE** (`execute_decode`, `mdec.c:309`) — RLE decode per block, IDCT,
  YUV→RGB, packs to the output buffer. Colour path (depth>=2) decodes block
  order Cr,Cb,Y0..Y3 per macroblock (`mdec.c:338`). Mono path (depth<2)
  decodes luma-only blocks (`mdec.c:322`).
- **RLE decode** — `decode_rle_block` (`mdec.c:220`): DC then run/level pairs,
  zig-zag scatter, dequant.
- **IDCT** — `idct_block` (`mdec.c:198`): two separable passes.
- **YUV→RGB** — `append_rgb_pixel` (`mdec.c:263`), fixed-point coefficients.
- **Output packing** — 15bpp (depth 3) and 24bpp (depth 2) in
  `append_rgb_pixel`; 8bpp/4bpp luma via `append_luma_block` (`mdec.c:284`).
- **Status register** — `mdec_read` (`mdec.c:445`): builds bits 30/29/28/27/23/24/25/31
  + current-block + remaining-words.
- **DMA coupling** — `mdec_dma_write_word`/`mdec_dma_read_word`
  (`mdec.c:487`), ready predicates (`mdec.c:513`), wired into async channels
  0/1 in `dma.c` (`advance_mdec_channel`, `dma.c:473`; `DMA_MDEC_OUT_CYCLES_PER_WORD=14`).
- Debug trace ring + FMV-activity stamp (`mdec_recently_active`).

Architectural difference from hardware/Beetle: **we buffer the entire DECODE
input, decode the whole frame in one shot, then stream the output buffer to
DMA.** Beetle is a true streaming state machine (`MDEC_Run`, `mdec.cpp:526`)
with 32-word In/Out FIFOs, decoding block-by-block as words arrive and as the
Out FIFO drains. This is the root of several discrepancies below.

---

## 2. Discrepancies vs Beetle mdec.cpp + psx-spx

### D1 — [HIGH] DC coefficient dequant is wrong (no rounding, wrong scale)

Ours (`mdec.c:231`):
```
qscale = (word>>10)&0x3F;  coeff = sign_extend_10(word&0x3FF);
coeff = (qscale==0) ? coeff*2 : coeff * quant[0];     // <-- DC uses quant[0] only
block[0] = clamp(coeff, -0x400, 0x3FF);
```
Beetle DC path (`mdec.cpp:439-453`, `CoeffIndex==0`):
```
QScale = V>>10;
q = QMatrix[qmw][0];                 // No QScale on DC, matches us
if(q != 0) tmp = ((ci*q)<<4) + (ci? (ci<0?8:-8):0);   // <<4 fixed point + bias
else       tmp = (ci*2)<<4;
Coeff[0] = clamp(tmp, -0x4000, 0x3FFF);
```
Differences:
1. Beetle keeps coefficients in a **<<4 fixed-point domain** (clamp ±0x4000),
   ours stores plain integers (clamp ±0x400/0x3FF). The IDCT scale tables are
   tuned to the chosen domain, so the absolute brightness/contrast of every
   block depends on this matching. (See D4 — our IDCT divides scale by 8, a
   private convention; the magnitudes still must line up with the dequant.)
2. Beetle adds a **sign-dependent rounding bias** (`ci<0?8:-8`) even on the DC
   term; ours adds nothing on DC. Hardware applies the bias on every nonzero
   coefficient. Missing bias = systematic DC offset → block-level brightness
   banding in FMV.
3. Our clamp window (`-0x400..0x3FF`) is asymmetric and an order of magnitude
   smaller than Beetle's `-0x4000..0x3FFF`; large DC values saturate
   prematurely → flat/clipped bright or dark blocks.

### D2 — [HIGH] AC coefficient dequant formula and bias differ

Ours (`mdec.c:248`): `coeff = (coeff * quant[k] * qscale + 4) / 8;`
Beetle (`mdec.cpp:475-485`):
```
q = QScale * QMatrix[qmw][CoeffIndex];
if(q != 0) tmp = (((ci*q) >> 3) << 4) + (ci<0?8:-8);   // >>3 then <<4, sign bias
else       tmp = (ci*2) << 4;
Coeff = clamp(tmp, -0x4000, 0x3FFF);
```
psx-spx: `val = (coeff * q * scale + 4) / 8` *then* `val = signed-saturate to
the matrix domain`, with the val multiplied by scale tables in IDCT. Two
concrete divergences:
1. **Rounding term.** Ours uses `+4`/`/8` (round-half-up, symmetric). Beetle
   uses an **arithmetic `>>3` (round toward −∞) plus a sign-magnitude bias
   `±8` in the <<4 domain.** These give different LSBs on roughly half of all
   coefficients → per-pixel chroma/luma jitter, visible as dither/banding
   differences in flat gradients.
2. **Domain/clamp** as in D1 — ours ±0x400, Beetle ±0x4000 (<<4). Premature
   AC saturation softens or rings high-detail blocks.
3. Note psx-spx and Beetle agree the AC `q = scale * matrix[k]`; ours computes
   the same product but in the wrong fixed-point domain.

### D3 — [HIGH] Color block ordering vs RAMOffset / "current block" status field

- Decode block order Cr,Cb,Y0,Y1,Y2,Y3 is **correct** (matches Beetle's
  `DecodeWB` 0=Cr,1=Cb,2..5=Y and psx-spx). Good.
- BUT Beetle's status "current block" field is `(DecodeWB + 4) % 6`
  (`mdec.cpp:518`, commented-out in status at `mdec.cpp:772` but used to drive
  `EncodeImage`). Ours reports `mdec.current_block` which is only ever set to 4
  in `begin_command`/`soft_reset` (`mdec.c:386`,`195`) and **never advances
  during decode**. The status block-number bits (16..18) are therefore static.
  Games that poll the current-block field will read a frozen value. Low FMV
  impact (most use DMA), but it is an inaccurate status field. (psx-spx
  documents bits 16-18 as "current block (4=Y1,5=Y2,6=Y3,7=Y4? ...)".)

### D4 — [HIGH] IDCT structure and rounding differ from Beetle/psx-spx

Ours (`mdec.c:198`):
```
sum += src[y + z*8] * (scale[x + z*8] / 8);     // pre-divide each tap by 8
dst[x + y*8] = (sum + 0xFFF) / 0x2000;           // round, /8192
```
Beetle (`mdec.cpp:243-291`): two 1-D passes, each `sum += in * IDCTMatrix`,
output `(sum + 0x4000) >> 15`; the SET_SCALE loader pre-shifts the matrix by
`>> 3` (`mdec.cpp:647`). Pass 2 produces int8 with `Mask9ClampS8` (9-bit mask
then clamp to ±127), pass 1 stays int16 (no clamp).
Divergences:
1. **Rounding constant / shift.** Ours `(sum+0xFFF)/0x2000` = round add 0xFFF,
   divide 8192. Beetle `(sum+0x4000)>>15` = round add 0x4000, divide 32768.
   Combined with our per-tap `/8` and Beetle's matrix `>>3`, the net scaling
   *may* coincide, but the **rounding biases do not** (0xFFF vs 0x4000 over
   different divisors → different half-LSB behavior). This is the classic
   source of "FMV looks slightly washed / wrong contrast vs real HW."
2. **Per-tap truncation.** Ours divides `scale[x+z*8]/8` *inside* the inner
   loop — truncating each matrix tap to integer *before* multiply. Beetle
   pre-shifts the matrix once at load (`>>3`, also truncating) — so both
   truncate, but ours truncates a per-use copy while computing with already
   sign-extended `int16`. Equivalent only if `scale[]` were already the >>3
   values; ours stores the **raw** SET_SCALE int16 (`mdec.c:371`) and divides
   by 8 at IDCT time, Beetle stores the **already-/8** matrix. If a game
   uploads the standard PSX scale table both reach similar magnitudes, but the
   intermediate clamp differs (see next).
3. **Intermediate clamp.** Beetle clamps **pass 1 to int16 (implicit) and pass
   2 to signed 9-bit then ±127** (`Mask9ClampS8`, `mdec.cpp:230`,`266`,`277`).
   Ours stores both passes into `int16 tmp[64]` with **no clamp** until the
   YUV/luma output stage. Missing the 9-bit mask between passes changes
   overflow/wrap behavior on high-energy blocks → ringing artifacts differ.
4. **Index/transpose layout.** Ours swaps src/dst between passes and indexes
   `src[y+z*8]`/`dst[x+y*8]`; Beetle transposes explicitly (`out[(x*8)+col]`
   on pass 1, `out[(col*8)+x]` on pass 2). Need a numeric block-diff (Section
   4) to confirm the two produce the same sample at the same (x,y); the
   asymmetric indexing is a likely transpose mismatch.

### D5 — [HIGH] YUV→RGB coefficients, rounding, and green truncation differ

Ours (`mdec.c:263`):
```
r = y + ((1436*cr)         >> 10);
g = y - ((352*cb + 731*cr) >> 10);
b = y + ((1815*cb)         >> 10);
```
Beetle (`mdec.cpp:293-304`):
```
r = ClampS8(y + (((359*cr)            + 0x80) >> 8));
g = ClampS8(y + ((((-88*cb)&~0x1F) + ((-183*cr)&~0x07) + 0x80) >> 8));
b = ClampS8(y + (((454*cb)            + 0x80) >> 8));
r ^= 0x80; g ^= 0x80; b ^= 0x80;        // unsigned/signed handled via xor
```
Divergences:
1. **Coefficients/scale.** Ours uses /1024 fixed point (1436,352,731,1815);
   Beetle uses /256 (359, 88/183, 454). 1436/1024=1.402, 359/256=1.402 — same
   target ratios, but the **rounding +0x80 and the shift width differ**, so the
   per-pixel results differ by ±1 routinely.
2. **Green-channel bit masking.** Beetle deliberately masks the chroma
   contributions: `(-88*cb) &~ 0x1F` and `(-183*cr) &~ 0x07`. This reproduces
   the real MDEC's *reduced-precision green* path (nocash notes green is
   computed with fewer bits). **Ours has no such masking** → our green channel
   is too precise vs hardware; FMV greens will be subtly off-hue compared to a
   real PS1 / Beetle. This is a documented, hardware-faithful quirk we are
   missing.
3. **Clamp before pack.** Beetle clamps each channel with `Mask9ClampS8`
   (9-bit signed mask → ±127) *then* xors 0x80 to get 0..255. Ours
   `to_output_u8` (`mdec.c:257`) clamps to −128..127 then `+128`. The clamp
   bound is the same effective range, but ours has **no 9-bit mask**, so an
   out-of-9-bit intermediate wraps differently before clamping.

### D6 — [MED] 4bpp / 8bpp luma output formula differs

Ours mono path (`append_luma_block`, `mdec.c:284`): emits `to_output_u8(y)` —
one byte per pixel, signed/unsigned via the global output_signed, and **only an
8bpp byte stream; there is no 4bpp packing at all.** Depth<2 (`mdec.c:322`)
treats 4bpp (depth 0) and 8bpp (depth 1) identically.
Beetle (`EncodeImage`, `mdec.cpp:332-368`):
- 4bpp (case 0): `min(127, y+8)`, take high nibble, **pack two pixels per
  byte**, xor 0x88/0x00 for unsigned/signed.
- 8bpp (case 1): `y ^ (0x80 or 0x00)`, one byte/pixel.
psx-spx confirms 4bpp packs nibbles. **Our 4bpp output is wrong: not packed,
no +8 bias, wrong byte count** → any title using MDEC for 4-bit texture
decompression gets garbage. (Tomba-family uses colour FMV so FMV itself is
unaffected, but texture-decode paths would corrupt.)

### D7 — [MED] Status register bit layout / semantics

`mdec_read` (`mdec.c:445`) vs Beetle `MDEC_Read` (`mdec.cpp:755`):
- We OR `output_bit15<<23 | output_signed<<24 | output_depth<<25`. Beetle does
  `((Command>>25)&0xF)<<23` (`mdec.cpp:770`) — i.e. it mirrors **4 command bits
  (25..28)** into status 23..26 directly. Ours reconstructs the same bits but
  drops command bit 28 (the "data-output-depth high / data-in request format"
  region). Verify bit 26 alignment: ours puts depth (2 bits) at 25-26, Beetle
  at 25-26 via the 23-shift of bits 25-28 — alignment likely OK but **bit 23
  source differs** (ours = bit15-out flag; Beetle = command bit 25). Needs
  pinning against psx-spx (bits 23-28 = "current output settings").
- **DMA-request bits.** Ours gates bit27 (data-out request) on
  `enable_dma_out && mdec_dma_read_ready()` and bit28 (data-in request) on
  `enable_dma_in && write_ready` (`mdec.c:463`). Beetle gates bit28 on
  `InFIFO.CanWrite()>=0x20 && Control bit30 && InCommand && InCounter!=0xFFFF`
  and bit27 on `OutFIFO.in_count>=0x20 && Control bit29` (`mdec.cpp:701-709`).
  Hardware asserts the DMA-request bits in **32-word (0x20) FIFO granularity**;
  ours asserts on any single ready word. A game using the request bits to pace
  block DMA (rather than a fixed word count) would see different timing.
- **Remaining-words (bits 0-15).** Ours derives from
  `(expected_halfwords-input_count+1)/2` (`mdec.c:453`); Beetle returns the live
  `InCounter` (`mdec.cpp:774`). Because we one-shot decode, our remaining-words
  collapses instantly after the input is buffered, whereas hardware counts down
  across the whole transfer.
- **Bit 29 (command busy)** ours = `mdec.busy`; Beetle = `InCommand`.
  Semantically close but ours clears in `finish_command` immediately after the
  one-shot decode, so the busy window is far shorter than hardware.

### D8 — [MED] No FIFO model → DMA-can-write/read granularity and overrun behavior

Ours has no 32-word In/Out FIFO. `mdec_dma_write_ready` (`mdec.c:513`) returns
true whenever output is drained and the command can still take input;
`mdec_dma_read_ready` true whenever output has bytes. Beetle exposes true 0x20
FIFO fill levels and `MDEC_DMACanWrite/Read` require `>=0x20`. Consequences:
- The "GameShark 4.0 abuse of DMA channel 0" and FF7-intro edge cases Beetle
  calls out (`mdec.cpp:49-52`) rely on FIFO-fill semantics we don't reproduce.
- Read underflow: ours returns zero-padded words and counts underflows
  (`mdec.c:500`); hardware/Beetle would simply not assert the read-request bit
  (Out FIFO `in_count` stays < 0x20). Different stall/idle behavior under DMA.

### D9 — [MED] 0xFE00 (end-of-block / EOB) handling differs

Ours (`decode_rle_block`, `mdec.c:227`): **skips leading 0xFE00 words** before
the DC (`while word==0xFE00 advance`), and on a mid-block 0xFE00 just `break`s
(leaving the rest of the block zero — fine).
Beetle (`WriteImageData`, `mdec.cpp:433`,`458`): a 0xFE00 **before any coeff
(`CoeffIndex==0`) is ignored (return) without consuming a block slot**, and a
0xFE00 mid-block zero-fills the remaining zig-zag entries and finishes the block
(`mdec.cpp:460`). psx-spx: 0xFE00 = "end of block", padding. Our leading-EOB
skip loops within the same block buffer, but because we one-shot over the whole
input the **alignment of which 0xFE00 ends which block can drift** vs the
streaming model, especially when a frame has trailing 0xFE00 padding between
macroblocks. This can desync block ordering on malformed/edge streams.

### D10 — [LOW] DC sign-extend width and RLE level width

Both use 10-bit signed level (ours `sign_extend_10`, Beetle `sign_10_to_s16`) —
**agree.** Run length is `word>>10` in both (ours adds `+1`, `mdec.c:241`;
Beetle advances `CoeffIndex` by `rlcount` then writes one coeff — net same
zig-zag advance). No discrepancy, listed for completeness.

### D11 — [LOW] SET_SCALE entry transform

Ours stores `(int16)input[i]` raw (`mdec.c:371`). Beetle stores
`(int16)(tfr&0xFFFF) >> 3` (`mdec.cpp:647`) — pre-shifted. Ours compensates with
`/8` in the IDCT inner loop (D4.2). Net magnitude equivalent **only if** the
truncation point doesn't matter; arithmetic `>>3` of a negative int16 (Beetle)
vs `/8` of the same value at IDCT time (ours) **round differently for negative
taps** (>>3 floors, /8 truncates toward zero). Off-by-one on negative scale
entries.

---

## 3. Prioritized fix list (FMV-visible impact first)

1. **D4 IDCT rounding + intermediate 9-bit clamp** — adopt Beetle's exact
   two-pass `(sum+0x4000)>>15` with `Mask9ClampS8` between/after passes, and
   store the scale matrix pre-`>>3` (D11). This is the single largest source of
   global FMV brightness/contrast/ringing error. *(Highest visible impact.)*
2. **D5 YUV→RGB coefficients + green truncation mask + ±0x80 rounding** — switch
   to Beetle's /256 coeffs with `&~0x1F`/`&~0x07` green masking and `^0x80`
   sign handling. Fixes hue (esp. greens) across all colour FMV.
3. **D1 + D2 dequant fixed-point domain, sign bias, and clamp window** — move
   coefficients into the <<4 domain, add the `±8` sign bias, clamp ±0x4000.
   Must be done **together with #1** (the IDCT scale assumes this domain). Fixes
   block-level banding and premature saturation.
4. **D6 4bpp packing + 8bpp xor bias** — implement nibble-packed 4bpp with the
   `min(127, y+8)` bias and the unsigned-xor (0x88/0x80). Required for any MDEC
   texture-decompression title; FMV-only titles lower priority.
5. **D9 EOB/0xFE00 alignment** — only fully correct once decode is streamed
   (see #7); interim, match Beetle's "ignore EOB at CoeffIndex==0, zero-fill on
   mid-block EOB and finish the block."
6. **D7 status bits** — mirror command bits 25-28 into status 23-26 verbatim;
   advance the current-block field as `(DecodeWB+4)%6`; report live remaining
   count. (D3 folds in here.)
7. **D8 + streaming FIFO model** — the deepest fix: replace one-shot decode with
   a block-by-block state machine + 32-word In/Out FIFOs so DMA-request bits,
   busy window, and remaining-words count match hardware. Enables the FF7 /
   GameShark / SimCity edge cases and makes D7/D9 exact. Largest effort; do last
   unless a title visibly desyncs on DMA pacing.

---

## 4. Validation method (native :4500 vs Beetle :4382)

Goal: byte-exact decoded-block diff, not "looks right."

A. **Coefficient/block-level pin (deterministic, no playback needed).**
   - Feed an identical synthetic DECODE stream to both cores via a debug
     command: SET_QUANT (standard PSX matrix), SET_SCALE (standard scale
     table), then one macroblock of known RLE coefficients.
   - Dump (i) the post-dequant `Coeff[64]` for each of the 6 blocks, (ii) the
     post-IDCT `block_cr/cb/y` int8 arrays, (iii) the final packed output words.
   - Our side: add a debug-server dump of `crblk/cbblk/yblk` + output buffer
     (mdec.c already has a trace ring + `mdec_debug_get_state`; extend with a
     block-dump). Beetle side: dump `Coeff`, `block_*`, `PixelBuffer`.
   - Diff arrays element-wise. This isolates D1/D2 (Coeff), D4 (block_*), and
     D5/D6 (PixelBuffer) independently.

B. **Live FMV frame diff.** Run the same disc to the same FMV frame on both
   ports (port 4500 = our wt/tomba2 dev build per project memory; 4382 = Beetle
   here — confirm the live port, project docs also cite 4380). Use the existing
   `screenshot`/framebuffer dump on each, diff the decoded MDEC output region
   (or the GPU upload of it) pixel-by-pixel; histogram the per-channel delta.
   Expect R/B near-zero and **green showing the largest delta until D5 is
   fixed** — that signature confirms the green-truncation finding.

C. **Status-register pin.** Drive a DECODE and poll the MDEC status word on
   both cores at matched DMA progress points; diff bits 23-31 and the
   remaining-words field to validate D7/D3/D8.

D. **Regression gate.** After each fix, re-run A (block diff must reach
   byte-identical for the standard tables) before B. Keep A as a permanent
   pinning test so future emitter/codegen changes can't silently regress MDEC.
   Per CLAUDE.md Rule 15, build the block-dump tooling rather than eyeballing.

Note: confirm the live debug ports before run B — project memory cites
4500 (wt/tomba2 native), 4370 (master native), 4380/4382 (Beetle). Sections
1-3 above are static-analysis findings and stand independent of the port.
