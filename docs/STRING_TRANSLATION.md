# On-the-Fly String Translation / Localization for psxrecomp

**Status:** IMPLEMENTED and wired into the runtime. Reusable framework feature;
**Tsumu Light (SLPS-02253)** is the first consumer.

> This document was left marked "SPEC (design only — no implementation yet)"
> long after the feature shipped. It now reads as the design rationale behind
> working code, not a proposal. Where the prose below says "will" or "should",
> read it as describing what was built.

The implementation lives in `runtime/src/mods/text_xlate.cpp` (~970 lines) behind
`runtime/include/text_xlate.h`:

| Entry point | Role |
|---|---|
| `text_xlate_init(project_root, language)` | Loads `translations/*.toml` under the project root; called from `runtime/src/app/main.cpp` at startup |
| `text_xlate_set_language(language)` | Re-applies the resolved language (launcher/settings selection) |
| `text_xlate_on_dispatch(cpu, target)` | Per-dispatch capture/substitution hook |
| `text_xlate_vram_upload(x, y, w, h)` | VRAM-upload interception path |
| `text_xlate_debug_json(subcmd, out, cap)` | TCP debug-server introspection |

Configuration is `[localization].language` (falling back to `[runtime]`), with
`[localization].languages` driving the launcher's language dropdown; see
`recompiler/src/config_loader.h`. Setting the language to `jp`, `off`, or empty
disables substitution while leaving capture running.

**Goal.** Let a Japanese-only PSX title be played in English (or any target
language) by **capturing the game's source strings from memory and substituting
translated bytes on the fly**, so a non-Japanese speaker can navigate menus,
read tutorials/dialogue, and understand the game. **Ultimate success = every
string we can find renders in the target language.** Multilingual is a
first-class requirement (English is just the first target); the mechanism is a
**shared psxrecomp module** any title can adopt.

