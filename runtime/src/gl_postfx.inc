/* ---- Post-effect pipeline (docs/POST_EFFECTS.md) ---------------------------
 * One executor for every post effect on the composed game image: chains of
 * full-screen fragment passes (postfx_chain.h). Three stages, in order, each
 * a chain that reads a fresh copy of the image and writes it back:
 *   1. bloom      built-in "bloom" preset, [video] bloom (strength uniform)
 *   2. user       [video] post_shader / PSX_POST_SHADER: any .pfx preset
 *   3. AA         built-in "fxaa"/"fxaa_hq" preset, [video] antialiasing_mode
 * All are elective and off by default; with no stage active nothing here
 * runs. Like the former fixed passes this runs in the window rect before
 * hold-last capture and the OSD, on whichever thread presents (the render
 * thread included), and Smooth motion's generated frames run stages 1 and 2.
 *
 * Inputs a pass may read: `source` (the composed image, one shared copy),
 * `mask` (optional: the primitive-derived "may glow" mask, see below), and
 * earlier passes' targets. The executor owns target allocation and lifetime,
 * program build/cache, uniform binding (u_rcp, u_size, u_org and the preset's
 * uniforms) and GL state: everything the pass touches is put back.
 *
 * The mask. While an active chain reads `mask`, every draw keeps a mark in
 * stencil bit 1 of the surface it draws into (BCLS_DRAW in gpu_gl_renderer.c):
 * set by 3D (PGXP-proven) and additive primitives, cleared by opaque 2D ones
 * (sprites, fills, the HUD). The marks travel with the picture (native-wide
 * centre copies, Smooth motion surfaces). At the present they are blitted
 * from the presented surface into a window-sized stencil and turned into an
 * R8 texture (1 = lit). A temporal-blending in-between present reuses the last
 * mask. A chain that does not read `mask` costs none of this. */
/* The GL-free preset parser is compiled into this translation unit (every
 * build and GL fixture that compiles the renderer gets it with no extra
 * source); runtime/tests/test_postfx_chain.c builds it alone. */
#include "postfx_chain.c"

/* Uniform locations, looked up once per program (a lookup per frame is a
 * driver round trip on a threaded GL). */
enum { PFX_LOC_RCP, PFX_LOC_SIZE, PFX_LOC_ORG, PFX_LOC_UV, PFX_LOC_FIXED };
typedef struct {
    GLint fixed[PFX_LOC_FIXED];
    GLint in[PFX_MAX_INPUTS];
    GLint u[PFX_MAX_UNIFORMS];
    GLint time, proj;
} PfxLocs;
typedef struct {
    PfxChain chain;
    int      loaded, failed;
    GLuint   prog[PFX_MAX_PASSES];
    PfxLocs  loc[PFX_MAX_PASSES];
    GLuint   tex[PFX_MAX_PASSES], fbo[PFX_MAX_PASSES];
    GLuint   dtex[PFX_MAX_PASSES], dfbo[PFX_MAX_PASSES];   /* source_down copies */
    int      dw[PFX_MAX_PASSES], dh[PFX_MAX_PASSES];
    int      tw[PFX_MAX_PASSES], th[PFX_MAX_PASSES];
    GLuint   htex[PFX_MAX_PASSES], hfbo[PFX_MAX_PASSES];   /* `history` copies */
    int      hw[PFX_MAX_PASSES], hh[PFX_MAX_PASSES], hist[PFX_MAX_PASSES];
} PfxStage;
enum { PFX_STAGE_BLOOM, PFX_STAGE_USER, PFX_STAGE_AA, PFX_STAGE_HQ, PFX_NSTAGES };
static PfxStage s_pfx[PFX_NSTAGES];
static char   s_pfx_user_path[1024] = "";
static int    s_pfx_user_env = 0;

static float  s_bloom_thr = 0.55f;          /* bloom bright-pass threshold */
static uint64_t s_bloom_passes = 0;
static int    s_bloom_time = 0;             /* debug: glFinish-bracketed cost */
static double s_bloom_us_sum = 0.0; static uint64_t s_bloom_us_n = 0;   /* real frames */
static double s_bloom_gus_sum = 0.0; static uint64_t s_bloom_gus_n = 0; /* generated frames */
static uint64_t s_pfx_reused = 0;           /* passes skipped on generated frames */

/* The mask (see above). */
static GLuint s_mask_src_fbo = 0, s_mask_src_rb = 0;
static int    s_mask_src_tw = 0, s_mask_src_th = 0;
static int    s_mask_src[4];                /* x0,y0,x1,y1 in source FBO px (y0 = top) */
static int    s_mask_src_valid = 0, s_mask_ok = 0;
/* Native-wide: the centre's marks come from the hr surface (its copy into
 * the wide surface is colour only). */
static GLuint s_mask_ctr_fbo = 0, s_mask_ctr_rb = 0;
static int    s_mask_ctr_tw = 0, s_mask_ctr_th = 0;
static int    s_mask_ctr[4], s_mask_ctr_valid = 0;
static float  s_mask_ctr_x0 = 0.0f, s_mask_ctr_x1 = 0.0f;   /* fractions of the displayed width */
static GLuint s_mask_tex = 0, s_mask_fbo = 0, s_mask_prog = 0, s_mask_map_prog = 0;
static GLint  s_mask_uUv = -1, s_mask_uV = -1;
static GLint  s_mmap_uUv = -1, s_mmap_uS = -1, s_mmap_uSrc = -1, s_mmap_uDst = -1;
/* Marks resolved in the surfaces' own px: an R8 texture per surface,
 * attached with that surface's own depth-stencil renderbuffer, so no
 * depth-stencil blit is needed (one costs the CPU milliseconds on macOS). */
