/* ---- Bloom ([video] bloom) -------------------------------------------------
 * Opt-in, off by default: with strength 0 nothing here runs, no draw is marked
 * and the present path is the stock one.
 *
 * Sources. Only pixels a 3D primitive (PGXP-proven: needs PGXP geometry) or an
 * additive semi-transparent primitive (modes 1 and 3) drew last can feed the
 * bloom. Every draw marks that in stencil bit 1 of the surface it draws into
 * (BCLS_DRAW in gpu_gl_renderer.c), so a 2D sprite, a fill or the HUD drawn
 * over a lamp clears it again: the HUD never glows. The mark travels with the
 * picture: native-wide centre copies and frame-generation surfaces copy it.
 *
 * Pass. Like post-process AA this works on the composed game image in the
 * window (lx,ly,lw,lh), before AA and the OSD, so it composes with 4:3,
 * native-wide, supersample and dynamic resolution. The presented surface's
 * marks are blitted (NEAREST, flipped to window orientation) into a
 * quarter-resolution stencil; a soft-knee bright pass of the window image runs
 * under that stencil, three separable Gaussian iterations spread it, and the
 * composite screen-blends it back. A temporal-blending in-between present
 * reuses the last marks. Guest VRAM and the surfaces' colour are never
 * written. */
static float  s_bloom_thr = 0.55f;          /* bright-pass threshold (max channel) */
static int    s_bloom_view = 0;             /* debug: 1 bloom only, 2 split, 3 sources */
static int    s_bloom_failed = 0;
static GLuint s_bloom_src_fbo = 0;
static int    s_bloom_src[4];               /* x0,y0,x1,y1 in source FBO px (y0 = top) */
static int    s_bloom_src_valid = 0, s_bloom_mask_ok = 0;
static GLuint s_bloom_copy_tex = 0, s_bloom_rb = 0;
static GLuint s_bloom_tex[2] = {0, 0}, s_bloom_fbo[2] = {0, 0};
static int    s_bloom_w = 0, s_bloom_h = 0;
static GLuint s_bloom_bright_prog = 0, s_bloom_blur_prog = 0, s_bloom_comp_prog = 0;
static uint64_t s_bloom_passes = 0;
static int    s_bloom_time = 0;             /* debug: glFinish-bracketed cost */
static double s_bloom_us_sum = 0.0; static uint64_t s_bloom_us_n = 0;

static const char *BLOOM_COMMON =
    "#version 330\n"
    "in vec2 v_uv; out vec4 o;\n";

static const char *BLOOM_BRIGHT_FS_BODY =
    "uniform sampler2D u_tex; uniform vec2 u_rcp; uniform float u_thr;\n"
    "vec3 S(vec2 p){ return textureLod(u_tex, p, 0.0).rgb; }\n"
    "void main(){\n"
    /* 4 bilinear taps = a 16-texel box of the full-resolution image */
    "  vec3 c = 0.25*(S(v_uv+u_rcp*vec2(-1,-1))+S(v_uv+u_rcp*vec2(1,-1))+S(v_uv+u_rcp*vec2(-1,1))+S(v_uv+u_rcp*vec2(1,1)));\n"
    "  float l = max(c.r, max(c.g, c.b));\n"
    "  float k = 0.15; float s = clamp(l - u_thr + k, 0.0, 2.0*k); s = s*s/(4.0*k+1e-5);\n"
    "  float w = max(s, l - u_thr) / max(l, 1e-4);\n"
    "  o = vec4(c * w, 1.0);\n"
    "}\n";

static const char *BLOOM_BLUR_FS_BODY =
    "uniform sampler2D u_tex; uniform vec2 u_dir;\n"
    "void main(){\n"
    "  vec3 c = textureLod(u_tex, v_uv, 0.0).rgb * 0.2270270;\n"
    "  c += (textureLod(u_tex, v_uv + u_dir*1.3846154, 0.0).rgb + textureLod(u_tex, v_uv - u_dir*1.3846154, 0.0).rgb) * 0.3162162;\n"
    "  c += (textureLod(u_tex, v_uv + u_dir*3.2307692, 0.0).rgb + textureLod(u_tex, v_uv - u_dir*3.2307692, 0.0).rgb) * 0.0702703;\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";

