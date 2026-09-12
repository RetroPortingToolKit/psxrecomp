# Ahead-of-time overlay sharding

Overlays can be recompiled before gameplay without first decompiling the game.
The necessary knowledge is narrower: which disc bytes become executable, where
they load, how they are transformed, and which entry points and dependencies
can be established. Full source recovery is not a prerequisite.

The goal is to prepare native coverage from original inputs so first visits do
not depend on runtime capture and compilation. Interpreter and runtime-compiler
fallbacks remain useful until individual titles demonstrate sufficient coverage.
An inventory of every overlay file is not proof of every execution path.

## What exists today

The framework already separates discovery from compilation:

| Layer | Existing implementation | Responsibility |
| --- | --- | --- |
| Disc/container discovery | `tools/aot_overlay_spike/extract_generic.py`, `tools/extract_overlays.py` | Read disc files and recognized archives; recover candidate load layouts and static roots. |
| Native production | `tools/compile_overlays.py`, recompiler function analysis | Walk code, recover supported indirect dispatch, compile and audit native candidates. |
| Cache selection | Runtime overlay loader, `.dll`/`.ranges` pairs | Match the game, build/flavor ABI and current guarded bytes before dispatch. |
| Release staging | `tools/release_overlay_stage.ps1` and shared packaging helpers | Stage matching cache namespaces and fallback tooling. |

The extractor lives under `aot_overlay_spike`: it is useful tooling with partial
format support, not a universal overlay detector. Per-title evidence and recipes
are still required when an input format or loader layout is ambiguous. The production pipeline below adds declarative profiles for verified methods;
arbitrary game loaders still require bounded investigation.

The compiler's input option is named `--captures` for historical reasons. It
also accepts records produced entirely from disc bytes. That filename/schema
does not imply a playthrough or RAM capture is required.

## Production pipeline and declarative profiles

`tools/aot_overlay_pipeline.py` owns extraction, inventory checks, native builds,
original-byte auditing and staging. Games supply JSON facts with schema
`psxrecomp AOT methods v1`; they do not carry copies of extractors or auditors.
The pipeline contains no game-ID branches.

```sh
python tools/aot_overlay_pipeline.py release \
  --profile /game/aot/overlays.json --game-toml /game/game.toml \
  --runtime-config /game/packaging/release/game.toml \
  --recompiler /framework/recompiler/build/psxrecomp-game \
  --work-dir /private/aot-build --stage /release/payload --gcc gcc --workers 3
```

Use `extract` instead of `release` and omit `--stage` for bounded discovery.
Every invocation creates a fresh work directory and reads the original disc;
existing shards and gameplay captures are never inputs. `--runtime-config`
selects the packaged code-generation settings/cache namespace while the source
project supplies BIOS profiles and extraction inputs. Never rename cache tags
to force compatibility. Python 3.11 or newer is required.

Profiles declare these reusable methods:

| Field / method | Contract |
| --- | --- |
| `disc_hashes` | Hash the cue's original data track before extracting. |
| `images[].method = fixed_address_files` | Whole original files placed at a verified `load_addr`. Optional duplicate-leaf checks require identical bytes. |
| `images[].method = tagged_relocated_files` | Apply a trailing two-bit ABS32/HI16/LO16/J26 relocation stream to a verified `load_addr`, retaining only the original loader's image interval. Destinations are explicit evidence, never inferred from a historical RAM capture. |
| `images[].method = psx_exe` | Read the load address and image from a PS-X EXE header; the generic extractor may split resident and overlay floors. |
| `images[].method = packed_sector_members` | Decode u32 count/offset sector descriptors across ordered payload files. Verify every member classification and requested full-payload coverage; use loader-established addresses for executable members. |
| `images[].method = sector_extent_members` | Decode `{sector offset, byte size}` pairs in an archive. Use archive-relative offsets, verify the exact count and terminator, reject gaps/overlaps, and account for every member after sector rounding. Each member needs a verified load address or an explicit exclusion. |
| `images[].method = aligned_lzss_banks` | Read aligned stored-size pairs, decode the metadata and its tagged bank members using parameterized LZSS, verify every container and bank classification, and deduplicate exact decoded images at verified destinations. External-RAM references fail extraction. |
| `checks[].method = words` | Verify loader instructions or descriptors at explicit file offsets / virtual addresses. |
| `checks[].method = tagged_relocations` | Validate a complete relocation stream, retained `image_size` and `relocation_count`, including modules whose heap placement remains unresolved and which are excluded from native production. |
| `checks[].method = pointer_strings` | Verify a pointer-indexed filename table against exact expected strings. |
| `checks[].method = bcd_extent_table` | Verify an indexed BCD-MSF/size table against an ISO file's actual extent. |
| `checks[].method = adjacent_files` | Require the named ISO extents to be sector aligned and physically adjacent, in order. |
| `compositions` | Combine declared simultaneous producers in address order. `max_gap` bounds their separation; gap and alignment bytes remain unowned. |
| `expected_records` | Stop on inventory drift, including unexpected additions requiring review. Every configured image must be represented. |
| `strict_bounds` | Require emission to remain inside established producer intervals. |
| `images[].excluded_ranges` | Explicit fallback intervals with aligned start/end, original-byte SHA-256 and a reason. Requires strict bounds; required loader entries cannot be excluded. |

