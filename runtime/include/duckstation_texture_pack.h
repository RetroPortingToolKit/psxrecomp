#ifndef PSXRECOMP_DUCKSTATION_TEXTURE_PACK_H
#define PSXRECOMP_DUCKSTATION_TEXTURE_PACK_H

#include "hd_texture_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DuckTexturePack DuckTexturePack;

enum DuckTextureKind { DUCK_TEXTURE_UPLOAD = 0, DUCK_TEXTURE_PAGE = 1 };

/* Independent representation of the modern filename wire format. Source
 * width is in VRAM words; offsets and rectangle sizes are expanded texels.
 * Hashes are unseeded XXH3-64 over explicitly little-endian native words. */
typedef struct DuckTextureKey {
    uint64_t source_hash;
    uint64_t palette_hash;
    uint16_t source_width_words;
    uint16_t source_height;
    uint16_t offset_x;
    uint16_t offset_y;
    uint16_t width;
    uint16_t height;
    uint8_t kind;
    uint8_t depth; /* enum HdTextureDepth */
    uint8_t semitransparent; /* STP4 / STP8 / STC16 alpha convention */
    uint8_t palette_min;
    uint8_t palette_max;
} DuckTextureKey;

typedef struct DuckTexturePackInfo {
    const char* root;
    const char* diagnostic; /* unsupported config/formats and malformed names */
    size_t replacement_count;
    size_t ambiguous_count;
    size_t ignored_count;
    size_t queued_dump_count; /* Cumulative jobs actually submitted to writer. */
    size_t pending_dump_sources;
    size_t pending_palette_records;
    size_t snapshot_bytes;
} DuckTexturePackInfo;

typedef struct DuckTextureMatch {
    DuckTextureKey key;
    uint64_t entry_id;
    const char* replacement_path;
    /* Origin of replacement rectangle relative to current texture page, in
     * texels. May be negative for uploads spanning more than one page. */
    int32_t origin_u;
    int32_t origin_v;
    uint16_t source_width;
    uint16_t source_height;
    /* Intersection with this draw and the surviving native source region.
     * These page-space texels clip the full replacement mapping above. */
    int32_t clip_u;
    int32_t clip_v;
    uint16_t clip_width;
    uint16_t clip_height;
} DuckTextureMatch;

typedef struct DuckTextureConfig {
    uint16_t max_vram_write_splits;
    uint16_t max_vram_write_coalesce_width;
    uint16_t max_vram_write_coalesce_height;
    uint16_t texture_dump_width_threshold;
    uint16_t texture_dump_height_threshold;
    uint16_t vram_write_dump_width_threshold;
    uint16_t vram_write_dump_height_threshold;
    uint8_t dump_texture_pages;
    uint8_t dump_full_texture_pages;
    uint8_t dump_texture_force_alpha_channel;
    uint8_t dump_vram_write_force_alpha_channel;
    uint8_t dump_c16_textures;
    uint8_t reduce_palette_range;
    uint8_t convert_copies_to_writes;
    uint8_t replacement_scale_linear_filter;
} DuckTextureConfig;

typedef struct DuckTexturePixels {
    const uint8_t* rgba;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    void* lease;
} DuckTexturePixels;

/* Accepts a game/package folder containing replacements/ and dumps/, or the
 * replacements directory itself. Missing folders are valid empty packs, so
 * a new pack can start by dumping. Recursive image scan; no game assets bundled.
 * PNG, JPEG and WebP payloads; unsupported options and invalid names are
 * diagnosed. Upstream config.yaml keys are root-level scalar options. */
int duck_texture_pack_create(const char* root, DuckTexturePack** out_pack,
                             char* error, size_t error_capacity);
void duck_texture_pack_destroy(DuckTexturePack* pack);
void duck_texture_pack_get_info(const DuckTexturePack* pack,
                                DuckTexturePackInfo* out_info);
void duck_texture_pack_get_config(const DuckTexturePack* pack,
                                  DuckTextureConfig* out_config);
int duck_texture_pack_linear_filter(const DuckTexturePack* pack);
uint64_t duck_texture_pack_revision(const DuckTexturePack* pack);

int duck_texture_parse_name(const char* filename, DuckTextureKey* out_key);
int duck_texture_format_name(const DuckTextureKey* key, char* output,
                             size_t capacity); /* stem only, without .png */
uint64_t duck_texture_hash_words_le(const uint16_t* words, size_t count);
/* Native word rectangle must not wrap. Zero on invalid input (also a valid
 * hash value); use valid geometry rather than zero as a success indication. */
uint64_t duck_texture_hash_rect(const uint16_t* vram, size_t count,
                                uint16_t x, uint16_t y,
                                uint16_t width_words, uint16_t height);