static const char *BLOOM_COMP_FS_BODY =
    "uniform sampler2D u_tex, u_bloom; uniform float u_k; uniform int u_view;\n"
    "void main(){\n"
    "  vec3 c = textureLod(u_tex, v_uv, 0.0).rgb;\n"
    "  vec3 b = clamp(textureLod(u_bloom, v_uv, 0.0).rgb * u_k, 0.0, 1.0);\n"
    "  vec3 r = 1.0 - (1.0 - c) * (1.0 - b);\n"
    "  if (u_view == 1 || u_view == 3) r = b;\n"
    /* 2: split view, stock left / bloom right */
    "  if (u_view == 2) { if (v_uv.x < 0.5) r = c; if (abs(v_uv.x - 0.5) < 0.0012) r = vec3(1.0); }\n"
    "  o = vec4(r, 1.0);\n"
    "}\n";

static GLuint bloom_build(const char *body) {
    static char buf[4096];
    snprintf(buf, sizeof(buf), "%s%s", BLOOM_COMMON, body);
    return build_program(PRESENT_VS, buf);
}

static void bloom_tex(GLuint *t, int w, int h, GLenum ifmt, GLenum type) {
    if (!*t) glGenTextures(1, t);
    glBindTexture(GL_TEXTURE_2D, *t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, GL_RGBA, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

/* The present path's source: its FBO and the displayed rect, in native units
 * of a texture tex_w x tex_h native units across (as present_target_quad). */
static void bloom_note_source(GLuint fbo, GLuint tex, float tex_w, float tex_h,
                              int x, int y, int w, int h) {
    s_bloom_src_valid = 0;
    if (!(s_bloom > 0.0f) || !fbo || !tex || tex_w <= 0.0f || tex_h <= 0.0f) return;
    GLint tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    glBindTexture(GL_TEXTURE_2D, 0);
    const float sx = (float)tw / tex_w, sy = (float)th / tex_h;
    s_bloom_src_fbo = fbo;
    s_bloom_src[0] = (int)lrintf((float)x * sx);
    s_bloom_src[1] = (int)lrintf((float)y * sy);
    s_bloom_src[2] = (int)lrintf((float)(x + w) * sx);
    s_bloom_src[3] = (int)lrintf((float)(y + h) * sy);
    s_bloom_src_valid = 1;
}

static void bloom_draw(GLuint prog) {
    p_glUseProgram(prog);
    p_glUniform4f(p_glGetUniformLocation(prog, "u_uv_rect"), 0.0f, 1.0f, 1.0f, 0.0f);
    p_glBindVertexArray(s_present_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* The GL state the pass changes, put back afterwards so whatever runs next
 * (post-AA, the OSD, the next frame's draws) sees what it left. */
typedef struct {
    GLboolean blend, depth, scissor, stencil;
    GLfloat clear[4];
    GLint viewport[4], fbo, rfbo;
} BloomGlState;
static void bloom_state_save(BloomGlState *st) {
    st->blend = glIsEnabled(GL_BLEND); st->depth = glIsEnabled(GL_DEPTH_TEST);
    st->scissor = glIsEnabled(GL_SCISSOR_TEST); st->stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, st->clear);
    glGetIntegerv(GL_VIEWPORT, st->viewport);
    glGetIntegerv(0x8CA6 /*DRAW_FRAMEBUFFER_BINDING*/, &st->fbo);
    glGetIntegerv(0x8CAA /*READ_FRAMEBUFFER_BINDING*/, &st->rfbo);
}
static void bloom_state_restore(const BloomGlState *st) {
    if (st->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (st->depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (st->scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (st->stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    glClearColor(st->clear[0], st->clear[1], st->clear[2], st->clear[3]);
    glViewport(st->viewport[0], st->viewport[1], st->viewport[2], st->viewport[3]);
    p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, (GLuint)st->fbo);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, (GLuint)st->rfbo);
}

static void bloom_apply_pass(int src_ok, int lx, int ly, int lw, int lh);
/* fresh: the noted source is this present's (0: reuse the last marks). */
static void bloom_apply(int fresh, int lx, int ly, int lw, int lh) {
    const int src_ok = fresh && s_bloom_src_valid;
    s_bloom_src_valid = 0;
    if (!(s_bloom > 0.0f) || !s_ctx || s_bloom_failed) return;
    BloomGlState st;
    bloom_state_save(&st);
    bloom_apply_pass(src_ok, lx, ly, lw, lh);
    bloom_state_restore(&st);
}
static void bloom_apply_pass(int src_ok, int lx, int ly, int lw, int lh) {
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    if (lx < 0) { lw += lx; lx = 0; }
    if (ly < 0) { lh += ly; ly = 0; }
    if (lx + lw > ww) lw = ww - lx;
    if (ly + lh > wh) lh = wh - ly;
    if (lw < 8 || lh < 8) return;
    if (!s_bloom_comp_prog) {
        s_bloom_bright_prog = bloom_build(BLOOM_BRIGHT_FS_BODY);
        s_bloom_blur_prog = bloom_build(BLOOM_BLUR_FS_BODY);
        s_bloom_comp_prog = bloom_build(BLOOM_COMP_FS_BODY);
        if (!s_bloom_bright_prog || !s_bloom_blur_prog || !s_bloom_comp_prog) {
            fprintf(stdout, "psxrecomp: bloom shader build failed; bloom off\n");
            s_bloom_failed = 1; return;
        }
    }
#ifndef PSX_NO_DEBUG_TOOLS
    uint64_t t0 = 0;
    if (s_bloom_time) { glFinish(); t0 = SDL_GetPerformanceCounter(); }
#endif
    const int bw = lw / 4 > 0 ? lw / 4 : 1, bh = lh / 4 > 0 ? lh / 4 : 1;
    p_glActiveTexture(PSXGL_TEXTURE0);
    if (s_bloom_w != lw || s_bloom_h != lh) {
        bloom_tex(&s_bloom_copy_tex, lw, lh, GL_RGBA8, GL_UNSIGNED_BYTE);
        for (int i = 0; i < 2; i++) {
            bloom_tex(&s_bloom_tex[i], bw, bh, 0x881A /*RGBA16F*/, GL_FLOAT);
            if (!s_bloom_fbo[i]) p_glGenFramebuffers(1, &s_bloom_fbo[i]);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_bloom_fbo[i]);
            p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D, s_bloom_tex[i], 0);
        }
        if (!s_bloom_rb) p_glGenRenderbuffers(1, &s_bloom_rb);
        p_glBindRenderbuffer(PSXGL_RENDERBUFFER, s_bloom_rb);
        p_glRenderbufferStorage(PSXGL_RENDERBUFFER, PSXGL_DEPTH24_STENCIL8, bw, bh);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_bloom_fbo[0]);
        p_glFramebufferRenderbuffer(PSXGL_FRAMEBUFFER, PSXGL_DEPTH_STENCIL_ATTACHMENT,
                                    PSXGL_RENDERBUFFER, s_bloom_rb);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
        s_bloom_w = lw; s_bloom_h = lh; s_bloom_mask_ok = 0;
    }
    if (src_ok) {
        /* marks: source rect (y0 = top row) -> window orientation (row 0 = bottom) */
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, s_bloom_src_fbo);
        p_glBindFramebuffer(PSXGL_DRAW_FRAMEBUFFER, s_bloom_fbo[0]);
        glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(s_bloom_src[0], s_bloom_src[1], s_bloom_src[2], s_bloom_src[3],
                            0, bh, bw, 0, GL_STENCIL_BUFFER_BIT, GL_NEAREST);
        s_bloom_mask_ok = 1;
    }
    if (!s_bloom_mask_ok) { p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0); return; }
    /* window image -> copy texture (bottom-up, as the window) */
    glBindTexture(GL_TEXTURE_2D, s_bloom_copy_tex);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, lx, ly, lw, lh);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST);

    /* bright pass, only where the marks allow */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_bloom_fbo[0]);
    glViewport(0, 0, bw, bh);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 0x02, 0x02);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    p_glUseProgram(s_bloom_bright_prog);
    p_glUniform1i(p_glGetUniformLocation(s_bloom_bright_prog, "u_tex"), 0);
    p_glUniform2f(p_glGetUniformLocation(s_bloom_bright_prog, "u_rcp"), 1.0f / (float)lw, 1.0f / (float)lh);
    p_glUniform1f(p_glGetUniformLocation(s_bloom_bright_prog, "u_thr"), s_bloom_thr);
    bloom_draw(s_bloom_bright_prog);
    glDisable(GL_STENCIL_TEST);
    if (s_bloom_view != 3) {
        p_glUseProgram(s_bloom_blur_prog);
        p_glUniform1i(p_glGetUniformLocation(s_bloom_blur_prog, "u_tex"), 0);
        const GLint ud = p_glGetUniformLocation(s_bloom_blur_prog, "u_dir");
        for (int it = 0; it < 3; it++) {
            const float sp = (float)(1 + it);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_bloom_fbo[1]);
            glBindTexture(GL_TEXTURE_2D, s_bloom_tex[0]);
            p_glUniform2f(ud, sp / (float)bw, 0.0f);
            bloom_draw(s_bloom_blur_prog);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_bloom_fbo[0]);
            glBindTexture(GL_TEXTURE_2D, s_bloom_tex[1]);
            p_glUniform2f(ud, 0.0f, sp / (float)bh);
            bloom_draw(s_bloom_blur_prog);
        }
    }
    /* composite into the window rect */
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    glViewport(lx, ly, lw, lh);
    p_glUseProgram(s_bloom_comp_prog);
    p_glActiveTexture(PSXGL_TEXTURE0);     glBindTexture(GL_TEXTURE_2D, s_bloom_copy_tex);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, s_bloom_tex[0]);
    p_glUniform1i(p_glGetUniformLocation(s_bloom_comp_prog, "u_tex"), 0);
    p_glUniform1i(p_glGetUniformLocation(s_bloom_comp_prog, "u_bloom"), 1);
    p_glUniform1f(p_glGetUniformLocation(s_bloom_comp_prog, "u_k"), s_bloom_view == 3 ? 1.0f : s_bloom);
    p_glUniform1i(p_glGetUniformLocation(s_bloom_comp_prog, "u_view"), s_bloom_view);
    bloom_draw(s_bloom_comp_prog);
    p_glActiveTexture(PSXGL_TEXTURE0 + 1); glBindTexture(GL_TEXTURE_2D, 0);
    p_glActiveTexture(PSXGL_TEXTURE0);     glBindTexture(GL_TEXTURE_2D, 0);
    p_glBindVertexArray(0);
    p_glUseProgram(0);