`allow_missing` opts a fixed-address source into static root discovery when the
generic detector misses it. `entry_word` or `entries` can supply independently
verified loader entry points. These facts do not turn every plausible code word
into a proven function. There is no recipe option to ignore a failed compiler.

Each recipe compiles in an isolated cache. `tools/audit_aot_cache.py` checks every
native pair's ABI/exports, every manifest guard against known original bytes,
and nonzero native coverage for every recipe. Loader entry points supplied by
the profile must also have native entries guarded by that image's original bytes.
Staging copies precisely the
audited platform pairs, verifies hashes again, rejects extras, and emits
`AOT_CACHE_AUDIT.json` without game bytes. A missing disc, changed loader word,
missing image, compiler failure, empty recipe coverage or failed audit stops
release packaging. Pair counts are results, not inherited success thresholds.
An inventory plus valid guards still does not prove complete static execution
coverage or native semantics; keep fallback and perform gameplay spot checks.

Jersey Devil uses `aligned_lzss_banks` for five renderer variants across 70 bank occurrences and `psx_exe` for five secondary executables. Its profile accounts for all 93 BZZ containers, including 23 without code banks.

Exact-BIOS resident helper recipes also publish a `.resident` preload marker.
The pipeline preserves it through consolidation and staging, verifies its BIOS
hash against the recipe, and includes its hash in the audit receipt. Omitting
that marker can leave an otherwise valid helper unused at runtime.

Current consumers:

- Tomba! USA: `TombaRecomp/aot/overlays.json`, 25 fixed-address images, including
  17 area variants and eight support images.
- Tomba! 2 USA / Tombi! 2 Italian: `Tomba2Recomp/aot/{usa,ita}.json`, each with
  MAIN.EXE and 28 raw images, including all 22 area overlays. Their loader tables
  establish different regional addresses; composition uses the same shared
  method (including the Italian gap). US START.BIN is explicitly accounted for.
- Ape Escape USA: `ApeEscapeRecomp/aot/overlays.json`, 47 established packed
  sector-table overlays plus two minigame PS-X EXEs. Two additional archive
  members have unresolved load paths and are explicitly excluded.

For `packed_sector_members`, declare `table_file`, ordered `payload_files`,
`sector_size` (default 2048), `offset_bits` (default 20), and optional
`table_offset`. A descriptor's low bits are the logical sector offset and its
remaining high bits are the sector count. Members cannot cross payload-file
boundaries. `inventory_range` gives inclusive `first`/`last` descriptor indices;
every index must occur once in either `members` or `excluded_members`.
Executable members carry `index`, `load_addr`, and optionally verified `entries`.
Exclusions require a `reason`. `cover_payloads` names files that this entire
classified range must cover exactly, without gaps or overlaps. The shared reader
is `tools/packed_sector_table.py`; heuristic discovery uses that same parser.

See `tools/tests/test_aot_overlay_pipeline.py` for synthetic method and release
failure tests. A new title should add a profile when these contracts fit; add
and test a shared method only when its actual loader requires one.

## The reusable boundary

A title-specific producer should supply facts about memory images. The shared
compiler should consume those facts without knowing game names or addresses.
Keep each producer responsible for:

- Source identity: disc revision, file/member offsets, sizes and hashes.
- Transform: verbatim load, decompression or relocation, with the inputs needed
  to reproduce its result. Record how the loader establishes these facts.
- Destination: address of each actual file/member and the combinations that can
  coexist. Regional editions need their own source and layout verification.
- Known byte intervals and entry evidence: exports, loader entry tables, direct
  calls or supported static discovery. Preserve the distinction between function
  roots and interior dispatch targets.

Common container readers, decoders, relocation handling and instruction-pattern
recognizers belong in the framework once their contracts can be stated and
tested independently. A game's selector table or file naming convention belongs
in its recipe. Repeated methods can then be reused without treating a previous
title's addresses, hashes or runtime captures as authority for a new one.

Useful producer families include fixed-address raw files, self-describing PS-X
EXEs, archive members, deterministic compressed images and relocatable images.
The first three have existing extractor support for specific formats. The last
two need a verified transform; arbitrary compression/relocation is not inferred
automatically. Code that depends on runtime state may need multiple bounded
variants or continued fallback.

## A bounded discovery and build

1. Identify the exact disc revision and enumerate candidate files. Hash original
   inputs before attempting extraction. Include shared code and loader helpers,
   not only files named like areas.
2. Establish destinations and entry points from headers and loader behavior.
   Treat heuristic votes as candidates. If two bases nearly tie, investigate the
   specific loader path or skip the file; do not select the convenient answer.
3. Emit independently reproducible memory-image recipes with known ranges.
   Build standalone producers first, then justified shared-code combinations.
4. Compile using the same framework headers, compiler configuration, backend,
   architecture and flavor as the runtime being shipped.
5. Audit published pairs and exercise transitions with runtime compilation off.
   Retain fallback and document rejected or unresolved candidates.

For formats the generic extractor recognizes, run from the framework directory:

```sh
python tools/aot_overlay_spike/extract_generic.py \
  --game-toml /path/to/game.toml \
  --recompiler /path/to/psxrecomp-game \
  --out /private/aot/disc-records.json \
  --tmp /private/aot/extraction

python tools/compile_overlays.py \
  --captures /private/aot/disc-records.json \
  --game-toml /path/to/game.toml \
  --recompiler /path/to/psxrecomp-game \
  --runtime-include /path/to/psxrecomp/runtime/include \
  --out-dir /path/to/runtime/cache \
  --compiler gcc --gcc /path/to/gcc --flavor 0 --jobs 1
```

These are developer commands; use actual executable paths (`.exe` on Windows).
Flavor 0 is an example matching the baseline Tomba 2 tests, not a universal
setting. Other native flavors need matching artifacts. The full command can
perform additional cross-variant discovery. To bound expensive investigation,
compile one producer record per invocation into the same cache, then deliberately
add supported combinations. That trades some discovery breadth for a predictable
scope; it does not establish completeness.

Disc recipes must explicitly declare `guard_bytes: 0` when no runtime-capture
guard word was appended. The legacy capture heuristic can otherwise trim four
real bytes from an image whose length is four bytes beyond a page boundary.
The generic extractor now emits this declaration; verify it when adapting older
output or adding a custom producer. This field concerns
capture padding; native function guards still apply.

Declare `producer_ranges` using half-open `[start, end)` intervals for bytes
owned by each original source. A sparse image may have padding between files.
Padding used to serialize that image is not evidence of actual RAM contents.
Do not seed, discover or guard code across unknown intervals. If a candidate
really depends on those bytes, establish their producer or leave it unsupported.

## Validation and release gates

Keep three different claims separate:

| Claim | Evidence needed |
| --- | --- |
| The memory image was reconstructed correctly | Exact original-file/transform checks, loader destination evidence and bounded known ranges. |
| The cache can be selected safely | Matching namespace/flavor and ABI, exports, pair identity, all declared guard and delay-slot bytes, index capacity. |
| The translated game behavior works | Meaningful execution checks and player movement, interaction, audio, graphics and area round trips. |

Audit every published pair, including supplemental fragments. A manifest row
count is not a count of independently proven functions. Multiple recipes may
reuse the same guarded native function when its required bytes match; a separate
DLL per combination is unnecessary. Conversely, a successful file inventory
cannot excuse a compile/audit rejection. Keep nonzero outcomes visible and
review any accepted partial result explicitly.