This design is a direct PSX adaptation of the author's proven N64 prior art in
the `n64recomp` fork for **Pocket Monsters Stadium (Japan)**
(`F:\Projects\n64recomp\PocketMonstersStadiumRecomp\`). Section 1 documents that
mechanism; Section 2 documents Tsumu's concrete text reality; Sections 3+ define
the PSX framework.

---

## 0. TL;DR architecture

Three always-on subsystems hung off the **`psx_dispatch` call chokepoint** (the
one place every guest function call funnels through — see §3.1):

1. **Capture ring (always-on).** On every dispatch, scan the argument registers
   (and a few stack slots) for pointers to Shift-JIS text records. Hash each
   distinct record (FNV-1a64 over its raw bytes), record `{hash, bytes, addr,
   target-PC, call-site}`, persist immediately. This *enumerates every string to
   translate* and *discovers which PCs draw text* — no arm-then-capture (per the
   project's ring-buffer rule).
2. **Translation table (per-title, human-editable, multilingual).**
   `TsumuRecomp/translations/tsumu.toml` — keyed by source-byte hash (+ optional
   `src_addr`), one target string per language, with encoding/length metadata.
   Hot-reloaded on mtime change.
3. **Apply hook (always-on).** A dispatch hook (sibling to the existing
   `g_psx_bios_hle_hook`) that, when the target is a registered text-draw PC and
   an arg points at a translated source record, writes the target-language bytes
   into transient guest scratch (below `$sp`), repoints the arg register, and
   lets the **original** draw routine render it. No regen, no engine change,
   unbounded replacement length, dynamic format strings preserved.

The only title-specific pieces are (a) the translation table and (b) the font
situation (§4). Everything else — capture ring, apply hook, config, coverage
tooling — is shared framework code.

---

## 1. Prior art: the n64recomp PMS-J mechanism

Files: `PocketMonstersStadiumRecomp/src/main/diagnostics.cpp` (the engine),
`include/trace.h` (the hook wiring), `tools/pms_build_translations.py` (authoring),
`translations.json` (the table). Verified by reading the source.

### 1.1 Capture — always-on string inventory + text-PC census

The recompiler emits a bare `TRACE_ENTRY()` at the top of **every** recompiled
function when `game.toml` has `trace_mode = true`. `trace.h` expands it to call,
with `rdram`/`ctx` in scope:

```c
pkmnstadium_textdraw_probe(__func__, ra, a0, a1, a2, a3);  // census + inventory
pkmnstadium_text_xlate(__func__, rdram, ctx);              // apply
```

`pkmnstadium_textdraw_probe` (diagnostics.cpp:1235) is the **always-on capture**
(explicitly *no* env gate — "we never 'arm' capture and hope to catch the
window, we record every unique on-screen string as it is first drawn and persist
it immediately"):

- **`read_text_arg()`** (1152) validates that an arg register points at a
  NUL-terminated EUC-JP/ASCII run ≥2 bytes: every byte must be newline,
  printable ASCII, or a EUC-JP lead/kana byte (`0x8E/0x8F/0xA1–0xFE`). The
  RDRAM-range check is the real discriminator — coords/ints/non-pointers are
  rejected before the byte scan.
- **Class-1 convention gate:** most text draws use `x=a0, y=a1, str=a2`; the
  `a0<0x1000 && a1<0x1000` "looks like screen coords" test rejects binary
  headers (`Yay0`/`PRESJPEG`) that decode as valid EUC-JP by chance.
- **`_Printf` chokepoint** (`kPrintfPC`): the universal vararg formatter is
  special-cased — its format string (a2) is inventoried regardless of the coord
  gate, so battle/overlay text built via `_Printf` is captured too.
- **`strinv_upsert()`** records every *distinct* string (content-hash keyed) into
  `stringdump.log`, flushed on every newly-seen string so a crash never loses
  captures.
- **`textpc_add()`** maintains a lock-free open-addressing set of PCs proven to
  draw text (`~70` distinct), so the apply hook only pays the arg-scan cost on
  real text PCs.

There is also a capture-only **discovery probe** (`xlate_discovery`, gated by
`PMS_TEXTPROBE`) that finds *which register/stack slot* a known-untranslated
watch-string arrives in — used to learn non-standard conventions (battle labels,
Pokédex descriptions in `r7`/`r4`, the `%sポケモン` category classifier in `r5`).

### 1.2 Translation table format

`translations.json` next to the exe, hot-reloaded on mtime (1550):

```json
{ "src_hex": "a5d0a5c8a5eb", "src_jp": "バトル", "target": "Battle", "orig_w": 3 }
```

- **`src_hex`** — the raw source bytes (EUC-JP) in hex. The load path converts
  hex→bytes and keys the in-memory map by **`FNV-1a64` of those bytes** (robust
  to RDRAM buffer reuse — the key is content, not address).
- **`target`** — the replacement (ASCII; may contain `%s`/`%d` specifiers to
  preserve dynamic args, and `\n` for line breaks).
- **`orig_w`** — Japanese footprint in glyph units (documentation + default fit
  budget). Optional **`fit_w`** overrides the width budget (use more/less space
  than the original when there is room). `src_jp` is documentation only.

Authoring workflow (`pms_build_translations.py`): `stringdump.log` → decode
EUC-JP → `strings_decoded.tsv` (persistent master across sessions) → author
`key_en.tsv` (`<fnv-key>\t<english>`) → `build` joins them into `translations.json`,
**merge-preserving** hand-authored entries. A `todo` mode lists still-untranslated
strings sorted by draw count.

### 1.3 Apply — on-the-fly substitution

`pkmnstadium_text_xlate` (2100) runs at every function entry; cheap no-ops
(`g_xlate_active` reentrancy guard, lock-free `g_xlate_armed` reject, reload
every 1024 calls). On a text PC it takes one of three paths:

- **Pointer-swap (`write_guest_str` + repoint `ctx->r6`).** Content-hash the
  source at the fmt arg; on a KV hit, write the replacement (ASCII, NUL-term)
  into **transient scratch on the calling thread's guest stack** (`sp-0x800` for
  format templates, `sp-0xC00` for literal args — two disjoint slots so a paired
  `_Printf` swap can't self-clobber) and repoint the arg register. The
  **original function then runs unchanged** — `_Printf` formats it (dynamic
  `%d`/`%s` stay intact) and renders glyph-by-glyph. No engine change, no regen,
  unbounded replacement length (never writes back into game RAM).
- **Proportional self-render (`self_render_static`, 1887).** For plain strings at
  coordinate drawers, it draws the English itself by calling the game's glyph
  routine (`FUN_8001a3e4`) per character with **per-glyph proportional advances**
  (calibrated by measuring real ink width of each glyph tile in VRAM,
  `calibrate_slot`/`measure_glyph`) and **auto-fit** condensing so English never
  spills past the Japanese footprint (`fit_w` budget or per-line JP width). Then
  it suppresses the caller's own draw with an empty fmt.
- **Register-specific paths** (`xlate_desc`, `xlate_general`) for the
  discovered non-`r6` conventions, including a subtle `_Printf` writeproc
  byte-count re-sync when a shorter replacement would otherwise embed a NUL and
  truncate a composed string.

### 1.4 Multilingual & font

- **Multilingual** is table-driven: the KV value is per-language. (PMS shipped
  English only, but the schema and hook are language-agnostic — selecting a
  language means loading a different column/file.)
- **Font/glyph:** PMS-J's font sheet **already contained Latin glyphs**, so
  English renders directly through the existing glyphs (the whole "no glyph
  injection needed" premise). `pkmnstadium_fontdump` (2227) dumps the font-state
  struct + glyph sheets (`.i8`) to answer exactly this gating question; the
  fallback contingency (documented in comments/tools) is to inject an ASCII
  glyph set if a game lacks Latin glyphs. The renderer measures glyph tiles
  directly from VRAM to get proportional widths.

### 1.5 What carries to PSX vs. what changes

| Aspect | N64 (PMS-J) | PSX (Tsumu) |
|---|---|---|
| Hook point | `TRACE_ENTRY()` emitted per function (needs `trace_mode=true` regen) | **`psx_dispatch` chokepoint** — one install site, no regen, works in Release |
| Encoding | EUC-JP (JIS X 0208 via `A1/A3/A4/A5` leads) | **Shift-JIS** (2-byte fullwidth + 1-byte halfwidth kana `A1–DF`) + custom control framing |
| String delimiter | NUL | **NUL (menu labels) AND `0xFFFF` (framed message records)** |
| Memory swizzle | big-endian `^3` byte swap | **little-endian, no swizzle** (simpler) |
| Arg access | `ctx->r4..r7` | `cpu->gpr[4..7]` |
| Glyph routine | `FUN_8001a3e4` (known) | text-draw PC pinned at runtime by the census (Phase 0) |
| Latin glyphs | present in font | fullwidth Latin present (proven); halfwidth ASCII TBD (§4) |

---

## 2. Tsumu's concrete text reality

Evidence gathered from the headerless boot EXE
(`TsumuRecomp/ghidra/SLPS_022.53_no_header.bin`, 512 KB, loads at `0x80010000`,
entry `0x800691F0`) via direct byte analysis + Ghidra, plus `DISC.md`.

### 2.1 Encoding — Shift-JIS (confirmed)

The text is **Shift-JIS**, not ASCII and not EUC-JP (EUC was the N64 game).
Proven by decoding real records:

| Address | Decoded | Notes |
|---|---|---|
| `0x80010C5C` | `ＴＳＵＭＵ　ｌｉｇｈｔ　プレイデータ` | fullwidth Latin + fullwidth space + katakana ("TSUMU light / Play Data") |
| `0x80010C94` | `ＴＳＵＭＵ　マップデータ` | "TSUMU / Map Data" |
| `0x80010190` | `？？？？？？？？` | fullwidth question marks (placeholder score row) |
| `0x80070AA8` | `それじゃ、がんばってね！`… | tutorial dialogue (hiragana) |

The character mix is:

- **2-byte fullwidth** (JIS X 0208): kanji, hiragana, katakana, **fullwidth
  Latin `0x8260–0x8279` (Ａ–Ｚ), `0x8281–0x829A` (ａ–ｚ), fullwidth digits
  `0x824F–0x8258`**, symbols. Lead byte `0x81–0x9F` / `0xE0–0xFC`, trail
  `0x40–0x7E` / `0x80–0xFC`.
- **1-byte halfwidth katakana** `0xA1–0xDF` (e.g. `0xBB`=ｻ) — mixed inline with
  2-byte chars. This means the font has a **halfwidth grid**.
- **Custom control framing** interleaved with text (raw bytes, not SJIS):
  - `0xFF 0xFF` — **message terminator** (record end).
  - `0x82 0xFE 0xFF` / `0x81 …` sequences — **line break / formatting** markers.
  - Menu labels (e.g. `0x80010C5C`) are simpler: plain 2-byte SJIS, **NUL**-terminated.

**Consequence for the framework:** the PSX text validator must be Shift-JIS
aware (2-byte lead+trail, 1-byte halfwidth kana `A1–DF`, ASCII `20–7E`), must
**pass through control bytes**, and must treat a "string" as a record delimited
by **either** NUL (menu labels) **or** `0xFFFF` (framed messages). The hash key
is the raw record bytes up to (not including) the terminator.

### 2.2 String storage

- **In-EXE Shift-JIS text block:** a large body of tutorial/hint/system messages
  lives in the EXE at roughly **`0x80070900`–`0x80072000`** (offsets
  `0x60900`–`0x62000`), reached through a **pointer/index table**.
- **Text pointer table** at **`0x80071474`** (preceded by header word
  `0x0000FC10` at `0x80071470`): a contiguous array of 32-bit little-endian
  pointers into the text records, e.g.
  `0x80070918, 0x80070958, 0x800709A4, 0x800709F0, … 0x80070AA8, 0x80070AC4, …`.
  This is the message-ID → record-address map; the text system indexes it by ID.
- **Menu/UI labels** live earlier in `.rodata` as NUL-terminated SJIS
  (`0x80010C5C`, `0x80010C94`, the `？？？？` row at `0x80010190`, memcard save
  titles `BISLPS-02253 TSMLGT` / `TSMMP` for "TSUMU light" / "TSUMU MaP").
- **Disc-streamed text/data:** `DISC.md` lists **`MOJI.BIN`** (文字 = "glyphs/
  characters" → the font sheet) and **`MONDAI.BIN`** (問題 = "problems/puzzles" →
  puzzle text/data), plus `GRAPH1-4.BIN`, `SOUND.BIN`, FMV (`DOUGA.BIN`/
  `END.STR`), `TSUMU.XA`. Disc image + files are present locally under
  `TsumuRecomp/tsumu/`. So some text is **static (patchable in the EXE)** and
  some is **loaded/composed from disc at runtime** (must be caught at draw time).

Static vs. dynamic: the in-EXE block and menu labels are static and could in
principle be patched at load, **but the framework deliberately intercepts at
draw time** (like PMS) so it uniformly covers disc-loaded and dynamically-
composed strings too, without touching game RAM.

**Recompiler note (not blocking):** the recompiler's flow walker currently
mis-decodes the text/pointer-table region as code (it emitted
`func_80070300` over `0x80070300…`, decoding the pointer words as load-byte
instructions — classic data-walk over-reach). This does not affect the runtime
translation hook (which works on live memory), but a data/code separation pass
would clean the generated C. Track separately.

### 2.3 Text render path

`psx_dispatch` funnels every call, so the exact draw routine is **pinned at
runtime by the capture census** (Phase 0), exactly as PMS discovered its
`~70` text PCs. Static anchors already found to seed/confirm it:

- The text-lookup routine loads base **`0x80071474`** and indexes it by message
  ID to fetch a record pointer; that pointer flows (as `a0`/`a1`/`a2`) into a
  glyph-draw routine that walks the SJIS bytes, maps each char to a glyph cell in
  the font texture (uploaded from `MOJI.BIN` into VRAM), and issues textured
  sprite primitives via the GPU. Menu labels reach the same drawer directly.
- Phase 0 confirms the exact draw PC(s), the arg register carrying the string
  pointer (PSX convention is commonly `a0`=str or `a0`=x,`a1`=y,`a2`=str), and
  the glyph-draw leaf (the PSX analogue of `FUN_8001a3e4`) if a self-render path
  is wanted for fit control.

(Full static disassembly of the drawer was not completed here because Ghidra
auto-analysis produced 0 functions on the raw dump and the region is
data-poisoned; the runtime census is the reliable, spec-sanctioned way to pin it
and is a Phase-0 deliverable.)

### 2.4 Font / glyph availability

- The game **renders fullwidth Latin and fullwidth digits today** (`ＴＳＵＭＵ
  ｌｉｇｈｔ`, score digits), so its font sheet **already contains a fullwidth Latin
  A–Z / a–z / 0–9 block** (SJIS `0x8260–0x8279`, `0x8281–0x829A`,
  `0x824F–0x8258`). **English can therefore render with zero glyph injection** by
  emitting fullwidth-Latin SJIS codes — the same "existing glyphs" premise that
  made PMS work.
- The game also uses **halfwidth katakana** (`0xA1–0xDF`), so a **halfwidth grid
  exists**. Whether a **halfwidth ASCII block (`0x20–0x7E`)** is also present —
  which would let English render *compactly* (half width) instead of fullwidth —
  is the **open question**, resolved in Phase 0 by dumping `MOJI.BIN` / the VRAM
  font page. PSX SJIS fonts frequently include halfwidth ASCII, so this is
  likely but must be confirmed.
- **Contingency** (if no compact Latin): inject an ASCII glyph set into the font
  page — append a Latin row to the `MOJI.BIN` glyph upload or upload a
  supplementary sheet into free VRAM and map ASCII codes to it (mirrors PMS's
  documented injection fallback).

### 2.5 Length / layout constraints

- **Menu labels** sit in fixed-width boxes; **framed messages** are laid out with
  the game's own `0xFFFF`/line-break control codes into a text window.
- English is usually longer than Japanese, and **fullwidth Latin doubles the
  width**, so overflow is the primary layout risk. Mitigations (§3.4): prefer
  halfwidth ASCII when available; re-insert the game's own line-break control
  codes to reflow multi-line messages; optionally a self-render fit path
  (PMS-style advance condensing) for coordinate drawers; and per-entry
  `max_w`/`fit` metadata in the table to cap width.

---

## 3. Framework design (PSX)

Shared module, shipped at `runtime/src/mods/text_xlate.cpp` +
`runtime/include/text_xlate.h` (peers of `fntrace.*` and `bios_hle.*`). The
design below was written as a proposal and is described in the future tense in
places; it is built, so read "will" as "does".

### 3.1 The hook point — `psx_dispatch`

`psx_dispatch` / `psx_dispatch_impl` (declared `cpu_state.h:164`) is the single
chokepoint every guest call routes through. Two precedents already hang off it:

- **`fntrace_record(cpu, target)`** (`fntrace.c:58`) — an **always-on ring** of
  every dispatch (`{frame, target, ra, a0..a3, s3, sp}`), explicitly designed as
  "record from boot, query the window after the fact" (the project ring-buffer
  rule). This is the capture analogue.
- **`g_psx_bios_hle_hook`** (`bios_hle.h:54`) — a hook slot consulted at the top
  of `psx_dispatch_impl` with the pre-normalize physical address; it can
  intercept/replace the target (HLE call + boot shell-skip). This is the apply
  analogue.

The translation module adds the same two shapes, so it needs **no per-function
codegen** and works in Release.

### 3.2 Capture subsystem (always-on ring)

Add `text_capture_record(cpu, target)` called from `psx_dispatch` right beside
`fntrace_record` (or fold into it):

1. For each candidate arg (`gpr[4..7]`, plus a few stack slots `sp+0x10..`),
   run **`sjis_read_record(addr, out, &len, &term)`**:
   - Reject if `addr` outside main RAM (`0x80000000–0x80200000` and mirrors).
   - Walk bytes until NUL **or** `0xFFFF` terminator (cap `kSrcMax=512`), classi-
     fying each: ASCII `0x20–0x7E`, halfwidth kana `0xA1–0xDF`, SJIS 2-byte
     lead+trail, or known control byte. Reject on any non-textish byte (filters
     structs/pointers). Require ≥1 real text char.
2. Hash the record bytes: **`h = fnv1a64(bytes, len)`**.
3. Upsert into an **always-on inventory ring** `{h, bytes[≤64 sample], full-len,
   src_addr, target_pc, ra, first_frame, count}`; on first sight, append to a
   persistent `tsumu_stringdump.log` (flush immediately — crash-safe, per the
   ring-buffer rule).
4. `textpc_add(target)` into a lock-free set so the apply hook only scans args on
   proven text PCs.

This runs from boot for every player (no arm gate). "Enumerate every string" =
drive the game (§6) and read the ever-growing inventory; "find untranslated" =
query the ring for records with no table entry.

**Coord/`_Printf` disambiguation:** reuse PMS's precision filters adapted to
PSX — a coordinate gate (`a0,a1 < 0x1000`) for the class-1 drawer, plus explicit
handling for any universal formatter PC once the census reveals it.

### 3.3 Translation table (multilingual, human-editable)

**Location:** `TsumuRecomp/translations/tsumu.toml` (single multilingual file;
TOML fits the project's config conventions — `game_options.toml`,
`SCPH1001.toml`). Loaded by the shared module from a path the runtime injects
(peer of the overlay-cache dir injection). Hot-reloaded on mtime change.

**Schema:**

```toml
# tsumu.toml — Tsumu Light string translations (multilingual)
schema = 1
default_lang = "en"
langs = ["en", "es", "de", "fr"]   # first-class multilingual; add columns freely

[[entry]]
src_hex   = "82bb82ea82b6..."   # raw Shift-JIS record bytes (hex), the hash key
src_jp    = "それじゃ、がんばってね！"  # documentation (decoded), not used for matching
src_addr  = 0x80070AA8          # optional disambiguator when two records collide on hash
term      = "ffff"              # terminator that delimited this record: "nul" | "ffff"
orig_w    = 12                  # JP footprint in glyph cells (fit budget / docs)
max_w     = 12                  # optional hard width cap (glyph cells) for overflow control
note      = "tutorial: end-of-lesson encouragement"
en = "Well then, do your best!"
es = "¡Pues venga, esfuérzate!"
de = "Also dann, gib dein Bestes!"

[[entry]]
src_hex = "8273827282758..."    # ＴＳＵＭＵ　ｌｉｇｈｔ　プレイデータ
term    = "nul"
en = "TSUMU light  Play Data"
```

- **Key = `fnv1a64(src bytes)`** (content hash — robust to buffer reuse), with
  `src_addr` as a tiebreak if two distinct records ever collide.
- Per-language target strings; **untranslated / missing-language falls back to
  the source** (render the original Japanese) so the game is never broken by a
  gap.
- Target strings are authored in **UTF-8**; the apply layer transcodes UTF-8 →
  game glyph codes (§3.4). `\n` requests a line break (emitted as the game's own
  line-break control code). `%d`/`%s`-style specifiers are preserved for dynamic
  args if the census shows a formatter path.
- `orig_w`/`max_w` drive width fitting.

**Authoring tools** (shared, `psxrecomp/tools/`, adapted from PMS):
`text_xlate_decode.py` (stringdump → decoded TSV master, Shift-JIS via Python
`cp932`), `text_xlate_build.py` (join `<key>\tUTF-8` → `tsumu.toml`,
merge-preserving), `text_xlate_todo.py` (list untranslated by draw count →
coverage report).

### 3.4 Apply subsystem (dispatch hook)

Install `g_psx_text_xlate_hook` (sibling to `g_psx_bios_hle_hook`), consulted at
the top of `psx_dispatch_impl` **after** the BIOS-HLE hook, gated by a lock-free
`armed` flag (table non-empty) and the `textpc_contains(target)` set:

1. Read the source record at the string-arg register (`sjis_read_record`).
2. `h = fnv1a64(...)`; look up `{lang}` target. Miss → **return without change**
   (fallback to source).
3. **Transcode** the UTF-8 target → a byte sequence the game's drawer will
   render:
   - **Halfwidth-ASCII path** (preferred, if Phase 0 confirms halfwidth Latin
     glyphs): emit ASCII `0x20–0x7E` directly — compact, closest to JP width.
   - **Fullwidth-Latin path** (guaranteed): map each ASCII char to its fullwidth
     SJIS code (`A→0x8260…`, `a→0x8281…`, `0→0x824F…`, space→`0x8140`). Always
     works because those glyphs are proven present. Double width → rely on width
     fitting.
   - **Injected-glyph path** (contingency): map to injected ASCII glyph cells.
   - Re-emit the record's framing: preserve/insert the game's `0xFFFF` terminator
     and line-break control codes; convert `\n` to the game's line-break code.
4. **Write to guest scratch** below the caller's `$sp` (`gpr[29]`) using the
   runtime store accessors (little-endian, no swizzle): pick a scratch slot with
   stack headroom (e.g. `sp - 0x800`, `& ~7`), bounds-checked against RAM; NUL/
   `0xFFFF`-terminate.
5. **Repoint the string-arg register** at the scratch address. Return "not
   handled" so the **original draw routine runs and renders the replacement**
   (the whole no-regen premise). A reentrancy `thread_local` guard prevents the
   hook re-triggering on any nested draw calls.

**Length / layout handling:**
- Prefer halfwidth ASCII (compact). Use table `max_w` to cap width.
- For framed multi-line messages, reflow by inserting the game's own line-break
  control codes at word boundaries to fit the text window.
- **Optional self-render fit path** (Phase 1+, mirrors PMS `self_render_static`):
  once the glyph-draw leaf PC is known, draw the target ourselves per-glyph with
  condensed advances and suppress the caller's draw — gives pixel-accurate
  fit-to-JP-footprint. Not required for first light; the pointer-swap path is.

### 3.5 Language selection

- **`game.toml` / `settings.toml`:** add `[localization] language = "en"` (peer
  of the existing renderer/aspect/FMV/turbo keys; `settings.toml` overrides
  `game.toml`, CLI overrides both — per the project's settings precedence).
  `"jp"`/`"off"` = passthrough (no translation).
- **Launcher:** a "Language" dropdown populated from the table's `langs`
  list; writes the choice to `game_options.toml`. Runtime switch = reselect →
  the module reloads the active language column (no restart needed; hot-reload
  already covers table edits).
- Default = `default_lang` from the table (English for Tsumu).

### 3.6 Coverage workflow

The loop to reach "all strings target-language" (§0 goal):

1. **Capture pass.** Fresh run; drive every screen/menu/tutorial/dialogue/ending
   (self-driven via the debug port + input injection, or a scripted playthrough).
   The always-on ring logs every distinct record to `tsumu_stringdump.log`.
2. **Decode + author.** `text_xlate_decode.py` → master TSV (Japanese visible).
   Fill translations; `text_xlate_build.py` → `tsumu.toml`.
3. **Apply + verify.** Re-run with the table; the hook substitutes. **Verify
   visually** (screenshot both windows — never infer from a counter), per the
   project's verification rules.
4. **Re-capture to find gaps.** `text_xlate_todo.py` diffs the live inventory
   against the table → coverage report (`N/total translated`, untranslated
   sorted by draw count). Repeat until the untranslated set is empty across a
   full playthrough.
5. **Coverage report** artifact: total distinct records seen, translated %,
   per-screen breakdown, list of dynamic/composed strings needing special
   handling. "We've found them all" = a full playthrough adds zero new records to
   the inventory.

### 3.7 Framework generality (what's shared vs. title-specific)

| Shared (framework) | Title-specific |
|---|---|
| Capture ring + `sjis_read_record` + inventory persistence | The translation table (`<title>.toml`) |
| `fnv1a64` keying, text-PC census set | The font situation (Latin present? inject?) |
| Apply dispatch hook + guest-scratch write + arg repoint | Encoding profile (SJIS here; another game might be custom-tiled) |
| UTF-8 → glyph transcoders (ASCII, fullwidth-Latin, injected) | The string-arg register convention (pinned by census) |
| Config keys + launcher wiring + hot-reload | Control-framing profile (terminators, line-break codes) |
| Authoring tools (decode/build/todo/coverage) | — |

Encoding is abstracted behind an **`EncodingProfile`** (validator + terminator
rules + transcoders) selected per title in `game.toml`
(`[localization] encoding = "shift_jis"`), so a game using a custom
tile-index encoding plugs in its own profile without touching the capture/apply
core.

---

## 4. Font/glyph plan — concrete Tsumu situation

1. **Guaranteed today:** fullwidth Latin + fullwidth digits are in the font
   (proven by `ＴＳＵＭＵ ｌｉｇｈｔ`). Ship English on the **fullwidth-Latin path**
   for first light — zero asset work, guaranteed to render.
2. **Phase-0 check:** dump `MOJI.BIN` / the live VRAM font page and inspect for a
   **halfwidth ASCII block** (`0x20–0x7E`). The presence of halfwidth katakana
   (`0xA1–0xDF`) means a halfwidth grid exists; if ASCII is in it, switch to the
   compact **halfwidth-ASCII path** (better width fit).
3. **If neither compact option:** inject an ASCII glyph set — extend the
   `MOJI.BIN` upload or blit a supplementary Latin sheet into free VRAM and map
   ASCII to those cells. Add a small `font_probe`/`font_dump` tool (PSX analogue
   of `pkmnstadium_fontdump`) that reports the font page layout and which code
   ranges have ink.

---

## 5. Risks / open questions

1. **Halfwidth ASCII glyphs present?** Determines compact vs. fullwidth (width
   budget). Phase-0 blocker for layout quality (not for first light).
2. **Exact text-draw PC(s) + string-arg register.** Pinned by the Phase-0
   census; menu labels vs. framed messages may use different drawers/registers
   (PMS had `~70` PCs and multiple conventions — expect the same spread).
3. **Control-code framing semantics.** `0xFFFF` (terminator) and the `0x81…`/
   `0x82 0xFE 0xFF` sequences (line break/format) must be decoded so we preserve
   them across substitution and can reflow. Reverse the exact code set in
   Phase 0.
4. **Dynamically composed strings** (scores, counts, names concatenated at
   runtime, e.g. the `？？？？` placeholder row): the source bytes vary, so a
   pure content-hash misses them. Handle via formatter-template entries
   (`"%d"`-style) once the census reveals a composing routine, or per-fragment
   translation — same class of problem PMS solved for `%sポケモン`.
5. **Length overflow / fixed boxes.** English (esp. fullwidth) overruns menu
   boxes; needs `max_w` capping, reflow via line-break codes, and possibly the
   self-render fit path.
6. **Disc-loaded text (`MONDAI.BIN`/`MOJI.BIN`).** Confirm puzzle/problem text is
   caught at draw time (it should be — the capture is draw-time, not load-time);
   if a puzzle *is* Japanese-character gameplay, translation may be
   gameplay-affecting and should be scoped carefully (menus/UI/tutorials first).
7. **DTE / dictionary compression.** Not observed (text is plain SJIS in the
   EXE), but confirm no compressed text table exists on disc before declaring
   full coverage.
8. **Hot path cost.** The apply hook runs at every dispatch; keep the fast reject
   (lock-free armed flag + text-PC set membership) ahead of any RAM scan, exactly
   as PMS does, so non-text dispatch pays ~nothing.

---

## 6. Phased implementation plan

- **Phase 0 — Discovery (runtime census).** Add the capture ring +
  `sjis_read_record` + inventory persistence + text-PC census. Drive Tsumu; dump
  `tsumu_stringdump.log`. Pin the text-draw PC(s), the string-arg register, the
  control-code set, and the font situation (`MOJI.BIN` dump). Deliver the encoding
  profile. *(No visible change yet — pure observability.)*
- **Phase 1 — Apply (pointer-swap).** Install `g_psx_text_xlate_hook`; implement
  the fullwidth-Latin transcoder + guest-scratch write + arg repoint. Author a
  starter `tsumu.toml` (main menu). Verify menus render English visually.
- **Phase 2 — Encoding/layout polish.** Halfwidth-ASCII path if available;
  reflow via line-break codes; `max_w` fitting; framed-message coverage.
- **Phase 3 — Multilingual + launcher.** Language dropdown, `game.toml`/
  `settings.toml` key, runtime switch, second language column to prove the
  pipeline.
- **Phase 4 — Coverage to 100%.** Iterate capture→author→verify with the
  coverage report until a full playthrough surfaces no untranslated records.
  Handle dynamic/composed strings and any injected-glyph work.
- **Phase 5 — Generalize.** Lift the module to a clean framework API
  (`EncodingProfile`, config-driven table path), document a "new title" recipe,
  and (optionally) fix the data/code separation for the mis-decoded text region.

---

## Appendix Z — Implementation status & Phase-0 empirical findings (2026-07-05)

The mechanism is IMPLEMENTED and verified working headless on Tsumu; the
translation table is an ongoing coverage effort. Hard-won findings from driving
the real game (these correct/extend §2, which was written pre-implementation):

- **Hook point (as specced):** `text_xlate_on_dispatch()` is called from
  `fntrace_record()` at the `psx_dispatch` chokepoint. No generated-code edits,
  no BIOS regen, works in the Release build. Capture + apply confirmed live.
- **Two encodings, two endiannesses.** The EXE menu labels (e.g. `0x80010C5C`
  `ＴＳＵＭＵ ｌｉｇｈｔ`) are **big-endian** Shift-JIS, NUL-terminated. The
  message-table text (tutorial/hint/dialogue, drawn via a string pointer) is
  **little-endian 16-bit** Shift-JIS (bytes `bf 82` = ち), framed with control
  codes and **0xFFFF**-terminated. The transcoder must emit the matching
  endianness (`PSX_XLATE_LE`); this should become an `EncodingProfile` flag.
- **Control framing (confirmed):** `FE FF` = line break; `FF FF` = end of
  message; `30 FC` / `10 FC` = page/prompt breaks; `＄`/`％` (`8x` codes) =
  on-screen button icons; fullwidth space `81 40` pads fixed-width fields.
- **Two draw paths — only one is interceptable.** Message/dialogue text is drawn
  by passing a **string pointer** to a formatter → captured & translatable.
  The **title prompt, HUD ("ステージ"/"バッテリー"), and menu labels are drawn
  per-glyph as sprites** (glyph index, no string pointer) → **NOT reachable by
  arg-scanning**. Full HUD/menu localization needs a second hook that intercepts
  the per-glyph sprite draw and remaps the glyph index — a larger, separate
  effort (open item).
- **Struct-embedded strings crash if naively replaced.** Some NUL records are
  fixed-width struct fields with binary params packed after the text (e.g. the
  `ちゅーとりあるの5` "Tutorial N" label carries trailing coordinate bytes).
  Replacing the whole record corrupts the struct and derails the game (observed
  `PC=0`). **Apply is therefore gated to standalone 0xFFFF-framed messages by
  default** (`PSX_XLATE_ALLOW_NUL=1` to override for vetted labels). This is the
  concrete form of the §5.4 "dynamically-composed / struct" risk.
- **Capture quality gate.** The relaxed reader admits vertex/coordinate binary
  that passes by chance (~20k records). A gate requiring ≥2 hiragana/katakana/
  fullwidth chars (SJIS lead `0x82`/`0x83`) keeps the always-on inventory a clean
  enumeration (~52 real records). Authoring reads it via TCP `xlate dump/todo`.
- **Verification.** Framed tutorial hints apply on real draws (hits climb, game
  stays alive, English written little-endian). These particular hints are
  composed during a load transition and displayed only in **interactive tutorial
  mode**; a full on-screen visual pass needs menu navigation to that mode (and,
  for the HUD/menu, the per-glyph hook above). Debug-server `screenshot` is
  flaky after extended headless runs — a tooling bug to fix (CLAUDE.md rule 15).

**Coverage status (138 entries shipped):** the full in-EXE message table was
statically enumerated — pointer table at **0x80071474** plus the block
**0x80070900–0x80073700** — decoded (little-endian Shift-JIS, control framing
honored) and translated to English: mode-select descriptions, the entire
interactive tutorial, ~90 per-stage hints, all memory-card save/load/format
messages, the Yes/No confirmation dialogs, and the contributor credits. Every
entry's `src_hex` is byte-exact to what the game draws — cross-checked by
hashing against the always-on runtime capture ring (confirmed matches), so the
apply hook fires on the real draws (verified: hits climb, no crash, English
written little-endian).

**Reader limitation (small gap):** the byte-based reader rejects a record whose
first byte isn't "textish" — which for LE storage means a first glyph whose low
byte is 0xA0 (i.e. records starting with **あ**, e.g. the name-entry prompt
"あなたの名前を教えてね"). Those few stay Japanese until the reader is made
LE-word-aware (would rehash the table).

**Still out of scope / open:** (1) per-glyph sprite-draw text — HUD
(ステージ/バッテリー), title prompt, menu labels, the "CLEAR!!" banner, the
name-entry glyph grid — needs a separate glyph-index-remap hook (deferred
feature, by design); (2) `MONDAI.BIN` disc puzzle data (if it carries
string-pointer text, the always-on capture will surface it during play);
(3) fold endianness + the LE-first-byte fix into `EncodingProfile`;
(4) `screenshot` writes fail to some temp paths — use a project-dir path.

## Appendix Y — Per-glyph text: investigation & why it resists (2026-07-05)

The HUD / title / menu labels are drawn **per-glyph as sprites** (glyph index →
font tile → sprite), NOT via the string-pointer formatter, so the message-path
hook cannot reach them. Investigation results (concrete, for the eventual hook):

**Font glyph-index map** (table at EXE `0x80074ACA`, one LE-SJIS code per index):
- **Latin `Ａ–Ｚ` = indices 73–98**, **digits `０–９` = 105–114** (primary bank);
  a second Latin/digit bank at **213–252**. Hiragana fills 0–64 and 121–207.
  So for any English char the glyph index is known. **The font already contains
  fullwidth Latin/digit glyphs** (confirmed) — no glyph injection needed.
- Note: this table has **no katakana** (it is the name-entry character set);
  the katakana glyphs the HUD uses live at other indices in the VRAM font
  (`MOJI.BIN`) — dumping `MOJI.BIN` / the VRAM font page is still needed to map
  katakana → index if a katakana label is to be recognized.

**Label storage** — per-glyph labels are static **little-endian Shift-JIS** in
the EXE, in **fixed-width, space-padded slots** (patchable):
- Level names ~`0x80070755–0x800708E4`: `はこにわ`(0x8007075E) `たわー`(0x800707AC)
  `くらげ`(0x800707C6) `せんしゃ`(0x800707E0) `せぱれーと`(0x800707FA)
  `おてほん`(0x80070814) `ろじうら`(0x800708E4) … each padded with fullwidth
  spaces (`81 40`) to a fixed slot.
- Tutorial labels ~`0x80070300+` (`ちゅーとりある` ×6); mode name `のーまるげーむ`;
  `はい`/`いいえ` at several sites.

**Cleanest hook (designed, not shipped):** since fullwidth Latin glyphs exist,
a **load-time source patch** — after game-start, overwrite a label's katakana/
hiragana SJIS in RAM with fullwidth-Latin English SJIS (`ステージ`→`ＳＴＡＧＥ`),
padded to the slot with `81 40` — makes the **existing** per-glyph routine draw
English, with no new draw routine and no glyph injection. English must fit the
slot (the space padding gives the budget; e.g. `たわー`→`ＴＯＷＥＲ`,
`くらげ`→`ＪＥＬＬＹＦＩＳＨ`). For labels drawn instead via the string-pointer
path, they simply go in `tsumu.toml` like any message entry.

**Why it is not shipped/verified here:** the bar is a screenshot of English on
these screens, and **the screens are unreachable in the headless build**: the
attract never auto-plays the gameplay demo (150 s observed: title/Sony-logo
only), and reaching the mode/level-select or in-game HUD requires completing the
**name-entry glyph grid** (enter a char, navigate to the "End" cell, confirm) —
which is intractable via blind debug-input injection (input reaches the pad,
verified, but precise grid navigation to "End" did not land). Committing a
RAM-patch feature that overwrites game memory without a rendered-English
screenshot would be unverified coverage, so it is documented here instead.

**Remaining resisters within per-glyph:** the HUD counter `ステージ：X–Y` (label
storage not found statically — composed at runtime / in an overlay; needs
gameplay capture to pin); the title prompt and `CLEAR!!` (likely baked sprite
graphics, not glyph-drawn → would need image replacement, out of the glyph
model). Next step for whoever has a reachable build: dump `MOJI.BIN`, confirm the
katakana index range, ship the load-time source patch for the padded label
slots, and screenshot the level-select / HUD.

## Appendix A — Key source references

**n64recomp prior art** (`F:\Projects\n64recomp\PocketMonstersStadiumRecomp\`):
- `src/main/diagnostics.cpp` — capture (`pkmnstadium_textdraw_probe` 1235,
  `read_text_arg` 1152, `strinv_upsert`/`textpc_add`), apply
  (`pkmnstadium_text_xlate` 2100, `self_render_static` 1887, `xlate_desc`/
  `xlate_general`), `load_translations` 1550, `pkmnstadium_fontdump` 2227.
- `include/trace.h` — `TRACE_ENTRY()` hook wiring.
- `tools/pms_build_translations.py` — decode/build/todo authoring.
- `translations.json` — table format.

**psxrecomp hook points** (paths relative to this repository; they were captured
from the since-removed `_wt-tsumu` worktree, but the files live on `master`):
- `runtime/src/mods/text_xlate.cpp` + `runtime/include/text_xlate.h` — the shipped
  module: `text_xlate_init` / `_set_language` / `_on_dispatch` /
  `_vram_upload` / `_debug_json`. Loads every `translations/*.toml`.
- `runtime/include/fntrace.h` + `runtime/src/debug/fntrace.c:58` — always-on dispatch
  ring (capture analogue).
- `runtime/include/bios_hle.h:54` (`g_psx_bios_hle_hook`) + `runtime/src/bios/bios_hle.c:297`
  — dispatch hook slot precedent (apply analogue).
- `runtime/include/cpu_state.h:164` — `psx_dispatch` / `psx_dispatch_call`.

**Tsumu evidence** (`F:\Projects\psxrecomp\TsumuRecomp\`):
- `ghidra/SLPS_022.53_no_header.bin` — boot EXE (load `0x80010000`, entry
  `0x800691F0`).
- Menu labels `0x80010C5C`, `0x80010C94`, `0x80010190`; tutorial text block
  `~0x80070900–0x80072000`; **text pointer table `0x80071474`** (header
  `0x0000FC10` @ `0x80071470`).
- `DISC.md` — `MOJI.BIN` (font), `MONDAI.BIN` (puzzle text), disc file list.
- `game.toml` — `load_address`, `entry_pc`, `debug_port = 4510`.

## Appendix X — Per-glyph coverage pass: shipped, and the composed-HUD resisters (2026-07-05)

Follow-up to Appendix Y. Concrete outcomes of the per-glyph work, with the
static evidence that decides what is source-patchable vs. what genuinely resists.

### Shipped (RAM source-patch layer — `[[glyph_label]]`)
- **Stage/level/tutorial names** — the full uniform 26-byte-stride table at
  **`0x80070300`–`0x800708FE`** (59 slots, one per puzzle). Confirmed clean:
  every slot start is +26 from the last (`text` + fullwidth-space pad), so slot
  bounds are certain. Patched in RAM once resident, verified byte-for-byte
  against the source before writing exactly 26 bytes. (commit `f00631e` runtime,
  `31ab3c2` table.)
- **Name-entry prompt** `あなたの名前を教えてね。` @ **`0x80071360`** — a static
  0xFFFF-framed record whose renderer reads the source address *directly* (a
  pointer-repoint `[[entry]]` captures it with `xl=true` and `hits` increments,
  but the pixels don't change). Source-patched instead: `[[glyph_label]]` width
  24 (12 cells) → fullwidth "YOUR NAME?", trailing `FF FF` @ `0x80071378` left
  intact. Renders English on-screen. (commit `31ba92d`.)
- **Name-entry Latin input is NATIVE.** The kana grid already draws the font's
  fullwidth Latin `Ａ–Ｚ` and digits `０–９` (char table `0x80074ACA`, indices
  73–98 / 213–252). A non-JP player can type an English name today — no change
  needed; the only JP on that screen was the prompt above.

### Key discriminator: static source address ⇒ source-patchable
The RAM source-patch works for **any** label the game draws from a fixed EXE
address, *regardless of draw path* (per-glyph sprite loop OR string formatter) —
both read the same bytes. It CANNOT touch text that is **composed at runtime**
(no static source to overwrite).

### `CLEAR!!` banner — ALREADY ENGLISH (no work needed)
Reaching a gameplay stage (attract demo, HUD on-screen) and screenshotting the
stage-clear moment shows the banner **already renders "CLEAR!!" in stylized
Latin letters** — it is not Japanese. Consistent with the static evidence: `クリア`
(`4e 83 8a 83 41 83`) appears only message-embedded ("…をクリアしていく"…), there
is no standalone `クリア` / fullwidth `ＣＬＥＡＲ` / ASCII `CLEAR` in the EXE, and
the disc's only pre-rendered text graphic is the `TSUMU light` logo. The clear
banner is drawn from Latin glyphs the game already owns. **No translation
required** (the earlier "resister" assumption was wrong — corrected here).

### In-game HUD — RESOLVED 2026-07-06, see Appendix Z (the analysis below was superseded)
The characterization below assumed the HUD labels were composed per-glyph at
stage load. They are not: the labels exist as **pre-rendered pixel strips
inside the glyph-sheet asset itself** (the 64hw-wide block uploaded to VRAM
832,256 — MOJI.BIN carries the font AND the baked label strips). The stage-load
compose just MoveImages the whole strip (`src=(834,352) 18×16` ステージすう：,
`src=(852,352) 18×16` のこりバッテリー：) into the HUD cache row at y=32. The
shipped fix is the `[[vram_patch]]` upload-time pixel patch (Appendix Z) — no
glyph remap hook was needed. Historical analysis kept for the record:

### In-game HUD `ステージ：X–Y` / `バッテリー：N` — RESISTER (characterized, NOT shipped)
The HUD label is the one genuinely-unshipped item. Rendering model, pinned via
the always-on GP0 ring (`gpu_frame_dump`, records every primitive with its
submitting PC + DMA `src_addr`):
- The katakana `ステージ` (`58 83 65 83 5b 81 57 83`) / `バッテリー`
  (`6f 83 62 83 65 83 8a 83 5b 81`) exist in a full 2 MB live RAM scan **only** at
  the static message-text addresses (`0x800709xx`…), each followed by a particle
  (を/が/の/は) — never as a standalone HUD label, and there is no `ステージ：`
  template. So there is **no static source the RAM source-patch could rewrite.**
- The game composites heavily: it averages **~6 draw primitives/frame**
  (`gp0_draw` ÷ frames) and rebuilds only the animated foreground each frame
  (character + a few blocks, ~8 textured `0x2C` quads at screen y ≥ 31, issued
  via the BIOS DMA library from an OT at RAM `~0x138xxx`). The HUD sits at screen
  y ≈ 8 and is **NOT among the per-frame draws** — it is composed **once at
  stage-load** into the display/background and thereafter just shown.
- The stage-load compose is therefore the only place to intercept it, and it
  happens before the GP0 ring's steady-state window; catching it needs a **fresh
  stage load while observing** (force real gameplay past name-entry, or clear a
  stage so the next one composes), then decoding whether the label is blitted as
  CPU→VRAM glyph bitmaps (`0xA0`) or drawn as font-tile quads, and remapping that
  specific compose — a scoped per-glyph/blit intercept, not a source-patch.

This remains the **per-glyph interception** effort flagged in Appendix Y. It was
NOT shipped: a clean, pixel-verified remap of the composited HUD wasn't achieved
in this pass, and a blanket VRAM font swap (the only "quick" alternative) would
corrupt katakana in menus/messages and can't handle the `ステージ`→`STAGE`
4-vs-5-glyph layout change. Per project Rule 5 (pixels or it's not done) and the
no-fragile-hacks rule, no partial was shipped. Next step for a reachable build:
force a fresh stage-load, dump the GP0 ring across the compose, and remap there.

### Disc asset map (all files extracted from `Tsumu Light (Japan).bin`)
ISO9660 root: `DOUGA.BIN` (FMV), `END.STR` (ending FMV), `FB_TEX.BIN` (4bpp block
textures @VRAM 768,0), `GRAPH1/4.BIN` (16bpp `TSUMU light` title @320,0),
`GRAPH2/3.BIN` (8bpp character sprite sheets @320,256 / 576,256), `HECTLOGO.TIM`
(4bpp Hect logo), `TSU_KOK.TIM` (16bpp), **`MOJI.BIN`** (22528 B raw font, lba
7119 — the HUD/per-glyph font sheet), `MONDAI.BIN` (puzzle data), `SOUND.BIN`,
`TSUMU.XA`. Rendered PNGs confirm no CLEAR!! banner among the baked graphics.

### HUD remap — the drawer is now pinned (mechanism cracked, for the next pass)
Live GP0-ring capture on the name-entry screen (which draws per-glyph text at the
screen top, same class as the HUD) pinned the per-glyph text drawer concretely:
- **Drawer PC `0x8005443C`** — issues one textured quad per glyph via **direct
  GP0** (not DMA), building the primitives in game RAM at **`~0x80092xxx`**. Each
  glyph is a `0x2C` textured quad: screen rect (X,Y, 16–32 px cell) + **U,V into
  texpage `0x1E`** selecting the font tile, CLUT `0x7C10`.
- **The font page (texpage 0x1E) holds hiragana AND Latin** — proven because the
  name-entry grid draws `Ａ–Ｚ`/`０–９` through this exact drawer. So the Latin
  tile U,Vs the HUD remap needs are directly readable off the grid's letter quads
  (correlate each grid cell's on-screen glyph ↔ its quad U,V).
- **Remaining blocker for a clean HUD remap:** catch the HUD label's own compose
  (it draws its katakana quads via this drawer at stage-load) to get its source
  prims + katakana U,Vs, then either rewrite those prims' U,V to the Latin tiles
  or hook `0x8005443C` to remap the HUD-position glyph quads. Reaching a fresh
  stage-load is gated on confirming a name in the grid (blind debug-input grid
  navigation to the "decide" cell did not land) or a memcard LOAD path — a
  reachable interactive build (the USER's) clears this immediately. With the
  drawer + font-tile scheme now known, the remap itself is a scoped U,V rewrite.

### HUD remap — reachability blockers (why it's not finishable headless)
Attempted the LOAD-GAME unblock (skip the name grid) to reach a dynamic HUD:
- **Both memcards are empty.** `saves/card1.mcd` + `card2.mcd` are formatted
  (magic `MC`) but have **0 used blocks** and no `SLPS`/`TSM` save markers — so
  **LOAD GAME has nothing to load** and cannot reach a stage.
- **NEW GAME** still routes through the name-entry glyph grid; blind debug-input
  navigation to its "decide" cell did not land (confirmed again this pass).
- **The attract demo's HUD is not per-frame-drawn.** With the GP0 ring recording
  from the game's first GP0 write (frame 554), a full 554→3754 capture shows the
  HUD (screen y≈8) is **never** drawn as glyph quads or any top-region primitive;
  steady gameplay frames contain only the animated 3D scene (~8 textured `0x2C`
  quads at screen y≥31, via BIOS DMA `0xBFC38B1C`). The HUD is composed once at
  stage-load and thereafter only displayed. In the demo its values are static
  (`0-5` / `∞`), so there is **no dynamic HUD draw to intercept** and no
  discriminator to capture.

**Net:** the drawer + font-tile scheme are cracked (see above), but capturing the
HUD label's own glyph quads (kana U,Vs + a call-context/screen-region
discriminator) requires **real gameplay where the HUD is dynamically drawn**
(battery counting down / stage advancing) — reachable only via a real save
(create one first) or driving NEW GAME past the name grid on an interactive
build. No hook was shipped: without the captured discriminator, any remap would
risk rewriting non-HUD quads (blocks/menus) — a fragile hack (Rule 5, forbidden).
Precise remaining step for the USER's reachable build: enter a stage so the HUD
draws, dump the GP0 ring at drawer `0x8005443C`, read the HUD glyph quads' U,Vs +
screen row, then rewrite those to the font's Latin tiles for STAGE / BATTERY.

## Appendix Z — VRAM-strip patch layer (`[[vram_patch]]`) — HUD labels SHIPPED (2026-07-06)

### The missing model: pre-rendered strips, not composed glyphs
The HUD labels (and, it turns out, the entire menu system) are **pre-rendered
pixel strips baked into the glyph-sheet asset** — not glyph-code strings, not
per-glyph composes. Evidence chain:
- The 144-entry font order table at `0x80074a9c` contains **no katakana** (only
  hiragana + fullwidth Latin/digits + a little punctuation), yet the HUD shows
  katakana `ステージすう：`/`のこりバッテリー：` — so that text cannot come from
  the glyph-code path at all.
- No SJIS code string for either label exists anywhere in the EXE or in a full
  2 MB RAM scan (searched as both hiragana and katakana, both byte orders).
- Rendering VRAM `(832,256) 64×256` as 4bpp shows the sheet rows 0–4 are the
  font, and rows past the font are **baked strips**: the HUD labels at y=352,
  `○ボタンで すすむ !!` at y=368, results-screen labels (`ステージすう`/
  `のこりバッテリー`/`ランク`/`ほじゅうバッテリー`/`ノーマル`/`スペシャル`) at
  y≈448–511; block `(896,256)` holds the main-menu strips (`ちゅーとりある`,
  `のーまる げーむ`, `ふりー ぷれい`, `すとろーく げーむ`, `でーた せーぶ`,
  `にゅーげーむ`, `こんてぃにゅー`, `ハイスコア`) and `(896,384)` the high-score
  table (`１位/２位/３位`, `なまえ`, `のこりバッテリー`, `グループ`); block
  `(768,256)` the pause menu (`このままつづける`, `さいしょからはじめる`,
  `あきらめる`, `そうさタイプのへんこう`, `目標残バッテリー：`) and misc
  (`〜を選択`, `Tブロック…不正です`, `○ボタンでせーぶへ!!`); `(960,~440)`
  `データ１〜４`. (`CRASH`/`CLEAR!!`/`EMPTY` strips are already English.)
- The sheet arrives as four `GP0 0xA0` LoadImage uploads of 64×256 at
  `(768,256) (832,256) (896,256) (960,256)` (~boot frame 1500; ring-attributed
  to the libgpu LoadImage path, guest buffer transient — a CD-read bounce
  buffer, which is why the RAM scan finds nothing).
- At stage load the game MoveImages whole label strips into a HUD cache row:
  `src=(834,352) 18×16` + `src=(852,352) 18×16` → `dst=(642..,32)`, digits
  blitted separately from the big-digit row y=336 (ring `bld[]` chains:
  `0x80041E78`/`0x80041EA4` return sites). Patching the SHEET therefore fixes
  every downstream consumer.

### Mechanism (general, runtime): patch rides the game's own upload
`text_xlate.cpp` gains a third layer, `[[vram_patch]]`: on completion of every
CPU→VRAM transfer (`gpu.c` GP0 0xA0 state machine, both completion paths call
`text_xlate_vram_upload(x,y,w,h)`), any configured rect fully contained in the
upload is verified **halfword-for-halfword** against `src_hex` (expected JP
pixels) and only on an exact match rewritten with `<lang>_hex` through
`gr_vram_read`/`gr_vram_write` — the renderer facade the upload itself uses, so
software / GL / supersampling mirrors all stay coherent. Re-applies on every
matching re-upload (source-path patch, NOT a one-shot VRAM poke — survives
scene reloads; a different asset at the same coords fails verify and is left
alone). Config lives with the other translation surfaces in
`translations/tsumu.toml`; language-gated like `[[glyph_label]]`. Debug:
`xlate stats` → `vram_patches`/`vram_applied`; `xlate vpatch` force-applies
against current VRAM (verify still gates) for config iteration without waiting
for a re-upload.

### Shipped entries + authoring recipe
Shipped: `hud_stage` `(834,352,18,16)` → `STAGE`, `hud_battery`
`(852,352,18,16)` → `BATTERY` — rects exactly the game's own compose-blit strip
units; the replacement preserves the bullet-dot and colon pixels verbatim and
swaps only the JP text texels for text composed from the sheet's own fullwidth
Latin glyphs (row y=304, 8 texels/glyph, palette nibbles {1=stroke, 2=shadow} —
proven HUD-visible because the y=320 row, same palette, is blitted onto the
HUD). Authoring loop (scratchpad `composestrips2.py`): peek the strip rect →
`src_hex`; overwrite the measured text-texel span with Latin glyph texels →
`en_hex`; preview-render both; append to toml; `xlate reload` + `vpatch` to
test live. Same-day follow-up shipped the rest of the inventory — 37 entries
total: title menu (TUTORIAL/NEW GAME/CONTINUE ×2 variants), pause menu
(CONTINUE/RESTART/GIVE UP/CHANGE CONTROLS + TARGET BATT), results
(STAGES/BATTERY/RANK/REFILL/NORMAL/SPECIAL), mode menu (TUTORIAL/NORMAL
GAME/FREE PLAY/STROKE GAME/DATA SAVE), DATA 1-4, STAGE SELECT, SELECT MOVIE
PIECE, BAD T BLOCK COUNT/LAYOUT, HIGH SCORE + FREE pills, TO CONTINUE / TO
SAVE prompts, high-score headers NAME/BATTERY. Per-strip palette families
matter: pause/mode boxes bg=1 with {c,d,e,f} text (Latin glyphs must be
remapped, e.g. {1→d,2→c}, or the stroke nibble collides with the box bg and
vanishes); gray prompt strips bg=5 keep the Latin {1,2} palette; the
high-score pill is light-on-dark (map {1→f,2→d}). Remaining: グループ box +
１/２/３位 badges, and the level-select レベル１/２/３ labels which are NOT in
this sheet (likely the 8bpp character sheets GRAPH2/3) — separate hunt.
