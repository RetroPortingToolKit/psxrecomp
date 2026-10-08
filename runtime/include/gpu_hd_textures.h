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
} GpuHdTextureDiag;
void gpu_hd_textures_get_diag(GpuHdTextureDiag* out);
void gpu_hd_textures_note_applied(void);
/* Non-GL backends can observe native draws for dumping; replacements remain
 * an OpenGL presentation feature. */
void gpu_hd_textures_observe_draw(uint16_t texpage, uint16_t clut_x,
                                 uint16_t clut_y, const int limits[4],
                                 uint32_t texture_window, int semitransparent);

/* Native VRAM and residency hooks. A full restage resets identity rather than
 * pretending a savestate is an original texture upload. */
void gpu_hd_textures_set_vram(const uint16_t* vram);
void gpu_hd_textures_track_upload(int x, int y, int width_words, int height,
                                 const uint16_t* words);
/* Header-before-payload invalidation and pre/post native copy observations. */
void gpu_hd_textures_begin_upload(int x, int y, int width_words, int height);
void gpu_hd_textures_begin_copy(int source_x, int source_y, int destination_x,
                                int destination_y, int width_words, int height);
void gpu_hd_textures_end_copy(void);
void gpu_hd_textures_invalidate(int x, int y, int width_words, int height);
void gpu_hd_textures_reset_tracking(void);

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

#ifdef __cplusplus
}
#endif
#endif
