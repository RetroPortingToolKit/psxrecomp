#ifndef PSXRECOMP_GPU_HD_TEXTURES_H
#define PSXRECOMP_GPU_HD_TEXTURES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emulation-thread configuration. Roots are external user-owned directories;
 * a successful replacement session selects native CPU VRAM authority on GL.
 * Reconfiguration retains the previous session if opening the new root fails.
 * Empty root with both options off clears the current session. */
int gpu_hd_textures_configure(const char* root, int replacements_enabled,
                             int dump_enabled, char* error, size_t capacity);
void gpu_hd_textures_shutdown(void);
int gpu_hd_textures_reload(char* error, size_t capacity);
void gpu_hd_textures_set_dump_enabled(int enabled);
int gpu_hd_textures_replacements_enabled(void);
int gpu_hd_textures_dump_enabled(void);
int gpu_hd_textures_active(void);
typedef struct GpuHdTextureDiag {
    const char* root;
    int active, replacements, dump, format;
    uint64_t draw_queries, matched_draws, ready_draws, applied_draws, dumped_textures;
    size_t replacement_count;
    const char* diagnostic;
    uint64_t pending_dump_sources;
    uint64_t composition_hits, composition_builds, composition_pixels;
    uint64_t gl_uploads, gl_evictions;
    uint64_t decoded_images, decode_evictions, decoded_bytes;
} GpuHdTextureDiag;
void gpu_hd_textures_get_diag(GpuHdTextureDiag* out);
void gpu_hd_textures_note_applied(void);
void gpu_hd_textures_note_gl_cache(int eviction);
/* Non-GL backends can observe native draws for dumping; replacements remain
 * an OpenGL presentation feature. */
void gpu_hd_textures_observe_draw(uint16_t texpage, uint16_t clut_x,
                                 uint16_t clut_y, const int limits[4],
                                 uint32_t texture_window, int semitransparent);

/* Native VRAM and residency hooks. A full restage resets identity rather than
 * pretending a savestate is an original texture upload. */
void gpu_hd_textures_set_vram(const uint16_t* vram);
/* Move to another copy of the same native VRAM (the GL render thread's
 * private copy and gpu.c's array match at every hand-off); keeps residency. */
void gpu_hd_textures_bind_vram(const uint16_t* vram);
/* Smooth motion's generated frames replay real frames' draws for
 * presentation only: their queries never dump (renderer thread). */
void gpu_hd_textures_suppress_dumps(int on);
/* Residency is hashed from the bound native VRAM after the write; words may
 * be NULL (a replayed upload whose payload already reached that VRAM). */
void gpu_hd_textures_track_upload(int x, int y, int width_words, int height,
                                 const uint16_t* words);
/* Header-before-payload invalidation and pre/post native copy observations. */
void gpu_hd_textures_begin_upload(int x, int y, int width_words, int height);
void gpu_hd_textures_begin_copy(int source_x, int source_y, int destination_x,
                                int destination_y, int width_words, int height);
void gpu_hd_textures_end_copy(void);
void gpu_hd_textures_invalidate(int x, int y, int width_words, int height);
void gpu_hd_textures_reset_tracking(void);
/* A full VRAM restage (savestate, rewind): reset residency, then re-admit the
 * Beetle-keyed upload rectangles seen this session whose restored words still
 * carry the same key. */
void gpu_hd_textures_restage(void);
/* Savestate sidecar: the Beetle upload residency at save time, bound to a
 * CRC of the whole native VRAM. Loading it after a restage restores the
 * identities only if the restored VRAM is exactly the VRAM they described,
 * so replacements match again without fabricating an upload. save returns 0
 * when there is nothing to keep (no Beetle-format session); free(*data). */
int gpu_hd_textures_residency_save(uint8_t** data, size_t* size);
int gpu_hd_textures_residency_load(const uint8_t* data, size_t size);

typedef struct GpuHdTextureImage {
    const uint8_t* rgba;
    uint32_t width, height, stride;
    uint64_t cache_key;
    uint64_t generation;
    int32_t origin_u, origin_v;
    uint32_t source_width, source_height;
    /* 1: Beetle (native STP), 2: Duck non-ST, 3: Duck ST alpha encoding,
     * 4: composed cutout/STP/opaque classes (alpha 0/128/255). */
    int alpha_mode;
    void* lease;
} GpuHdTextureImage;

/* Inclusive page-UV bounds, before native raster writes the destination.
 * Texture-window masks are applied to the queried footprint. Pending decode,
 * ambiguity and unsupported geometry return native sampling. Ready partial
 * replacements compose with native holes; pending parts never decode here. */
int gpu_hd_textures_acquire_draw(uint16_t texpage, uint16_t clut_x,
                                uint16_t clut_y, const int limits[4],
                                uint32_t texture_window, int semitransparent,
                                GpuHdTextureImage* image);
void gpu_hd_textures_release_image(GpuHdTextureImage* image);
/* Renderer-thread variant: rgba may be NULL when the GL cache already owns
 * this immutable image. CPU callers use acquire_draw above. */
int gpu_hd_textures_acquire_gl_draw(uint16_t texpage, uint16_t clut_x,
    uint16_t clut_y, const int limits[4], uint32_t texture_window,
    int semitransparent, GpuHdTextureImage* image);

#ifdef __cplusplus
}
#endif
#endif