/* Track post-write native VRAM, after PSX set/check-mask semantics. Wrapped
 * uploads are invalidated but deliberately not tracked. Immutable original
 * words survive partial overwrites and configurable active-region splits. */
int duck_texture_pack_track_upload(DuckTexturePack* pack,
                                   uint16_t x, uint16_t y,
                                   uint16_t width_words, uint16_t height,
                                   const uint16_t* vram, size_t count);
void duck_texture_pack_invalidate(DuckTexturePack* pack,
                                  uint16_t x, uint16_t y,
                                  uint16_t width_words, uint16_t height);
void duck_texture_pack_reset_tracking(DuckTexturePack* pack);
/* Same-session reload only: clone immutable sources and owned observations
 * while the caller keeps native VRAM unchanged. Lookup/image caches remain
 * independent; only same-root filename deduplication history is shared.
 * This is not a savestate restoration API. Strong replacement on success. */
int duck_texture_pack_copy_tracking(DuckTexturePack* destination,
                                    const DuckTexturePack* source);
/* Begin before changing native VRAM, end after PSX mask semantics. Default
 * copies invalidate destination identities. ConvertCopiesToWrites updates an
 * existing destination write's original extent when its active region covers
 * the copy; it does not clone the source upload into the destination. */
int duck_texture_pack_begin_copy(DuckTexturePack* pack,
                                 uint16_t source_x, uint16_t source_y,
                                 uint16_t destination_x, uint16_t destination_y,
                                 uint16_t width_words, uint16_t height,
                                 const uint16_t* vram, size_t count);
int duck_texture_pack_end_copy(DuckTexturePack* pack,
                               const uint16_t* vram, size_t count);

/* Only one replacement covering the entire inclusive query is accepted.
 * UV wrapping, edge-wrapped pages and multi-image composition fall back.
 * Uses HD_TEXTURE_LOOKUP_* statuses. Query geometry is shared with Beetle. */
int duck_texture_pack_match(DuckTexturePack* pack,
                            const HdTextureDrawQuery* query,
                            DuckTextureMatch* out_match);
/* Prefer ST names on semitransparent primitives and ordinary names otherwise;
 * when only the other convention exists it remains usable. The filename's
 * convention always determines how the renderer interprets its alpha. */
int duck_texture_pack_match_draw(DuckTexturePack* pack,
                                 const HdTextureDrawQuery* query,
                                 int semitransparent, DuckTextureMatch* out_match);
/* All intersecting parts, in composition order: pages before uploads, larger
 * rectangles before smaller ones. Same-coverage ST variants prefer this draw's
 * convention. An insufficient capacity returns ERROR and zero parts, never a
 * silently truncated composition. At most 64 parts are supported. */
int duck_texture_pack_match_parts(DuckTexturePack* pack,
                                  const HdTextureDrawQuery* query,
                                  int semitransparent,
                                  DuckTextureMatch* out_matches,
                                  size_t capacity, size_t* out_count);

/* One bounded background worker; no PNG decoding in the render thread.
 * Pixels retain raw DuckStation alpha bytes. The renderer interprets the
 * key.semitransparent flag; this is not conventional opacity for ST names.
 * Release the lease even after pack destruction. */
void duck_texture_pack_set_decode_budget(DuckTexturePack* pack, size_t bytes);
int duck_texture_pack_request_decode(DuckTexturePack* pack, uint64_t entry_id);
int duck_texture_pack_acquire_decoded(DuckTexturePack* pack, uint64_t entry_id,
                                     DuckTexturePixels* out_pixels);
void duck_texture_pixels_release(DuckTexturePixels* pixels);

/* Records a draw's used region and captured palette, unioning nested crops
 * until the upload/page retires or a flush occurs. Upload-only by default;
 * minimum 16x16, C16 off and reduced palette ranges on. Any semi draw selects
 * ST unless ForceAlpha is enabled. FOUND means new usage was recorded, not
 * that a file was written. Existing files are never overwritten. */
int duck_texture_pack_dump_draw(DuckTexturePack* pack,
                                const HdTextureDrawQuery* query,
                                int semitransparent,
                                char* error, size_t error_capacity);
/* Finalize current usage without discarding live identities, then drain dump
 * writes. Call before disabling dumps/changing folders. Reset/destruction also
 * finalize. Reload copies own independent sources and share same-root filename
 * history, so destroying a copied pack never steals the original's records.
 * Returns FOUND on success (including empty flush), ERROR on failure. */
int duck_texture_pack_flush_dumps(DuckTexturePack* pack,
                                  char* error, size_t error_capacity);
void duck_texture_pack_reset_dump(DuckTexturePack* pack);

#ifdef __cplusplus
}
#endif
#endif