#ifndef PSX_NO_DEBUG_TOOLS
    if (s_bloom_time) {
        glFinish();
        s_bloom_us_sum += (double)(SDL_GetPerformanceCounter() - t0) * 1e6 /
                          (double)SDL_GetPerformanceFrequency();
        s_bloom_us_n++;
    }
#endif
    s_bloom_passes++;
}

/* Objects died with the context. */
static void bloom_release(void) {
    s_bloom_copy_tex = s_bloom_rb = 0;
    s_bloom_tex[0] = s_bloom_tex[1] = s_bloom_fbo[0] = s_bloom_fbo[1] = 0;
    s_bloom_bright_prog = s_bloom_blur_prog = s_bloom_comp_prog = 0;
    s_bloom_w = s_bloom_h = 0; s_bloom_failed = 0; s_bloom_mask_ok = 0; s_bloom_src_valid = 0;
}

/* PSX_BLOOM=<0..2> overrides [video] bloom. */
void gl_renderer_set_bloom(float strength) {
    GL_RT_SYNC("set_bloom");
    const char *e = getenv("PSX_BLOOM");
    if (e && *e) strength = (float)atof(e);
    if (!(strength >= 0.0f)) strength = 0.0f;
    if (strength > 2.0f) strength = 2.0f;
    s_bloom = strength;
    s_bcls_on = strength > 0.0f;
    pgxp_render_wanted_update();
}
float gl_renderer_get_bloom(void) { return s_bloom; }

void gl_renderer_bloom_debug(float strength, float threshold, int view, int time,
                             GlBloomInfo *out) {
    GL_RT_SYNC("bloom_debug");
    if (strength >= 0.0f && strength <= 2.0f) {
        s_bloom = strength; s_bcls_on = strength > 0.0f;
        pgxp_render_wanted_update();
    }
    if (threshold > 0.0f) s_bloom_thr = threshold;
    if (view >= 0) s_bloom_view = view;
    if (time >= 0) s_bloom_time = time ? 1 : 0;
    if (out) {
        out->strength = s_bloom; out->threshold = s_bloom_thr; out->view = s_bloom_view;
        out->passes = s_bloom_passes;
        out->us_mean = s_bloom_us_n ? s_bloom_us_sum / (double)s_bloom_us_n : 0.0;
        out->us_n = s_bloom_us_n;
        s_bloom_us_sum = 0.0; s_bloom_us_n = 0;
    }
}
