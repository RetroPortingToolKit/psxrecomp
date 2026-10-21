# Native text and donor assets

`runtime/include/native_text.h` provides the reusable text side of a native
translation mod. It complements the existing draw-hook translation support:
adapters can encode plain Unicode back into original guest text and font data,
then let the guest renderer consume it.

Catalog schema 1 keeps a stable `id`, readable `original`, editable
`translation`, and lowercase `source_sha256` for each entry. The digest identifies
the exact original native record. An empty translation means native fallback.
The adapter validates source digests against its title's records before making
changes. Duplicate IDs, malformed Unicode and missing digests are errors.

`NativeGlyphMap` maps Unicode scalars to native glyph indices.
`native_text_encode` wraps words and emits adapter-supplied line, page and final
control bits. Unsupported characters and words exceeding the native width fail;
they never silently become spaces. A title owns its dimensions, style bits,
source record inventory, buffer capacity and alignment requirements.

`native_font_compose` copies a cell in the original indexed 4-bpp font to an
unused cell and composes an acute accent, tilde, diaeresis or inverted mark.
Native palette indices, base shape and baseline are retained. The adapter can
omit top rows of the copied shape to remove i's dot before composing í. Original
cells remain unchanged, and occupied destinations are refused. Other native
font layouts need their own explicit adapter; there is no host-font fallback.

For asset-backed games, an activation callback can use
`psx_mod_replace_disc_file` to mount a derived file without modifying the owner
disc. The runtime copies its bytes into an immutable Mode-2 Form-1 extent and
updates both endian halves of its ISO directory record. Host file readers and
guest CD reads resolve the same final file. Multiple replacements of the same
file layer in activation order. Private native LBA/size tables remain the
title adapter's responsibility. Use only verified committed resources and
trusted adapter code to derive these bytes; paths, clocks and host fonts must
not affect their content.

The `psx-mode2` verified resource format retains 2336 bytes per raw Mode-2
sector, including XA subheaders and audio/video payloads. Its identity differs
from the existing cooked `psx-disc` format. Cooked ISO images cannot satisfy this
contract. `ModMode2Disc` performs bounded ISO9660 lookup and ordinary file reads
against this immutable snapshot; raw streams mount through
`psx_mod_append_disc_extent` and the original CD decoder.

Register a session-reset callback for title-owned preparation caches. Activation
reconstructs them from the committed plan; reset retires stale data even when
the next session disables the feature. Save-state identity includes verified
catalog/donor content through the ordinary mod plan. Adapters still need title
playback, visual and audio qualification, plus any relevant co-op or netplay
qualification.

Mega Man X4 is the first asset adapter. Its source keeps archive and message
format knowledge outside the framework. Focused coverage is in
`native_text_test`, `mod_media_test` and `mod_runtime_test`, including malformed
directory records, cross-sector reads, layered replacement, native directory
parity, immutable sources, session cleanup and edited catalog checksums.