typedef struct { GLuint rb, tex, fbo; int w, h; } PfxSurfMask;
static PfxSurfMask s_smask[2];
/* `depth`: each surface's depth-stencil renderbuffer is blitted into a depth
 * texture of its size (dtex/dfbo), then mapped to the window as view depth sz
 * (GTE screen z; PGXP writes zn = 1 - 512/(sz + 256)) in an R32F texture. */
typedef struct { GLuint rb, dtex, dfbo; int w, h; } PfxSurfDepth;
static PfxSurfDepth s_sdepth[2];
static GLuint s_depth_tex = 0, s_depth_fbo = 0, s_depth_map_prog = 0;
static GLint  s_dmap_uUv = -1, s_dmap_uS = -1, s_dmap_uSrc = -1, s_dmap_uDst = -1;
static int    s_depth_w = 0, s_depth_h = 0, s_depth_ok = 0;
static int    s_pfx_depth_on = 0;          /* a loaded chain reads `depth` */
static uint64_t s_pfx_t0 = 0;              /* u_time origin */
/* Live values for preset `uniform` lines, set by the title from game state
 * (psx_host_set_post_uniform, host_overlay.h): a live value replaces the
 * preset default of the same name in every pass that declares it. */
enum { PFX_LIVE_MAX = 16 };
typedef struct { char name[PFX_NAME]; float v[4]; } PfxLive;
static PfxLive s_pfx_live[PFX_LIVE_MAX];
static int s_pfx_nlive = 0;
static SDL_SpinLock s_pfx_live_lock = 0;
void psx_host_set_post_uniform(const char *name, float x, float y, float z, float w) {
    if (!name || !name[0] || strlen(name) >= PFX_NAME) return;
    SDL_LockSpinlock(&s_pfx_live_lock);
    int i = 0;
    while (i < s_pfx_nlive && strcmp(s_pfx_live[i].name, name)) i++;
    if (i == s_pfx_nlive && s_pfx_nlive < PFX_LIVE_MAX) {
        snprintf(s_pfx_live[i].name, PFX_NAME, "%s", name);
        s_pfx_nlive++;
    }
    if (i < s_pfx_nlive) {
        s_pfx_live[i].v[0] = x; s_pfx_live[i].v[1] = y; s_pfx_live[i].v[2] = z; s_pfx_live[i].v[3] = w;
    }
    SDL_UnlockSpinlock(&s_pfx_live_lock);
}
/* Last load outcome of the user chain, for the debug server. */
static char s_pfx_user_status[192] = "";
static int pfx_live_get(const char *name, float out[4]) {
    int found = 0;
    SDL_LockSpinlock(&s_pfx_live_lock);
    for (int i = 0; i < s_pfx_nlive; i++)
        if (!strcmp(s_pfx_live[i].name, name)) { memcpy(out, s_pfx_live[i].v, sizeof s_pfx_live[i].v); found = 1; break; }
    SDL_UnlockSpinlock(&s_pfx_live_lock);
    return found;
}
extern volatile int32_t g_gte_last_h, g_gte_last_ofx, g_gte_last_ofy;   /* gte.cpp */
static int    s_mask_w = 0, s_mask_h = 0;

static int pfx_stage_on(int i) {
    switch (i) {
    case PFX_STAGE_BLOOM: return s_bloom > 0.0f;
    case PFX_STAGE_USER:  return s_pfx_user_path[0] != 0;
    case PFX_STAGE_AA:    return s_post_aa == GL_POST_AA_FXAA;
    case PFX_STAGE_HQ:    return s_post_aa == GL_POST_AA_FXAA_HQ;
    }
    return 0;
}
static int postfx_active(void) {
    for (int i = 0; i < PFX_NSTAGES; i++) if (pfx_stage_on(i)) return 1;
    return 0;
}

static void pfx_stage_unload(PfxStage *st) {
    for (int i = 0; i < PFX_MAX_PASSES; i++) {
        if (s_ctx) {
            if (st->prog[i]) p_glDeleteProgram(st->prog[i]);
            if (st->fbo[i]) p_glDeleteFramebuffers(1, &st->fbo[i]);
            if (st->tex[i]) glDeleteTextures(1, &st->tex[i]);
            if (st->dfbo[i]) p_glDeleteFramebuffers(1, &st->dfbo[i]);
            if (st->dtex[i]) glDeleteTextures(1, &st->dtex[i]);
            if (st->hfbo[i]) p_glDeleteFramebuffers(1, &st->hfbo[i]);
            if (st->htex[i]) glDeleteTextures(1, &st->htex[i]);
        }
        st->htex[i] = st->hfbo[i] = 0; st->hw[i] = st->hh[i] = 0; st->hist[i] = 0;
        st->prog[i] = st->tex[i] = st->fbo[i] = 0; st->tw[i] = st->th[i] = 0;
        st->dtex[i] = st->dfbo[i] = 0; st->dw[i] = st->dh[i] = 0;
    }
    pfx_chain_free(&st->chain);
    st->loaded = st->failed = 0;
}

/* Which stages read the mask decides whether draws keep it. */
static void pfx_mask_wanted_update(void) {
    int want = 0;
    for (int i = 0; i < PFX_NSTAGES; i++) {
        if (!pfx_stage_on(i)) continue;
        if (i == PFX_STAGE_BLOOM) want = 1;          /* the bloom preset reads it */
        else if (s_pfx[i].loaded && s_pfx[i].chain.uses_mask) want = 1;
    }
    s_pfx_depth_on = 0;
    for (int i = 0; i < PFX_NSTAGES; i++)
        if (pfx_stage_on(i) && s_pfx[i].loaded && s_pfx[i].chain.uses_depth) s_pfx_depth_on = 1;
    if (want != s_bcls_on) { s_bcls_on = want; pgxp_render_wanted_update(); }
}

