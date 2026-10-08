# DuckStation texture format, xxHash, and libwebp provenance

The independent format integration is described in
[DuckStation texture format notes](../../DUCKSTATION_TEXTURE_FORMAT.md).
Reference behavior was observed at DuckStation commit
[697599c47a646a6cfcc4d246018adf4904d55001](https://github.com/stenzek/duckstation/tree/697599c47a646a6cfcc4d246018adf4904d55001),
whose [license](https://github.com/stenzek/duckstation/blob/697599c47a646a6cfcc4d246018adf4904d55001/LICENSE)
is CC-BY-NC-ND-4.0. DuckStation is authored by Stenzek and its contributors;
its authorship is credited by the pinned reference links. No DuckStation
implementation, translated implementation, patch, or binary is included,
and no DuckStation co-author trailer is claimed for this independent work.

Implemented behavior: bounded modern texture filenames and palette ranges,
XXH3 identities over native pixel sources and palettes, alpha classification,
replacement loading, and source-lifetime original-texture dumping. The fidelity
followup independently adds root-level authoring options, bounded copy/split
and coalescing tracking, and multiple replacement parts. Wrapped footprints,
XXH3-128 background-write images, general YAML, and DuckStation's rendering/cache
implementation remain excluded. The format guide records the exact limits.

The checked-in hash implementation is unmodified
[xxHash v0.8.3](https://github.com/Cyan4973/xxHash/tree/v0.8.3), authored by Yann
Collet and contributors under
[BSD-2-Clause](https://github.com/Cyan4973/xxHash/blob/v0.8.3/LICENSE).
`runtime/third_party/xxhash.h` retains its original copyright and license;
its SHA-256 is
`17973c0dc49d9854ca26caa191f0e12f7a424b68858d9a78de3860d959d85e4b`.
Player packages carry `runtime/licenses/xxhash-NOTICES.txt` as a runtime
notice. Vendoring preserves authorship in the unmodified source and notices,
rather than attributing that source to this change's author.

Static WebP decoding uses unmodified
[libwebp 1.6.0](https://chromium.googlesource.com/webm/libwebp/+/refs/tags/v1.6.0),
commit [4fa21912338357f89e4fd51cf2368325b59e9bd9](https://chromium.googlesource.com/webm/libwebp/+/4fa21912338357f89e4fd51cf2368325b59e9bd9),
authored by Google Inc. and WebM contributors under BSD-3-Clause. The immutable
archive SHA-256 is
`923f3382a47a2af185c3240c954cf004428b237bd7317413a95146d01eb4b94b`.
Authorship and license credit remain in the upstream source and original
COPYING, PATENTS, and AUTHORS texts in `runtime/licenses/libwebp-NOTICES.txt`.
No decoder code is translated or attributed to this integration's author.
Only the static decoder is linked; encoder, utilities, and DLL dependencies
are excluded. PNG/JPEG reuse stb_image's existing implementation.

The bounded image helper was rebuilt and checked with Windows Clang and Linux
GCC against synthetic fixtures: raw PNG/lossless WebP RGBA, baseline/progressive
JPEG, lossy WebP, header dimensions, encoded/decoded bounds, truncated images,
and explicit animated-WebP rejection. It reads supplied bytes and performs no
filesystem access. This focused check does not claim full-game dump parity.

The fidelity reference was additionally run as the official development binary
`0.1-12074-g697599c47`, ZIP SHA-256
`4813f22823e6aa262c46a4a516f10b01e1e2ddf6fcea82763e359ef9ac812a9d`.
An owned synthetic PS-X probe supplies a 10x56-word upload, nested head draws,
a separate body palette, and a full overwrite. Its input formulas, exact two
output identities, and decoded RGBA digests are documented in the format guide.
The reference confirms lifetime unions and ST dump alpha 143 for that controlled
case. It supplies no proprietary artwork and makes no full-game parity claim.

Validation on current framework master `92c0f3cd`: registered DuckStation
module and OpenGL renderer tests pass, alongside mod package, builtin
manifest, runtime, and resident tests. Independent xxHash wheel vectors,
literal filenames, PNG pixels and file preservation, cache/reload behavior,
alpha handling, upload aborts, and native VRAM invariance are covered.
Tomba USA title and Village gameplay showed synthetic replacements, with
live off/reload/on and stable native texture atlas comparisons. Full scene
coverage and third-party artwork packs were not claimed; the first consumer
needs a framework pin update and runtime rebuild, without a game C regen
for the HD texture feature itself.