During a static-cache playtest set `PSX_OVERLAY_AUTOCOMPILE_OFF=1` and omit the
configured runtime compilation command. Inspect `autocompile_status` to verify
that compilation is disabled and no builds ran. Inspect `overlay_loader_status`
for native dispatch, loaded candidates and capacity overflows. Runtime capture
or live RAM observations may help identify an exercised area; they must not
silently become inputs to a supposedly disc-only build.

Existing captured shards are investigative references, not correctness or
completeness oracles. Matching their entry list or containing every observed PC
does not prove correct function boundaries, emitted semantics, or coverage of
unvisited paths. Byte guards prevent selection of mismatching bytes; they do
not prove that the matching translation is correct. Interpreter fallback is
also an implementation to validate, not an unconditional correctness guarantee.

For release, recompute the cache tag from the packaged configuration with the
shared tooling; never rename a cache directory to make it look compatible.
Validate every staged pair, report omitted candidates, keep user saves/disc
images/capture JSON out of packages, and perform a smoke test of the packaged
runtime. Preserve input hashes, source revision, commands and audit receipts so
the prepared coverage can be regenerated.

## Tomba 2: the same method across two regions

The US and Italian discs each contain 22 area files (`A00` through `A0L`). The
generic extraction method recovered both sets, but the bytes and addresses
differ: US areas load at `0x80108F9C`; Italian areas load at `0x8010A444`.
Each region therefore needs its own verified recipes and cache namespace.

The Italian shared loader demonstrates why limited reverse engineering still
matters. The generic extractor could not confidently choose GAME's base. The
original START filename table and MAIN loader established that START, DEMO and
GAME share destination `0x80107448`. GAME ends 372 bytes before the area starts.
Declaring the two original-file ranges separately allowed shared/area recipes
without inventing the contents of that gap. No full decompilation was required.

The Italian test validated 77 recipes and 74 published native pairs; many GAME
combinations reused matching functions. All 22 area files had AOT candidates.
The owner reported great performance in water temple and Kujara Ranch with
runtime compilation disabled. These are bounded build and gameplay results,
not proof that every overlay path runs natively. One earlier watchdog abort
was not reproduced in the longer repeat and was not assigned a speculative fix.

The shared switch recognizer also gained support for a compiler scheduling form
with table-address setup around the bounds branch. Register dependencies,
producer ownership and table-target checks remain required. That improvement
contains no Tomba-specific addresses and can benefit other matching binaries.

## When discovery finds no game overlay

An archive extension or a historical dirty-RAM capture does not establish an
executable overlay. A resident-code game is a valid outcome of bounded disc
inspection. Do not count an independently identified BIOS RAM helper as a game
overlay, or manufacture a release inventory from runtime observations.

`tools/inspect_disc_inventory.py` verifies a declarative `psxrecomp disc inventory
v1` profile against the original disc. It reuses the AOT evidence-word checks,
matches fixed-stride `{LBA, byte-size}` tables to ISO files, and exposes
`typed_members(data, alignment)` for archives with a `{count, total-size}` header
and `{type, byte-size}` descriptors followed by aligned members. Repeated types
are allowed; declared size, count, bounds and full payload coverage are required.
Header bytes beyond the counted descriptors are opaque.

The metadata-only report accounts for every ISO file and reports member hashes,
types and aligned MIPS instruction shapes. These shapes are observations, not
proof of code presence or absence; loader destinations, callbacks and any
decompression path still need bounded inspection of the original executable.
The game profile supplies all addresses, filenames, counts and rationales. The
shared tool contains no game-specific branches or bundled game bytes.

```sh
python3 psxrecomp-v4/tools/inspect_disc_inventory.py \
  --profile aot/disc_inventory.json --cue "path/to/original-disc.cue" \
  --output build/disc-inventory.json
```

## Related material

- [Compiling and packaging overlays](COMPILING_OVERLAYS.md): cache and static-link workflows.
- [Historical AOT investigation](AOT_OVERLAY_PLAN.md): dated experiments and observations.
- [Extractor tooling](../tools/aot_overlay_spike/README.md): supported producers and limitations.
- [Overlay cache format](OVERLAY_CACHE_V2.md): runtime selection and invalidation.
- [Post-decompression data shards](DATA_SHARDS.md): a separate experiment for replaying data transforms.