static int pfx_stage_load(int i) {
    PfxStage *st = &s_pfx[i];
    /* failed first: a preset that loaded but has a pass that did not
     * compile stays off until it is set again (no rebuild every present). */
    if (st->failed) return 0;
    if (st->loaded) return 1;
    char err[256] = "";
    int ok;
    if (i == PFX_STAGE_USER) ok = pfx_chain_load_file(s_pfx_user_path, &st->chain, err, sizeof err);
    else ok = pfx_chain_builtin(i == PFX_STAGE_BLOOM ? "bloom" : i == PFX_STAGE_AA ? "fxaa" : "fxaa_hq",
                                &st->chain, err, sizeof err);
    if (!ok) {
        fprintf(stdout, "psxrecomp: post effect '%s' not loaded: %s\n",
                i == PFX_STAGE_USER ? s_pfx_user_path : "built-in", err);
        if (i == PFX_STAGE_USER) snprintf(s_pfx_user_status, sizeof s_pfx_user_status, "not loaded: %s", err);
        st->failed = 1;
        return 0;
    }
    st->loaded = 1;
    for (int p = 0; p < st->chain.npasses; p++)
        for (int k = 0; k < st->chain.pass[p].ninputs; k++)
            if (st->chain.pass[p].in[k].from == PFX_IN_HISTORY) st->hist[p] = 1;
    if (i == PFX_STAGE_USER) {
        fprintf(stdout, "psxrecomp: post shader '%s' (%d passes%s)\n", s_pfx_user_path,
                st->chain.npasses, st->chain.uses_mask ? ", mask" : "");
        snprintf(s_pfx_user_status, sizeof s_pfx_user_status, "parsed: %d passes", st->chain.npasses);
    }
    return 1;
}

static uint64_t s_pfx_builds = 0;           /* program builds (tests) */
static GLuint pfx_build_pass(const PfxPass *ps) {
    s_pfx_builds++;
    size_t n = strlen(ps->shader) + 512 + (size_t)ps->ninputs * 64 + (size_t)ps->nuniforms * 64;
    char *src = (char *)malloc(n);
    if (!src) return 0;
    int k = snprintf(src, n, "#version 330\nin vec2 v_uv; out vec4 o;\n"
                             "uniform vec2 u_rcp; uniform vec2 u_size; uniform ivec2 u_org;\n"
                             "uniform float u_time; uniform vec4 u_proj;\n");
    for (int i = 0; i < ps->ninputs; i++)
        k += snprintf(src + k, n - (size_t)k, "uniform sampler2D %s;\n", ps->in[i].sampler);
    /* The preset's `uniform` lines are declared here unless the body declares
     * them itself (the built-in shaders do). */
    static const char *const kTypes[] = { "float", "vec2", "vec3", "vec4" };
    for (int i = 0; i < ps->nuniforms && (size_t)k + 96 < n; i++) {
        char decl[PFX_NAME + 16];
        snprintf(decl, sizeof decl, "%s %s;", kTypes[ps->u[i].count - 1], ps->u[i].name);
        if (!strstr(ps->shader, decl)) k += snprintf(src + k, n - (size_t)k, "uniform %s\n", decl);
    }
    snprintf(src + k, n - (size_t)k, "%s", ps->shader);
    GLuint prog = build_program(PRESENT_VS, src);
    free(src);
    return prog;
}

/* The depth-stencil renderbuffer of a presentable surface (0: unknown). */
static GLuint pfx_surface_rb(GLuint fbo) {
    if (fbo == s_hr_fbo) return s_hr_rb;
    for (int i = 0; i < s_hiw_n; i++) if (s_hiw_t[i].fbo == fbo) return s_hiw_t[i].rb;
    const int wi = wide_index(fbo);
    return wi >= 0 ? s_wide_rb[wi] : 0;
}
/* A displayed rect of a surface in that surface's px (y0 = top row). */
static void pfx_rect_px(GLuint tex, float tex_w, float tex_h, int x, int y, int w, int h, int out[4],
                        int *out_tw, int *out_th) {
    GLint tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glBindTexture(GL_TEXTURE_2D, 0);
    const float sx = (float)tw / tex_w, sy = (float)th / tex_h;
    *out_tw = tw; *out_th = th;
    out[0] = (int)lrintf((float)x * sx);
    out[1] = (int)lrintf((float)y * sy);
    out[2] = (int)lrintf((float)(x + w) * sx);
    out[3] = (int)lrintf((float)(y + h) * sy);
}
/* Native-wide: the centre (native px x..x+w of the hr surface) shows at
 * native-wide columns off..off+w of the wide_w displayed. */
static void postfx_note_center(GLuint fbo, GLuint tex, float tex_w, float tex_h,
                               int x, int y, int w, int h, int off, int wide_w) {
    s_mask_ctr_valid = 0;
    if ((!s_bcls_on && !s_pfx_depth_on) || !fbo || !tex || wide_w <= 0) return;
    s_mask_ctr_rb = pfx_surface_rb(fbo);
    if (!s_mask_ctr_rb) return;
    pfx_rect_px(tex, tex_w, tex_h, x, y, w, h, s_mask_ctr, &s_mask_ctr_tw, &s_mask_ctr_th);
    s_mask_ctr_fbo = fbo;
    s_mask_ctr_x0 = (float)off / (float)wide_w;
    s_mask_ctr_x1 = (float)(off + w) / (float)wide_w;
    s_mask_ctr_valid = 1;
}

/* The presented surface: its FBO and the displayed rect, in native units of
 * a texture tex_w x tex_h native units across (as present_target_quad). */
