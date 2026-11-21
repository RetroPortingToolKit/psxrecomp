/* video_texture_filter.h - the one texture-filter enum: game.toml [video]
 * texture_filtering, the player's Texture filtering setting and the renderer
 * (gr_set_texture_filter). */
#ifndef PSX_VIDEO_TEXTURE_FILTER_H
#define PSX_VIDEO_TEXTURE_FILTER_H
#ifdef __cplusplus
extern "C" {
#endif

/* Values match [video] texture_filtering and gr_set_texture_filter().
 * STABLE_WORLD filters proven 3D polygons on OpenGL; untracked UI stays
 * nearest. Other backends use ordinary bilinear, including the UI. */
#define PSX_TEXTURE_FILTER_NEAREST      0
#define PSX_TEXTURE_FILTER_BILINEAR     1
#define PSX_TEXTURE_FILTER_STABLE_WORLD 2
#define PSX_TEXTURE_FILTER_XBR          3

#ifdef __cplusplus
}
#endif
#endif
