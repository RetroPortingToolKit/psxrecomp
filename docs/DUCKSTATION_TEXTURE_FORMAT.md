# DuckStation texture-pack interoperability

psxrecomp implements a bounded subset of modern DuckStation texture packs.
The parser, matcher, cache and dumper are independently written here. The
reference version is upstream commit
`697599c47a646a6cfcc4d246018adf4904d55001`, observed on 2026-10-06.
This pin matters for palette-range and alpha behavior.

## Provenance

At the reference pin, DuckStation's
[license](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/LICENSE)
is CC-BY-NC-ND-4.0; its texture-cache and shader files carry the same SPDX
identifier. This project includes no DuckStation implementation, translated
implementation, patch, or binary. Its own code retains the project's
PolyForm Noncommercial terms. Reading format behavior does not provide a
license to incorporate DuckStation's source. Pack artwork has its own rights;
users supply it separately.

The hash implementation comes directly from
[xxHash v0.8.3](https://github.com/Cyan4973/xxHash/tree/v0.8.3), under
[BSD-2-Clause](https://github.com/Cyan4973/xxHash/blob/v0.8.3/LICENSE).
The unmodified `runtime/third_party/xxhash.h` has SHA-256
`17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`.
Its license ships as `runtime/licenses/xxhash-NOTICES.txt`.
The [v0.8.0 release](https://github.com/Cyan4973/xxHash/releases/tag/v0.8.0)
made XXH3 output stable across future versions. Thus the upstream v0.8.3
implementation supplies the unseeded XXH3 behavior used by DuckStation's
vendored v0.8.0 without importing any emulator code.

WebP decoding uses only the static decoder from the official
[libwebp 1.6.0 tag](https://chromium.googlesource.com/webm/libwebp/+/refs/tags/v1.6.0),
commit `4fa21912338357f89e4fd51cf2368325b59e9bd9`, under BSD-3-Clause.
The immutable source archive has SHA-256
`923f3382a47a2af185c3240c954cf004428b237bd7317413a95146d01eb4b94b`.
Its copyright, COPYING, PATENTS, and AUTHORS notices ship in
`runtime/licenses/libwebp-NOTICES.txt`. No encoder, command-line utility, or
libwebp DLL is required. PNG/JPEG use the existing shared stb_image implementation.

## Supported files

Choose a game/package folder containing `replacements/` and `dumps/`, or choose
`replacements/` itself. Replacement PNG/JPEG/static WebP images can live in subdirectories. See the
[player workflow](HD_TEXTURE_PACKS.md) for dumping, editing and reloading.
DuckStation documents its serial-based folders and authoring process in its
[texture replacement wiki](https://github.com/stenzek/duckstation/wiki/Texture-Replacement).

Paletted filename stems follow this grammar:

```text
<kind>-<mode>-<sourceHash>-<paletteHash>-<sourceWords>x<sourceHeight>-<offsetX>-<offsetY>-<width>x<height>-P<min>-<max>
kind = texupload | texpage
mode = P4 | P8 | STP4 | STP8
```

Direct stems omit the palette fields:

```text
<kind>-<mode>-<sourceHash>-<sourceWords>x<sourceHeight>-<offsetX>-<offsetY>-<width>x<height>
mode = C16 | STC16
```

Append `.png`, `.jpg`/`.jpeg`, or `.webp`. Hash fields contain exactly 16 hexadecimal digits; either case
is accepted. Dimensions, offsets and palette endpoints are decimal. Source
width is in native VRAM words. Rectangle width and X offset are expanded
texels: four per word for P4, two for P8, one for C16. Height and Y offset are
rows. A page's source size is 64x256, 128x256 or 256x256 words respectively.
These definitions follow the
[upstream filename documentation](https://github.com/stenzek/duckstation/wiki/Texture-Replacement).

The loader validates nonzero sizes, native VRAM bounds, word-aligned horizontal
rectangles, page bounds and palette endpoints. The replacement image's actual
size controls its independent X/Y scale; integer and equal-axis scale factors
are not required. The renderer applies the declared native rectangle's origin
and extent when mapping UVs.

## Hash compatibility

Both texture and palette identities use unseeded XXH3-64 over packed native
PSX words. Words are serialized low byte first. Rectangles concatenate each
row's words without stride padding. `texupload` hashes the complete tracked
post-write source rectangle; `texpage` hashes its declared subrectangle.
Dimensions and palette data do not enter the texture hash.

A full CLUT hashes 16 P4 or 256 P8 words, including bit 15. A P8 CLUT extending
beyond VRAM's right edge hashes only the remaining words, without wrapping.
For reduced `Pmin-max` ranges, the reference implementation hashes the **first
`max-min+1` palette words**, without advancing by `min`; a range whose declared
maximum crosses the edge does not match. It does not revalidate used indices
against the declared range during lookup. psxrecomp preserves this behavior
for interoperability. Changing a palette entry outside that prefix can leave
a replacement matched. Set `ReducePaletteRange: false` to capture full ranges
when that tradeoff is preferable. These
are observations of the pinned
[texture-cache behavior](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/src/core/gpu_hw_texture_cache.cpp).

## Alpha conventions

The decoder preserves raw RGBA bytes. Ordinary P4/P8/C16 replacements treat
alpha below 128 as cutout and alpha at least 128 as occupied, including black.
ST names encode PSX transparency classification: alpha at most 242 marks
STP, while alpha at least 243 marks an opaque texel. Exact RGBA zero is cutout;
ST black with alpha at least 243 also becomes PSX transparent zero. Alpha zero
with nonzero RGB in an ST image remains a semitransparent texel. Actual blending
still uses the primitive's PSX transparency mode. These rules follow the pinned
[replacement merge shader](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/src/core/gpu_hw_shadergen.cpp).

When ordinary and ST records cover the same identity and rectangle, psxrecomp
prefers the primitive's convention; if only the other convention exists it can
still supply the image. Duplicate canonical identities remain ambiguous.
Distinct matching parts are clipped and composed in a deterministic order.

The finalized dumper writes rounded 5-bit-to-8-bit native RGB, reduced CLUT
ranges by default, alpha
zero for native color zero, alpha 255 for occupied ordinary texels, and alpha
143 for occupied STP texels in ST dumps, matching the pinned upstream dump's
mask-expression result. `DumpTextureForceAlphaChannel` instead emits ordinary
names and makes every pixel's alpha 255. Native conversion
was checked against the pinned
[GPU helpers](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/src/core/gpu_helpers.h).

## Current compatibility limits

The module accepts bounded replacement parts clipped to the draw query.
It supports ordinary unchanged uploads and nonwrapping page subrectangles.
OpenGL composes up to 64 matching parts over a native-pixel fallback, with at
most 256 cached compositions, 64 MiB of composed RGBA, and 8192 pixels per side.
`ReplacementScaleLinearFilter` enables color interpolation when scaling parts
while keeping cutout, STP, and opaque classification discrete. The default is
nearest scaling. Decode or composition limits retain native rendering.
Bounded copy handling, partial-overwrite splitting, and adjacent upload
coalescing can be enabled with their configuration keys; their defaults are
off. Splits retain the immutable original source hash and words. Exceeding a
split limit finalizes observed usage and drops the remaining tracked identity.
Coalescing accepts unused, unsplit uploads adjacent to the right or below,
within the configured incoming-write dimensions. Copies normally invalidate
their destination; `ConvertCopiesToWrites` can refresh an existing destination
source's original extent when its active region contains the copy. It does not
clone the source identity. A same-session pack reload
can copy intact upload identities into the newly indexed pack. Loading a
savestate clears them; page matching remains available, while upload matching
needs subsequent guest uploads. The module's rectangle and query caches must
be invalidated on every native VRAM write.

The following cases use native rendering or report an unsupported-file/option
diagnostic:

- Wrapped UV intervals, horizontally wrapped pages and wrapped uploads.
- `vram-write-` XXH3-128 images and legacy unnamed layouts.
- Animated WebP and image payloads outside PNG/JPEG/static WebP.
- Unrecognized or unimplemented `config.yaml` behavior.

`config.yaml` supports root-level scalar options and a bounded `Aliases:` mapping
with literal or quoted filename keys and relative PNG/JPEG/WebP paths under
`replacements/`. A single-line literal/folded block value
is accepted. Escaped strings, multiline values, anchors and general YAML
features are outside the subset. Absolute paths and `..` traversal are
rejected. A direct canonical replacement takes precedence over its alias.
Unknown top-level options are reported by name and are not applied.

The root-level authoring keys and defaults are:

| Key | Default | Behavior |
| --- | --- | --- |
| `DumpTexturePages` | `false` | Select page capture instead of upload tracking. |
| `DumpFullTexturePages` | `false` | Capture full 256x256 pages in page mode. |
| `DumpTextureForceAlphaChannel` | `false` | Force opaque alpha and ordinary names for texture dumps. |
| `DumpC16Textures` | `false` | Include direct-color texture captures. |
| `ReducePaletteRange` | `true` | Emit the used palette-index range, with the pinned prefix-hash convention. |
| `ConvertCopiesToWrites` | `false` | Refresh bounded existing destination-source tracking after copies. |
| `MaxVRAMWriteSplits` | `0` | Bound partial-overwrite source splitting; zero disables it. |
| `MaxVRAMWriteCoalesceWidth`, `MaxVRAMWriteCoalesceHeight` | `0` | Bound incoming uploads eligible for adjacent coalescing. |
| `DumpTextureWidthThreshold`, `DumpTextureHeightThreshold` | `16` | Minimum finalized texture dimensions in texels. |
| `DumpVRAMWriteForceAlphaChannel` | `true` | Recognized upstream background-write option; that separate dump mode is unsupported. |
| `DumpVRAMWriteWidthThreshold`, `DumpVRAMWriteHeightThreshold` | `128` | Recognized background-write thresholds; do not enable XXH3-128 dumping. |
| `ReplacementScaleLinearFilter` | `false` | Interpolate composition colors within the nearest pixel's PSX alpha class. |
| `MaxHashCacheEntries` | `256` | Rectangle-hash cache entries, capped at 4096. |
| `MaxHashCacheVRAMUsageMB` | `64` | Source-snapshot budget in MiB, capped at 64. |
| `MaxReplacementCacheVRAMUsage` | `64` | Decoded replacement budget in MiB, capped at 64. |

Use these keys at the root, not inside `Options:`. General YAML structures are
unsupported. Existing full-range replacement filenames remain valid even when
new captures use reduced ranges. The Tomba example's `config.yaml.example`
documents the defaults without activating or overwriting a user's config.
The cache keys are compatibility mappings to this implementation's bounded
CPU storage: `MaxHashCacheVRAMUsageMB` controls source snapshots, rather than
reproducing DuckStation's GPU hash-cache allocation.

## Bounds and lifecycle

The loader hard-fails above 262144 candidate files or entries, scans at most
1048576 directory entries and limits recursive nesting to 16 levels. Duplicate
identities are marked ambiguous. Matching indexes upload hashes and page
geometry, with 256 rectangle hashes and 256 positive/negative query results.
Native page/CLUT writes invalidate affected cached queries. A query requiring
over 4096 candidate page geometries or more than 4 MiB of new hashing reports
an error and renders natively.

One low-priority background worker performs PNG/JPEG/WebP decoding and PNG dumping. The
decode queue is bounded to 32, decoded metadata to 512 entries, and decoded
pixels to a default 64 MiB budget. Encoded files are limited to 64 MiB and each
image dimension to 8192 before decoding. Pixel leases survive eviction and
pack destruction. Complete compositions retain their source pixels and can
be reused after source PNGs leave the decoded cache. OpenGL also reuses
GPU-resident full replacements without decoding them again; its image cache
holds at most 256 entries within the existing 128 MiB budget. Decoding failure
leaves native rendering active.

Dumps accumulate the union of word-aligned used rectangles per source and
palette, including semitransparent draw usage. Changed observations are
checkpointed approximately once per second during draws, so long-lived
backgrounds appear while capture remains enabled. Checkpoints retain the
original words, captured palettes, and crop union; later usage can enlarge
the crop. Unchanged captures are deduplicated. Sources also finalize when
they retire or capture is flushed.
Default upload mode emits `texupload`; optional page mode emits `texpage`.
Original source snapshots are bounded to 64 MiB by default, with at most 8192
tracked sources/uploads and 8192 palette records; pressure retires sources and
finalizes their observed usage. The write queue is bounded to eight jobs and
32 MiB of queued RGBA, applying backpressure instead of silently losing captures,
and dump deduplication to a rolling 8192 identities. Exiting the game flushes
remaining sources and waits for PNG writing. A temporary file is published
atomically without replacing an existing final file. Write failures remove
only the created temporary and are returned by capture/flush operations.
All snapshots decode native VRAM rather than replacement
pixels.

Texture identities describe upload/page pixels, palette ranges, source sizes,
and used rectangles. A PSX sprite assembled from several uploads or palettes
can still produce separate parts; palette animation can produce several IDs.
Different histories, supported options, wrapping, or composition requirements
can still differ from DuckStation. This is bounded interoperability, not a
promise that every real game's dumps have identical filenames or reconstructed
sprite images. Alias names can make editing easier without changing identity.

## Independent validation

`runtime/tests/test_duckstation_texture_pack.cpp` contains fixed XXH3 vectors
generated with the separate Python xxhash 3.6.0 native wheel. Input word `i`
is `((i * 7919) ^ 0xa51c) & 0xffff`, serialized little endian. Examples:

| Word count | XXH3-64 |
| --- | --- |
| 0 | `2D06800538D394C2` |
| 4 | `3631D2A04EC85311` |
| 8 | `000CA76CB9B329DC` |
| 16 | `9CBCDEA9305D7528` |
| 256 | `0005D0B83A005F4E` |
| 8192 | `C68A00DCCA652920` |

The test also checks literal filenames, strided native rectangles, crop
coordinates, CLUT edges and reduced-prefix behavior, positive/negative cache
invalidation, reload identity copying, duplicate ambiguity, alias diagnostics,
raw alpha thresholds, Unicode folders, background decoding, native dump pixels,
worker shutdown and preservation of existing edited files. Renderer fixtures
exercise the alpha classification and native-VRAM authority separately.

`texture_image_decode_test` uses synthetic literal-pixel fixtures generated by
Pillow 12.3.0. Windows Clang and Linux GCC checks cover raw PNG/lossless WebP
RGBA (including alpha 0, 1, 128, 254), baseline/progressive JPEG, lossy WebP,
dimensions, encoded/decoded bounds, truncated input, and explicit animation
rejection. Decoders accept bytes supplied by the background loader and perform
no filesystem access themselves.

An official pinned DuckStation binary was also used as an external oracle:
`0.1-12074-g697599c47 (dev)`, development ZIP SHA-256
`4813f22823e6aa262c46a4a516f10b01e1e2ddf6fcea82763e359ef9ac812a9d`.
The owned standalone PS-X probe contains no game artwork. It uploads 10x56
words at VRAM (640,0), repeating `3210 7654 BA98 FEDC` 140 times. Palette A at
(0,256) is zero followed by `(i*1537)&0x7fff` for indices 1..15; palette B at
(0,257) is zero followed by `((i*293)&0x7fff)|0x8000`. It draws a 32x32 ordinary
head and nested 36x32 semitransparent head at UV (0,0) with A, then a 40x24
ordinary body at UV (0,32) with B. Overwriting the complete upload retires it.
The reference produces exactly these two lifetime-union files:

| Reference filename | Decoded RGBA SHA-256 |
| --- | --- |
| `texupload-STP4-18DDF9B731440D20-0DAE5F73C88719B1-10x56-0-0-36x32-P0-15.png` | `5d1bec29956fb3979547f18c9ccc719903924bd9c9d70584913d363b67248ca7` |
| `texupload-P4-18DDF9B731440D20-AF506869B80F5CE7-10x56-0-32-40x24-P0-15.png` | `1a26a7b32dc147cd952364bc59290eaaa8f973fb1c2201c84b9e2756a7bc3479` |

psxrecomp produces the same two filenames and dimensions, with every decoded
RGBA byte identical to those reference PNGs. The module regression preserves
the literal oracle names; the primary audit compared the actual files independently.

This controlled oracle isolates naming, lifetime union, palette hashing, and
native pixel conversion. It does not establish full Tomba gameplay identity
equivalence, and no reference binary or proprietary game pixels are distributed.