static void postfx_note_source(GLuint fbo, GLuint tex, float tex_w, float tex_h,
                               int x, int y, int w, int h) {
    s_mask_src_valid = 0;
    s_mask_ctr_valid = 0;
    if ((!s_bcls_on && !s_pfx_depth_on) || !fbo || !tex || tex_w <= 0.0f || tex_h <= 0.0f) return;
    s_mask_src_rb = pfx_surface_rb(fbo);
    if (!s_mask_src_rb) return;
    pfx_rect_px(tex, tex_w, tex_h, x, y, w, h, s_mask_src, &s_mask_src_tw, &s_mask_src_th);
    s_mask_src_fbo = fbo;
    s_mask_src_valid = 1;
}

static void pfx_tex_alloc(GLuint *t, int w, int h, int fmt, int nearest) {
    if (!*t) glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    if (fmt == PFX_FMT_RGBA16F) glTexImage2D(GL_TEXTURE_2D, 0, 0x881A /*RGBA16F*/, w, h, 0, GL_RGBA, GL_FLOAT, NULL);
    else if (fmt == -1)         glTexImage2D(GL_TEXTURE_2D, 0, 0x8229 /*R8*/, w, h, 0, 0x1903 /*RED*/, GL_UNSIGNED_BYTE, NULL);
    else if (fmt == -2)         glTexImage2D(GL_TEXTURE_2D, 0, 0x822E /*R32F*/, w, h, 0, 0x1903 /*RED*/, GL_FLOAT, NULL);
    else                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
}
static void pfx_fbo_attach(GLuint *f, GLuint tex) {
    if (!*f) p_glGenFramebuffers(1, f);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, *f);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
}

/* Window-sized mask from the presented surface's marks (fresh) or the last. */
/* The marks of rect r (surface px) of surface rb (tw x th px) into slot k's
 * R8 texture: 0 over the rect, then 1 where stencil bit 1 is set. */
static int pfx_surf_mask(int k, GLuint rb, int tw, int th, const int r[4]) {
    PfxSurfMask *m = &s_smask[k];
    if (m->rb != rb || m->w != tw || m->h != th) {
        pfx_tex_alloc(&m->tex, tw, th, -1, 1);
        pfx_fbo_attach(&m->fbo, m->tex);
        p_glFramebufferRenderbuffer(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT,
                                    PSXGL_RENDERBUFFER, rb);
        m->rb = rb; m->w = tw; m->h = th;
    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, m->fbo);
    glViewport(r[0], r[1], r[2] - r[0], r[3] - r[1]);
    p_glUseProgram(s_mask_prog);
    p_glUniform1f(s_mask_uV, 0.0f);
    post_fullscreen(s_mask_prog, s_mask_uUv);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 0x02, 0x02);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    p_glUniform1f(s_mask_uV, 1.0f);
    post_fullscreen(s_mask_prog, s_mask_uUv);
    glDisable(GL_STENCIL_TEST);
    return 1;
}
/* Slot k's rect r into window columns [x0, x1) of the lw x lh mask, the way
 * a NEAREST blit (row 0 = the rect's bottom) would map it. */
static void pfx_mask_map(int k, const int r[4], int x0, int x1, int lh) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_mask_fbo);
    glViewport(x0, 0, x1 - x0, lh);
    p_glUseProgram(s_mask_map_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_smask[k].tex);
    p_glUniform1i(s_mmap_uS, 0);
    p_glUniform4i(s_mmap_uSrc, r[0], r[1], r[2], r[3]);
    p_glUniform4f(s_mmap_uDst, (float)x0, (float)(x1 - x0), (float)lh, 0.0f);
    post_fullscreen(s_mask_map_prog, s_mmap_uUv);
}

/* Window-sized mask from the presented surface's marks (fresh) or the last. */
static int pfx_mask_build(int fresh, int lw, int lh) {
    if (!s_mask_prog) {
        s_mask_prog = build_program(PRESENT_VS,
            "#version 330\nin vec2 v_uv; out vec4 o; uniform float u_v;\nvoid main(){ o = vec4(u_v); }\n");
        s_mask_map_prog = build_program(PRESENT_VS,
            "#version 330\nin vec2 v_uv; out vec4 o; uniform sampler2D u_s;\n"
            "uniform ivec4 u_src; uniform vec4 u_dst;\n"   /* dst: x0, width, height */
            "void main(){\n"
            "  vec2 f = gl_FragCoord.xy;\n"
            "  int sx = u_src.x + int(floor((f.x - u_dst.x) * float(u_src.z - u_src.x) / u_dst.y));\n"
            "  int sy = u_src.y + int(floor((u_dst.z - f.y) * float(u_src.w - u_src.y) / u_dst.z));\n"
            "  o = vec4(texelFetch(u_s, ivec2(sx, sy), 0).r);\n"
            "}\n");
        if (!s_mask_prog || !s_mask_map_prog) { s_mask_prog = 0; return 0; }
        s_mask_uUv = p_glGetUniformLocation(s_mask_prog, "u_uv_rect");
        s_mask_uV = p_glGetUniformLocation(s_mask_prog, "u_v");
        s_mmap_uUv = p_glGetUniformLocation(s_mask_map_prog, "u_uv_rect");
        s_mmap_uS = p_glGetUniformLocation(s_mask_map_prog, "u_s");
        s_mmap_uSrc = p_glGetUniformLocation(s_mask_map_prog, "u_src");
        s_mmap_uDst = p_glGetUniformLocation(s_mask_map_prog, "u_dst");
    }
    if (s_mask_w != lw || s_mask_h != lh) {
        pfx_tex_alloc(&s_mask_tex, lw, lh, -1, 1);
        pfx_fbo_attach(&s_mask_fbo, s_mask_tex);
        s_mask_w = lw; s_mask_h = lh; s_mask_ok = 0;
    }
    if (fresh && s_mask_src_valid) {
        pfx_surf_mask(0, s_mask_src_rb, s_mask_src_tw, s_mask_src_th, s_mask_src);
        if (s_mask_ctr_valid)
            pfx_surf_mask(1, s_mask_ctr_rb, s_mask_ctr_tw, s_mask_ctr_th, s_mask_ctr);
        pfx_mask_map(0, s_mask_src, 0, lw, lh);
        if (s_mask_ctr_valid) {
            const int c0 = (int)lrintf(s_mask_ctr_x0 * (float)lw), c1 = (int)lrintf(s_mask_ctr_x1 * (float)lw);
            if (c1 > c0) pfx_mask_map(1, s_mask_ctr, c0, c1, lh);
        }
        s_mask_ok = 1;
    }
    return s_mask_ok;
}

