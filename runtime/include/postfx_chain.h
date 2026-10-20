/* postfx_chain.h — post-processing pass chains (docs/POST_EFFECTS.md).
 *
 * A chain is a list of full-screen fragment passes run on the composed game
 * image by the OpenGL executor (gl_postfx.inc). This header is the GL-free
 * part: the preset format and its parser, so it can be unit tested alone.
 *
 * Preset (.pfx) format, one directive per line, '#' comments:
 *
 *   pfx 1                              # format version (required first)
 *   pass <name>                        # starts a pass; the last pass writes the window
 *     shader <file.glsl|builtin:name>  # fragment body (see below)
 *     input <sampler> <source|source_down|mask|depth|history|previous|<pass name>>
 *                                      # depth: PGXP view depth sz (GTE screen z, R32F;
 *                                      # 0 where nothing 3D drew); history: this pass's
 *                                      # own target from the last real frame (not last pass)
 *                                      # source_down: the image scaled to this pass's
 *                                      # target (one bilinear blit, no full-size copy)
 *     scale <f>                        # target size, fraction of the game rect (default 1)
 *     format rgba8|rgba16f             # target format (default rgba8)
 *     filter linear|nearest            # how later passes sample this target (default linear)
 *     uniform <name> <f> [f [f [f]]]   # float, vec2, vec3 or vec4
 *     blend replace|screen             # how the window pass lands (default replace):
 *                                      # screen = o + dst * (1 - o); discard leaves dst
 *     generated reuse|run              # Smooth motion's generated frames: reuse this
 *                                      # pass's target from the last real frame (not
 *                                      # for the last pass), or run it (default)
 *
 * A shader body is GLSL 330 appended to a header that declares
 *   in vec2 v_uv; out vec4 o;
 *   uniform vec2 u_rcp;   // 1 / this pass's target size in px
 *   uniform vec2 u_size;  // this pass's target size in px
 *   uniform ivec2 u_org;  // window origin of the game rect (gl_FragCoord - u_org = px in it)
 *   uniform float u_time; // seconds since the chain loaded (real and generated frames)
 * plus one `uniform sampler2D <sampler>;` per input. v_uv is 0..1 over the
 * game rect, bottom-up like the window. */
#ifndef PSX_POSTFX_CHAIN_H
#define PSX_POSTFX_CHAIN_H

#ifdef __cplusplus
extern "C" {
#endif

enum { PFX_MAX_PASSES = 16, PFX_MAX_INPUTS = 4, PFX_MAX_UNIFORMS = 8, PFX_NAME = 32 };
/* Input sources; a value >= 0 is an earlier pass's index. */
enum { PFX_IN_SOURCE = -1, PFX_IN_MASK = -2, PFX_IN_SOURCE_DOWN = -3, PFX_IN_DEPTH = -4,
       PFX_IN_HISTORY = -5 };
enum { PFX_BLEND_REPLACE = 0, PFX_BLEND_SCREEN = 1 };
enum { PFX_FMT_RGBA8 = 0, PFX_FMT_RGBA16F = 1 };

typedef struct {
    char  sampler[PFX_NAME];
    int   from;                      /* PFX_IN_* or an earlier pass index */
} PfxInput;

typedef struct {
    char  name[PFX_NAME];
    int   count;                     /* 1..4 */
    float v[4];
} PfxUniform;

typedef struct {
    char       name[PFX_NAME];
    char      *shader;               /* fragment body (owned) */
    float      scale;
    int        format, nearest;
    int        reuse_generated;      /* generated frames reuse the last real frame's target */
    int        blend;                /* PFX_BLEND_*: the window pass only */
    int        ninputs, nuniforms;
    PfxInput   in[PFX_MAX_INPUTS];
    PfxUniform u[PFX_MAX_UNIFORMS];
} PfxPass;

typedef struct {
    char    name[PFX_NAME];
    int     npasses;
    int     uses_mask;               /* some pass reads `mask` */
    int     uses_depth;              /* some pass reads `depth` */
    PfxPass pass[PFX_MAX_PASSES];
} PfxChain;

/* Resolves `shader` arguments: builtin:<name> and file paths (relative to the
 * preset). Returns a malloc'd body or NULL (err filled). */
typedef char *(*PfxShaderLoader)(const char *arg, void *user, char *err, int errlen);

/* Parse preset text into *out (zeroed first). Returns 1, or 0 with a
 * "line N: ..." message in err. */
int  pfx_chain_parse(const char *text, const char *name, PfxShaderLoader load, void *user,
                     PfxChain *out, char *err, int errlen);
/* Load a preset file; shader files resolve relative to its directory. */
int  pfx_chain_load_file(const char *path, PfxChain *out, char *err, int errlen);
/* A built-in preset ("bloom", "fxaa", "fxaa_hq"); 0 if unknown. */
int  pfx_chain_builtin(const char *name, PfxChain *out, char *err, int errlen);
/* A built-in shader body by name, or NULL. */
const char *pfx_builtin_shader(const char *name);
/* Set every pass's uniform `name` (count values); returns how many were set. */
int  pfx_chain_set_uniform(PfxChain *c, const char *name, int count, const float *v);
void pfx_chain_free(PfxChain *c);

#ifdef __cplusplus
}
#endif
#endif