/* Slot k: surface rb (tw x th) blitted into its depth texture. */
static int pfx_surf_depth(int k, GLuint fbo, GLuint rb, int tw, int th) {
    PfxSurfDepth *d = &s_sdepth[k];
    if (d->rb != rb || d->w != tw || d->h != th) {
        if (!d->dtex) glGenTextures(1, &d->dtex);
        glBindTexture(GL_TEXTURE_2D, d->dtex);
        glTexImage2D(GL_TEXTURE_2D, 0, PSXGL_DEPTH24_STENCIL8, tw, th, 0,
                     0x84F9 /*DEPTH_STENCIL*/, 0x84FA /*UNSIGNED_INT_24_8*/, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!d->dfbo) p_glGenFramebuffers(1, &d->dfbo);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, d->dfbo);
        p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, d->dtex, 0);
        d->rb = rb; d->w = tw; d->h = th;
    }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, d->dfbo);
    p_glBlitFramebuffer(0, 0, tw, th, 0, 0, tw, th, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    return 1;
}
static void pfx_depth_map(int k, const int r[4], int x0, int x1, int lh) {
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_depth_fbo);
    glViewport(x0, 0, x1 - x0, lh);
    p_glUseProgram(s_depth_map_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_sdepth[k].dtex);
    p_glUniform1i(s_dmap_uS, 0);
    p_glUniform4i(s_dmap_uSrc, r[0], r[1], r[2], r[3]);
    p_glUniform4f(s_dmap_uDst, (float)x0, (float)(x1 - x0), (float)lh, 0.0f);
    post_fullscreen(s_depth_map_prog, s_dmap_uUv);
    glBindTexture(GL_TEXTURE_2D, 0);
}
/* Window-sized view depth from the presented surface (fresh) or the last. */
static int pfx_depth_build(int fresh, int lw, int lh) {
    if (!s_depth_map_prog) {
        s_depth_map_prog = build_program(PRESENT_VS,
            "#version 330\nin vec2 v_uv; out vec4 o; uniform sampler2D u_s;\n"
            "uniform ivec4 u_src; uniform vec4 u_dst;\n"
            "void main(){\n"
            "  vec2 f = gl_FragCoord.xy;\n"
            "  int sx = u_src.x + int(floor((f.x - u_dst.x) * float(u_src.z - u_src.x) / u_dst.y));\n"
            "  int sy = u_src.y + int(floor((u_dst.z - f.y) * float(u_src.w - u_src.y) / u_dst.z));\n"
            "  float d = texelFetch(u_s, ivec2(sx, sy), 0).r;\n"
            "  /* zn = 2d - 1 = 1 - 512/(sz + 256); d = 0.5 is the zn = 0 of 2D draws */\n"
            "  float sz = (d >= 0.999999 || abs(d - 0.5) < 1e-6) ? 0.0 : 256.0 / max(1.0 - d, 1e-6) - 256.0;\n"
            "  o = vec4(sz);\n"
            "}\n");
        if (!s_depth_map_prog) return 0;
        s_dmap_uUv = p_glGetUniformLocation(s_depth_map_prog, "u_uv_rect");
        s_dmap_uS = p_glGetUniformLocation(s_depth_map_prog, "u_s");
        s_dmap_uSrc = p_glGetUniformLocation(s_depth_map_prog, "u_src");
        s_dmap_uDst = p_glGetUniformLocation(s_depth_map_prog, "u_dst");
    }
    if (s_depth_w != lw || s_depth_h != lh) {
        pfx_tex_alloc(&s_depth_tex, lw, lh, -2, 1);
        pfx_fbo_attach(&s_depth_fbo, s_depth_tex);
        s_depth_w = lw; s_depth_h = lh; s_depth_ok = 0;
    }
    if (fresh && s_mask_src_valid) {
        pfx_surf_depth(0, s_mask_src_fbo, s_mask_src_rb, s_mask_src_tw, s_mask_src_th);
        if (s_mask_ctr_valid)
            pfx_surf_depth(1, s_mask_ctr_fbo, s_mask_ctr_rb, s_mask_ctr_tw, s_mask_ctr_th);
        pfx_depth_map(0, s_mask_src, 0, lw, lh);
        if (s_mask_ctr_valid) {
            const int c0 = (int)lrintf(s_mask_ctr_x0 * (float)lw), c1 = (int)lrintf(s_mask_ctr_x1 * (float)lw);
            if (c1 > c0) pfx_depth_map(1, s_mask_ctr, c0, c1, lh);
        }
        s_depth_ok = 1;
    }
    return s_depth_ok;
}

/* Run one stage's chain over the window rect. 0 when it could not run.
 * generated: a Smooth motion in-between frame; passes marked `generated
 * reuse` keep the target the last real frame left (when it is there at this
 * size) instead of running. The full-size copy of the image is made only
 * if a pass that runs reads `source`; `source_down` is a blit of the window
 * rect straight to that pass's size. */
static int pfx_stage_run(PfxStage *st, int generated, int lx, int ly, int lw, int lh) {
    PfxChain *c = &st->chain;
    int copied = 0;
    for (int i = 0; i < c->npasses; i++) {
        const PfxPass *ps = &c->pass[i];
        const int last = i == c->npasses - 1;
        if (!st->prog[i]) {
            st->prog[i] = pfx_build_pass(ps);
            if (st->prog[i]) {
                PfxLocs *L = &st->loc[i];
                const GLuint pg = st->prog[i];
                L->fixed[PFX_LOC_RCP] = p_glGetUniformLocation(pg, "u_rcp");
                L->fixed[PFX_LOC_SIZE] = p_glGetUniformLocation(pg, "u_size");
                L->fixed[PFX_LOC_ORG] = p_glGetUniformLocation(pg, "u_org");
                L->fixed[PFX_LOC_UV] = p_glGetUniformLocation(pg, "u_uv_rect");
                L->time = p_glGetUniformLocation(pg, "u_time");
                L->proj = p_glGetUniformLocation(pg, "u_proj");
                for (int k = 0; k < ps->ninputs; k++) L->in[k] = p_glGetUniformLocation(pg, ps->in[k].sampler);
                for (int k = 0; k < ps->nuniforms; k++) L->u[k] = p_glGetUniformLocation(pg, ps->u[k].name);
            }
            if (!st->prog[i]) {
                fprintf(stdout, "psxrecomp: post effect '%s' pass '%s' did not compile; effect off\n",
                        c->name, ps->name);
                if (st == &s_pfx[PFX_STAGE_USER])
                    snprintf(s_pfx_user_status, sizeof s_pfx_user_status, "pass '%s' did not compile", ps->name);
                st->failed = 1;
                return 0;
            }
        }
        int w = lw, h = lh;
        if (!last) {
            w = (int)((float)lw * ps->scale); h = (int)((float)lh * ps->scale);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (generated && ps->reuse_generated && st->fbo[i] && st->tw[i] == w && st->th[i] == h) {
                s_pfx_reused++;
                continue;
            }
            if (st->tw[i] != w || st->th[i] != h) {
                pfx_tex_alloc(&st->tex[i], w, h, ps->format, ps->nearest);
                pfx_fbo_attach(&st->fbo[i], st->tex[i]);
                st->tw[i] = w; st->th[i] = h;
            }
        }
        /* The image this pass reads, before its target is bound. */
        for (int k = 0; k < ps->ninputs; k++) {
            if (ps->in[k].from == PFX_IN_SOURCE && !copied) {
                post_copy_bind(lw, lh);
                post_copy_window(lx, ly, lw, lh);
                copied = 1;
            } else if (ps->in[k].from == PFX_IN_SOURCE_DOWN) {
                if (st->dw[i] != w || st->dh[i] != h) {
                    pfx_tex_alloc(&st->dtex[i], w, h, PFX_FMT_RGBA8, 0);
                    pfx_fbo_attach(&st->dfbo[i], st->dtex[i]);
                    st->dw[i] = w; st->dh[i] = h;
                }
                p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
                p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, st->dfbo[i]);
                p_glBlitFramebuffer(lx, ly, lx + lw, ly + lh, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            }
        }
        if (last) { p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0); glViewport(lx, ly, lw, lh); }
        else { p_glBindFramebuffer(PSXGL_FRAMEBUFFER, st->fbo[i]); glViewport(0, 0, w, h); }
        const GLuint prog = st->prog[i];
        const PfxLocs *L = &st->loc[i];
        p_glUseProgram(prog);
        for (int k = 0; k < ps->ninputs; k++) {
            const int from = ps->in[k].from;
            const GLuint t = from == PFX_IN_SOURCE ? s_post_copy_tex
                           : from == PFX_IN_SOURCE_DOWN ? st->dtex[i]
                           : from == PFX_IN_MASK ? s_mask_tex
                           : from == PFX_IN_DEPTH ? s_depth_tex
                           : from == PFX_IN_HISTORY ? st->htex[i] : st->tex[from];
            p_glActiveTexture(PSXGL_TEXTURE0 + (GLenum)k);
            glBindTexture(GL_TEXTURE_2D, t);
            p_glUniform1i(L->in[k], k);
        }
        p_glActiveTexture(PSXGL_TEXTURE0);
        p_glUniform2f(L->fixed[PFX_LOC_RCP], 1.0f / (float)w, 1.0f / (float)h);
        p_glUniform2f(L->fixed[PFX_LOC_SIZE], (float)w, (float)h);
        p_glUniform2i(L->fixed[PFX_LOC_ORG], last ? lx : 0, last ? ly : 0);
        if (!s_pfx_t0) s_pfx_t0 = SDL_GetPerformanceCounter();
        if (L->proj >= 0) {
            /* u_proj: x/z = (uv.x - y) * w * x, y/z = (uv.y - z) * x for a view-space
             * point at depth sz (240-line reference, same scale on both axes). */
            const float aspect = (float)lw / (float)(lh > 0 ? lh : 1);
            const float H = g_gte_last_h > 0 ? (float)g_gte_last_h : 256.0f;
            const float ofx = (float)g_gte_last_ofx / 65536.0f, ofy = (float)g_gte_last_ofy / 65536.0f;
            p_glUniform4f(L->proj, 240.0f / H, 0.5f + (ofx - 160.0f) / (240.0f * aspect),
                          1.0f - (ofy > 0.0f ? ofy : 120.0f) / 240.0f, aspect);
        }
        if (L->time >= 0)
            p_glUniform1f(L->time, (float)((double)(SDL_GetPerformanceCounter() - s_pfx_t0) /
                                           (double)SDL_GetPerformanceFrequency()));
        for (int k = 0; k < ps->nuniforms; k++) {
            const PfxUniform *u = &ps->u[k];
            const GLint loc = L->u[k];
            if (loc < 0) continue;
            float live[4];
            const float *v = pfx_live_get(u->name, live) ? live : u->v;
            switch (u->count) {
            case 1: p_glUniform1f(loc, v[0]); break;
            case 2: p_glUniform2f(loc, v[0], v[1]); break;
            case 3: p_glUniform3f(loc, v[0], v[1], v[2]); break;
            default: p_glUniform4f(loc, v[0], v[1], v[2], v[3]); break;
            }
        }
        if (ps->blend == PFX_BLEND_SCREEN) {
            glEnable(GL_BLEND);
            p_glBlendEquationSeparate(PSXGL_FUNC_ADD, PSXGL_FUNC_ADD);
            p_glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR, GL_ZERO, GL_ONE);
        }
        post_fullscreen(prog, L->fixed[PFX_LOC_UV]);
        if (ps->blend != PFX_BLEND_REPLACE) glDisable(GL_BLEND);
        if (!last && !generated && st->hist[i]) {
            /* This pass's target becomes its `history` for the next real frame. */
            if (st->hw[i] != w || st->hh[i] != h) {
                pfx_tex_alloc(&st->htex[i], w, h, ps->format, ps->nearest);
                pfx_fbo_attach(&st->hfbo[i], st->htex[i]);
                st->hw[i] = w; st->hh[i] = h;
            }
            p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, st->fbo[i]);
            p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, st->hfbo[i]);
            p_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
    }
    for (int k = PFX_MAX_INPUTS - 1; k >= 0; k--) {
        p_glActiveTexture(PSXGL_TEXTURE0 + (GLenum)k);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    p_glBindVertexArray(0);
    p_glUseProgram(0);
    return 1;
}

/* The GL state the executor changes, put back afterwards so whatever runs
 * next (the OSD, the next frame's draws) sees what it left. It queries
 * little (capability flags and the viewport; every query can be a driver
 * round trip on a threaded GL, per present, generated frames included) and
 * never touches the clear colour. It runs inside a present, which draws to
 * the window, so both framebuffer bindings go back to the window (0) and
 * texture unit 0 is left active. */
typedef struct {
    GLboolean blend, depth, scissor, stencil;
    GLint viewport[4];
} PfxGlState;
static void pfx_state_save(PfxGlState *st) {
    st->blend = glIsEnabled(GL_BLEND); st->depth = glIsEnabled(GL_DEPTH_TEST);
    st->scissor = glIsEnabled(GL_SCISSOR_TEST); st->stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_VIEWPORT, st->viewport);
}
static void pfx_state_restore(const PfxGlState *st) {
    if (st->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (st->depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (st->stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glViewport(st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3]);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    p_glActiveTexture(PSXGL_TEXTURE0);
}

/* fresh: the noted source is this present's (0: reuse the last mask).
 * with_aa: a real frame (runs AA too); 0: a Smooth motion generated frame. */
static void postfx_present(int fresh, int with_aa, int lx, int ly, int lw, int lh) {
    const int src_fresh = fresh;
    if (!s_ctx) { s_mask_src_valid = 0; return; }
    if (!post_clip_rect(&lx, &ly, &lw, &lh, 8)) { s_mask_src_valid = 0; return; }
    PfxGlState saved;
    pfx_state_save(&saved);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
    int mask_done = 0, mask_ok = 0, depth_done = 0, depth_ok = 0;
    for (int i = 0; i < PFX_NSTAGES; i++) {
        if (!pfx_stage_on(i)) continue;
        if ((i == PFX_STAGE_AA || i == PFX_STAGE_HQ) && !with_aa) continue;
        if (!pfx_stage_load(i)) continue;
        PfxStage *st = &s_pfx[i];
        if (st->chain.uses_mask) {
            if (!mask_done) { mask_ok = pfx_mask_build(src_fresh, lw, lh); mask_done = 1; }
            if (!mask_ok) continue;                  /* no marks yet */
        }
        if (st->chain.uses_depth) {
            if (!depth_done) { depth_ok = pfx_depth_build(src_fresh, lw, lh); depth_done = 1; }
            if (!depth_ok) continue;                 /* no depth yet */
        }
        if (i == PFX_STAGE_BLOOM) {
            const float k = s_bloom;
            pfx_chain_set_uniform(&st->chain, "u_strength", 1, &k);
            pfx_chain_set_uniform(&st->chain, "u_thr", 1, &s_bloom_thr);
        }
        int reps = 1; double *sum = NULL; uint64_t *cnt = NULL;
#ifndef PSX_NO_DEBUG_TOOLS
        if (i == PFX_STAGE_AA || i == PFX_STAGE_HQ) {
            if (s_paa_time < 0) {
                const char *e = getenv("PSX_POST_AA_TIME");
                s_paa_time = e ? atoi(e) : 0;
                if (s_paa_time < 0) s_paa_time = 0;
                if (s_paa_time > 64) s_paa_time = 64;
            }
            if (s_paa_time) { reps = s_paa_time > 1 ? s_paa_time : 1; sum = &s_paa_gpu_us_sum; cnt = &s_paa_gpu_n; }
        } else if (i == PFX_STAGE_BLOOM && s_bloom_time) {
            sum = with_aa ? &s_bloom_us_sum : &s_bloom_gus_sum;
            cnt = with_aa ? &s_bloom_us_n : &s_bloom_gus_n;
        }
        uint64_t t0 = 0;
        if (sum) { glFinish(); t0 = SDL_GetPerformanceCounter(); }
#endif
        int ran = 0;
        for (int r = 0; r < reps; r++) ran = pfx_stage_run(st, !with_aa, lx, ly, lw, lh);
#ifndef PSX_NO_DEBUG_TOOLS
        if (sum && ran) {
            glFinish();
            *sum += (double)(SDL_GetPerformanceCounter() - t0) * 1e6 /
                    (double)SDL_GetPerformanceFrequency() / (double)reps;
            (*cnt)++;
        }
#endif
        if (!ran) continue;
        if (i == PFX_STAGE_BLOOM) s_bloom_passes++;
        if (i == PFX_STAGE_AA || i == PFX_STAGE_HQ) s_paa_passes++;
    }
    s_mask_src_valid = 0; s_mask_ctr_valid = 0;
    pfx_state_restore(&saved);
}

/* Objects died with the context. */
static void postfx_release(void) {
    for (int i = 0; i < PFX_NSTAGES; i++) {
        for (int k = 0; k < PFX_MAX_PASSES; k++) {
            s_pfx[i].prog[k] = s_pfx[i].tex[k] = s_pfx[i].fbo[k] = 0;
            s_pfx[i].tw[k] = s_pfx[i].th[k] = 0;
            s_pfx[i].dtex[k] = s_pfx[i].dfbo[k] = 0; s_pfx[i].dw[k] = s_pfx[i].dh[k] = 0;
        }
        s_pfx[i].failed = 0;
    }
    s_mask_tex = s_mask_fbo = s_mask_prog = s_mask_map_prog = 0;
    memset(s_smask, 0, sizeof s_smask);
    s_depth_tex = s_depth_fbo = s_depth_map_prog = 0; s_depth_w = s_depth_h = 0; s_depth_ok = 0;
    memset(s_sdepth, 0, sizeof s_sdepth);
    for (int i = 0; i < PFX_NSTAGES; i++)
        for (int k = 0; k < PFX_MAX_PASSES; k++) {
            s_pfx[i].htex[k] = s_pfx[i].hfbo[k] = 0; s_pfx[i].hw[k] = s_pfx[i].hh[k] = 0;
        }
    s_mask_w = s_mask_h = 0; s_mask_ok = 0; s_mask_src_valid = 0;
}

/* [video] post_shader (PSX_POST_SHADER overrides): a .pfx chain, "" = none. */
void gl_renderer_set_post_shader(const char *path) {
    GL_RT_SYNC("post_shader");
    if (!s_pfx_user_env) {
        const char *e = getenv("PSX_POST_SHADER");
        if (e) { snprintf(s_pfx_user_path, sizeof s_pfx_user_path, "%s", e); s_pfx_user_env = 1; }
    }
    if (!s_pfx_user_env) {
        /* Setting it again (same path or not) reloads: the file may have
         * changed, and a failed preset gets one new try. */
        pfx_stage_unload(&s_pfx[PFX_STAGE_USER]);
        snprintf(s_pfx_user_path, sizeof s_pfx_user_path, "%s", path ? path : "");
    }
    if (s_pfx_user_path[0] && s_ctx) (void)pfx_stage_load(PFX_STAGE_USER);
    else if (s_pfx_user_path[0]) {
        /* No context yet: the parse needs none, so the mask decision is made now. */
        (void)pfx_stage_load(PFX_STAGE_USER);
    }
    pfx_mask_wanted_update();
}
/* Debug: the user chain's last load outcome and the live uniforms, as JSON members. */
int gl_renderer_post_shader_debug(char *out, int cap) {
    char chemin[sizeof s_pfx_user_path];
    snprintf(chemin, sizeof chemin, "%s", s_pfx_user_path);
    for (char *q = chemin; *q; q++) if (*q == 92 || *q == 34) *q = '/';   /* JSON-safe */
    int k = snprintf(out, (size_t)cap, "\"path\":\"%s\",\"status\":\"%s\",\"uniforms\":{",
                     chemin, s_pfx_user_status);
    SDL_LockSpinlock(&s_pfx_live_lock);
    for (int i = 0; i < s_pfx_nlive && k < cap - 96; i++)
        k += snprintf(out + k, (size_t)(cap - k), "%s\"%s\":[%.4f,%.4f,%.4f,%.4f]", i ? "," : "",
                      s_pfx_live[i].name, s_pfx_live[i].v[0], s_pfx_live[i].v[1], s_pfx_live[i].v[2],
                      s_pfx_live[i].v[3]);
    SDL_UnlockSpinlock(&s_pfx_live_lock);
    if (k < cap - 2) k += snprintf(out + k, (size_t)(cap - k), "}");
    return k;
}

int gl_renderer_post_shader_loaded(void) {
    GL_RT_SYNC("post_shader");
    return s_pfx[PFX_STAGE_USER].loaded && !s_pfx[PFX_STAGE_USER].failed;
}

/* PSX_BLOOM=<0..2> overrides [video] bloom. */
void gl_renderer_set_bloom(float strength) {
    GL_RT_SYNC("set_bloom");
    const char *e = getenv("PSX_BLOOM");
    if (e && *e) strength = (float)atof(e);
    if (!(strength >= 0.0f)) strength = 0.0f;
    if (strength > 2.0f) strength = 2.0f;
    s_bloom = strength;
    pfx_mask_wanted_update();
}
float gl_renderer_get_bloom(void) { return s_bloom; }

void gl_renderer_bloom_debug(float strength, float threshold, int time,
                             GlBloomInfo *out) {
    GL_RT_SYNC("bloom_debug");
    if (strength >= 0.0f && strength <= 2.0f) { s_bloom = strength; pfx_mask_wanted_update(); }
    if (threshold > 0.0f) s_bloom_thr = threshold;
    if (time >= 0) s_bloom_time = time ? 1 : 0;
    if (out) {
        out->strength = s_bloom; out->threshold = s_bloom_thr;
        out->passes = s_bloom_passes;
        out->us_mean = s_bloom_us_n ? s_bloom_us_sum / (double)s_bloom_us_n : 0.0;
        out->us_n = s_bloom_us_n;
        out->gen_us_mean = s_bloom_gus_n ? s_bloom_gus_sum / (double)s_bloom_gus_n : 0.0;
        out->gen_us_n = s_bloom_gus_n;
        out->reused = s_pfx_reused;
        s_bloom_us_sum = 0.0; s_bloom_us_n = 0; s_bloom_gus_sum = 0.0; s_bloom_gus_n = 0;
    }
}
