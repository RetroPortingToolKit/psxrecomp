/* postfx_chain.c — post-processing chain presets: parser and built-ins
 * (include/postfx_chain.h, docs/POST_EFFECTS.md). No GL here. */
#include "postfx_chain.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- built-in shaders ---------------------------------------------------- */

/* FXAA ([video] antialiasing_mode), unchanged from the former fixed pass. */
static const char FXAA_FS[] =
    "uniform float u_quality;\n"
    "float L(vec3 c){ return dot(c, vec3(0.299,0.587,0.114)); }\n"
    "float LS(vec2 p){ return L(textureLod(u_tex,p,0.0).rgb); }\n"
    "void main(){\n"
    "  vec3 cM = textureLod(u_tex, v_uv, 0.0).rgb; float lM = L(cM);\n"
    "  float lN = LS(v_uv+vec2(0.0, u_rcp.y)), lS = LS(v_uv-vec2(0.0, u_rcp.y));\n"
    "  float lE = LS(v_uv+vec2(u_rcp.x,0.0)), lW = LS(v_uv-vec2(u_rcp.x,0.0));\n"
    "  float mx = max(lM,max(max(lN,lS),max(lE,lW)));\n"
    "  float mn = min(lM,min(min(lN,lS),min(lE,lW)));\n"
    "  float range = mx - mn;\n"
    /* thresholds: absolute 1/16 (1/24 at high quality), relative 1/8 */
    "  float thr = u_quality > 0.5 ? 0.0417 : 0.0625;\n"
    "  if (range < max(thr, mx*0.125)) { o = vec4(cM,1.0); return; }\n"
    "  float lNE = LS(v_uv+u_rcp), lSW = LS(v_uv-u_rcp);\n"
    "  float lNW = LS(v_uv+vec2(-u_rcp.x,u_rcp.y)), lSE = LS(v_uv+vec2(u_rcp.x,-u_rcp.y));\n"
    "  float eH = abs(lNW+lNE-2.0*lN) + 2.0*abs(lW+lE-2.0*lM) + abs(lSW+lSE-2.0*lS);\n"
    "  float eV = abs(lNW+lSW-2.0*lW) + 2.0*abs(lN+lS-2.0*lM) + abs(lNE+lSE-2.0*lE);\n"
    "  bool horz = eH >= eV;\n"
    "  float l1 = horz ? lS : lW, l2 = horz ? lN : lE;\n"
    "  float g1 = abs(l1-lM), g2 = abs(l2-lM);\n"
    "  bool neg = g1 >= g2;\n"
    "  float grad = 0.25*max(g1,g2);\n"
    "  float step = horz ? u_rcp.y : u_rcp.x;\n"
    "  float lLocal; if (neg) { step = -step; lLocal = 0.5*(l1+lM); } else lLocal = 0.5*(l2+lM);\n"
    "  vec2 p = v_uv; if (horz) p.y += 0.5*step; else p.x += 0.5*step;\n"
    "  vec2 off = horz ? vec2(u_rcp.x,0.0) : vec2(0.0,u_rcp.y);\n"
    "  vec2 p1 = p - off, p2 = p + off;\n"
    "  float e1 = LS(p1)-lLocal, e2 = LS(p2)-lLocal;\n"
    "  bool d1 = abs(e1) >= grad, d2 = abs(e2) >= grad;\n"
    "  int steps = u_quality > 0.5 ? 12 : 8;\n"
    "  for (int i = 1; i < steps && !(d1 && d2); i++) {\n"
    "    float k = i < 2 ? 1.0 : (i < 5 ? 1.5 : (i < 8 ? 2.0 : 4.0));\n"
    "    if (!d1) { p1 -= off*k; e1 = LS(p1)-lLocal; d1 = abs(e1) >= grad; }\n"
    "    if (!d2) { p2 += off*k; e2 = LS(p2)-lLocal; d2 = abs(e2) >= grad; }\n"
    "  }\n"
    "  float dist1 = horz ? v_uv.x-p1.x : v_uv.y-p1.y;\n"
    "  float dist2 = horz ? p2.x-v_uv.x : p2.y-v_uv.y;\n"
    "  bool near1 = dist1 < dist2; float dmin = min(dist1,dist2);\n"
    "  float len = dist1 + dist2;\n"
    "  bool mLess = (lM - lLocal) < 0.0;\n"
    "  bool good = ((near1 ? e1 : e2) < 0.0) != mLess;\n"
    "  float eo = good ? (0.5 - dmin/len) : 0.0;\n"
    "  float avg = (2.0*(lN+lS+lE+lW) + lNE+lNW+lSE+lSW) / 12.0;\n"
    "  float sp = clamp(abs(avg-lM)/range, 0.0, 1.0);\n"
    "  sp = (-2.0*sp + 3.0)*sp*sp; sp = sp*sp*0.75;\n"
    "  float f = max(eo, sp);\n"
    "  vec2 q = v_uv; if (horz) q.y += f*step; else q.x += f*step;\n"
    "  o = vec4(textureLod(u_tex, q, 0.0).rgb, 1.0);\n"
    "}\n";

/* Bloom: soft-knee bright pass of lit pixels (the mask) of the image already
 * scaled to this pass (source_down). */
static const char BLOOM_BRIGHT_FS[] =
    "uniform float u_thr;\n"
    "void main(){\n"
    "  vec2 r = 0.25 / u_size;\n"   /* the mask's 4x4 footprint, 4 taps */
    "  float m = 0.25*(textureLod(u_mask, v_uv+r*vec2(-1,-1), 0.0).r + textureLod(u_mask, v_uv+r*vec2(1,-1), 0.0).r\n"
    "                + textureLod(u_mask, v_uv+r*vec2(-1,1), 0.0).r + textureLod(u_mask, v_uv+r*vec2(1,1), 0.0).r);\n"
    "  vec3 c = textureLod(u_tex, v_uv, 0.0).rgb * m;\n"
    "  float l = max(c.r, max(c.g, c.b));\n"
    "  float k = 0.15; float s = clamp(l - u_thr + k, 0.0, 2.0*k); s = s*s/(4.0*k+1e-5);\n"
    "  float w = max(s, l - u_thr) / max(l, 1e-4);\n"
    "  o = vec4(c * w, 1.0);\n"
    "}\n";

/* Separable 9-tap Gaussian (linear-sampled); u_dir in target px. */
static const char BLUR_FS[] =
    "uniform vec2 u_dir;\n"
    "void main(){\n"
    "  vec2 d = u_dir * u_rcp;\n"
    "  vec3 c = textureLod(u_tex, v_uv, 0.0).rgb * 0.2270270;\n"
    "  c += (textureLod(u_tex, v_uv + d*1.3846154, 0.0).rgb + textureLod(u_tex, v_uv - d*1.3846154, 0.0).rgb) * 0.3162162;\n"
    "  c += (textureLod(u_tex, v_uv + d*3.2307692, 0.0).rgb + textureLod(u_tex, v_uv - d*3.2307692, 0.0).rgb) * 0.0702703;\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";

/* The glow, screen-blended (blend screen) onto lit pixels only: every other
 * pixel is discarded, so it keeps the image's value exactly (the HUD next to
 * a lamp included). No copy of the image is read. */
static const char BLOOM_COMPOSITE_FS[] =
    "uniform float u_strength;\n"
    "void main(){\n"
    "  if (texelFetch(u_mask, ivec2(gl_FragCoord.xy) - u_org, 0).r < 0.5) discard;\n"
    "  o = vec4(clamp(textureLod(u_bloom, v_uv, 0.0).rgb * u_strength, 0.0, 1.0), 0.0);\n"
    "}\n";

static const struct { const char *name, *body; } k_shaders[] = {
    { "fxaa", FXAA_FS },
    { "bloom_bright", BLOOM_BRIGHT_FS },
    { "blur", BLUR_FS },
    { "bloom_composite", BLOOM_COMPOSITE_FS },
};

const char *pfx_builtin_shader(const char *name) {
    for (size_t i = 0; i < sizeof k_shaders / sizeof k_shaders[0]; i++)
        if (!strcmp(k_shaders[i].name, name)) return k_shaders[i].body;
    return NULL;
}

/* ---- built-in presets ---------------------------------------------------- */

static const char BLOOM_PFX[] =
    "pfx 1\n"
    "# [video] bloom: glow from 3D and additive primitives only (the mask).\n"
    "# Smooth motion's generated frames reuse the real frame's glow (bright and\n"
    "# blur) and run only the masked composite over their own picture. Nothing\n"
    "# copies the full-size image: the bright pass reads it already scaled\n"
    "# down and the composite blends onto the window.\n"
    "pass bright\n"
    "  shader builtin:bloom_bright\n"
    "  input u_tex source_down\n"
    "  input u_mask mask\n"
    "  scale 0.25\n  format rgba16f\n"
    "  uniform u_thr 0.55\n"
    "  generated reuse\n"
    "pass h1\n  shader builtin:blur\n  input u_tex previous\n  scale 0.25\n  format rgba16f\n  generated reuse\n  uniform u_dir 1 0\n"
    "pass v1\n  shader builtin:blur\n  input u_tex previous\n  scale 0.25\n  format rgba16f\n  generated reuse\n  uniform u_dir 0 1\n"
    "pass h2\n  shader builtin:blur\n  input u_tex previous\n  scale 0.25\n  format rgba16f\n  generated reuse\n  uniform u_dir 2 0\n"
    "pass v2\n  shader builtin:blur\n  input u_tex previous\n  scale 0.25\n  format rgba16f\n  generated reuse\n  uniform u_dir 0 2\n"
    "pass h3\n  shader builtin:blur\n  input u_tex previous\n  scale 0.25\n  format rgba16f\n  generated reuse\n  uniform u_dir 3 0\n"
    "pass v3\n  shader builtin:blur\n  input u_tex previous\n  scale 0.25\n  format rgba16f\n  generated reuse\n  uniform u_dir 0 3\n"
    "pass composite\n"
    "  shader builtin:bloom_composite\n"
    "  input u_bloom previous\n"
    "  input u_mask mask\n"
    "  blend screen\n"
    "  uniform u_strength 1.0\n";

static const char FXAA_PFX[] =
    "pfx 1\npass fxaa\n  shader builtin:fxaa\n  input u_tex source\n  uniform u_quality 0\n";
static const char FXAA_HQ_PFX[] =
    "pfx 1\npass fxaa\n  shader builtin:fxaa\n  input u_tex source\n  uniform u_quality 1\n";

static char *pfx_builtin_only(const char *arg, void *user, char *err, int errlen) {
    (void)user;
    if (strncmp(arg, "builtin:", 8) == 0) {
        const char *b = pfx_builtin_shader(arg + 8);
        if (b) return strdup(b);
        snprintf(err, (size_t)errlen, "unknown built-in shader '%s'", arg + 8);
        return NULL;
    }
    snprintf(err, (size_t)errlen, "built-in presets take built-in shaders only: '%s'", arg);
    return NULL;
}

int pfx_chain_builtin(const char *name, PfxChain *out, char *err, int errlen) {
    const char *text = !strcmp(name, "bloom") ? BLOOM_PFX
                     : !strcmp(name, "fxaa") ? FXAA_PFX
                     : !strcmp(name, "fxaa_hq") ? FXAA_HQ_PFX : NULL;
    if (!text) { snprintf(err, (size_t)errlen, "unknown preset '%s'", name); return 0; }
    return pfx_chain_parse(text, name, pfx_builtin_only, NULL, out, err, errlen);
}

/* ---- parser -------------------------------------------------------------- */

static int pfx_fail(char *err, int errlen, int line, const char *fmt, ...) {
    char msg[200];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    snprintf(err, (size_t)errlen, "line %d: %s", line, msg);
    return 0;
}

static int pfx_word(const char **p, char *out, int n) {
    while (**p == ' ' || **p == '\t') (*p)++;
    int i = 0;
    while (**p && **p != ' ' && **p != '\t' && **p != '\n' && **p != '\r' && **p != '#') {
        if (i < n - 1) out[i++] = **p;
        (*p)++;
    }
    out[i] = 0;
    return i > 0;
}

void pfx_chain_free(PfxChain *c) {
    if (!c) return;
    for (int i = 0; i < c->npasses; i++) { free(c->pass[i].shader); c->pass[i].shader = NULL; }
    c->npasses = 0;
}

int pfx_chain_parse(const char *text, const char *name, PfxShaderLoader load, void *user,
                    PfxChain *out, char *err, int errlen) {
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name ? name : "");
    if (!load) load = pfx_builtin_only;
    int line = 0, have_header = 0;
    PfxPass *cur = NULL;
    const char *p = text;
    while (*p) {
        line++;
        const char *eol = strchr(p, '\n');
        const char *next = eol ? eol + 1 : p + strlen(p);
        char kw[PFX_NAME];
        const char *q = p;
        if (!pfx_word(&q, kw, sizeof kw)) { p = next; continue; }   /* blank / comment */
        if (!have_header) {
            char v[16];
            if (strcmp(kw, "pfx") || !pfx_word(&q, v, sizeof v) || strcmp(v, "1")) {
                pfx_chain_free(out);
                return pfx_fail(err, errlen, line, "expected 'pfx 1' first");
            }
            have_header = 1;
        } else if (!strcmp(kw, "pass")) {
            if (out->npasses == PFX_MAX_PASSES) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "more than %d passes", PFX_MAX_PASSES); }
            cur = &out->pass[out->npasses++];
            if (!pfx_word(&q, cur->name, sizeof cur->name)) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "pass needs a name"); }
            for (int i = 0; i < out->npasses - 1; i++)
                if (!strcmp(out->pass[i].name, cur->name)) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "duplicate pass '%s'", cur->name); }
            cur->scale = 1.0f;
        } else if (!cur) {
            pfx_chain_free(out);
            return pfx_fail(err, errlen, line, "'%s' outside a pass", kw);
        } else if (!strcmp(kw, "shader")) {
            char arg[512], e2[160] = "";
            if (!pfx_word(&q, arg, sizeof arg)) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "shader needs an argument"); }
            free(cur->shader);
            cur->shader = load(arg, user, e2, sizeof e2);
            if (!cur->shader) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "%s", e2); }
        } else if (!strcmp(kw, "input")) {
            char s[PFX_NAME], f[PFX_NAME];
            if (!pfx_word(&q, s, sizeof s) || !pfx_word(&q, f, sizeof f)) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "input <sampler> <source>"); }
            if (cur->ninputs == PFX_MAX_INPUTS) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "more than %d inputs", PFX_MAX_INPUTS); }
            int from;
            const int self = (int)(cur - out->pass);
            if (!strcmp(f, "source")) from = PFX_IN_SOURCE;
            else if (!strcmp(f, "source_down")) from = PFX_IN_SOURCE_DOWN;
            else if (!strcmp(f, "mask")) { from = PFX_IN_MASK; out->uses_mask = 1; }
            else if (!strcmp(f, "depth")) { from = PFX_IN_DEPTH; out->uses_depth = 1; }
            else if (!strcmp(f, "history")) from = PFX_IN_HISTORY;
            else if (!strcmp(f, "previous")) {
                if (self == 0) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "the first pass has no previous"); }
                from = self - 1;
            } else {
                from = -100;
                for (int i = 0; i < self; i++) if (!strcmp(out->pass[i].name, f)) from = i;
                if (from == -100) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "unknown input '%s' (source, mask, depth, history, previous or an earlier pass)", f); }
            }
            snprintf(cur->in[cur->ninputs].sampler, PFX_NAME, "%s", s);
            cur->in[cur->ninputs++].from = from;
        } else if (!strcmp(kw, "scale")) {
            char v[32];
            float f = pfx_word(&q, v, sizeof v) ? strtof(v, NULL) : 0.0f;
            if (!(f > 0.0f && f <= 4.0f)) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "scale must be in (0, 4]"); }
            cur->scale = f;
        } else if (!strcmp(kw, "format")) {
            char v[16]; pfx_word(&q, v, sizeof v);
            if (!strcmp(v, "rgba8")) cur->format = PFX_FMT_RGBA8;
            else if (!strcmp(v, "rgba16f")) cur->format = PFX_FMT_RGBA16F;
            else { pfx_chain_free(out); return pfx_fail(err, errlen, line, "format must be rgba8 or rgba16f"); }
        } else if (!strcmp(kw, "filter")) {
            char v[16]; pfx_word(&q, v, sizeof v);
            if (!strcmp(v, "linear")) cur->nearest = 0;
            else if (!strcmp(v, "nearest")) cur->nearest = 1;
            else { pfx_chain_free(out); return pfx_fail(err, errlen, line, "filter must be linear or nearest"); }
        } else if (!strcmp(kw, "blend")) {
            char v[16]; pfx_word(&q, v, sizeof v);
            if (!strcmp(v, "replace")) cur->blend = PFX_BLEND_REPLACE;
            else if (!strcmp(v, "screen")) cur->blend = PFX_BLEND_SCREEN;
            else { pfx_chain_free(out); return pfx_fail(err, errlen, line, "blend must be replace or screen"); }
        } else if (!strcmp(kw, "generated")) {
            char v[16]; pfx_word(&q, v, sizeof v);
            if (!strcmp(v, "reuse")) cur->reuse_generated = 1;
            else if (!strcmp(v, "run")) cur->reuse_generated = 0;
            else { pfx_chain_free(out); return pfx_fail(err, errlen, line, "generated must be reuse or run"); }
        } else if (!strcmp(kw, "uniform")) {
            if (cur->nuniforms == PFX_MAX_UNIFORMS) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "more than %d uniforms", PFX_MAX_UNIFORMS); }
            PfxUniform *u = &cur->u[cur->nuniforms];
            if (!pfx_word(&q, u->name, sizeof u->name)) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "uniform needs a name"); }
            char v[32]; int n = 0;
            while (n < 4 && pfx_word(&q, v, sizeof v)) {
                char *end; u->v[n] = strtof(v, &end);
                if (*end) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "bad number '%s'", v); }
                n++;
            }
            if (!n) { pfx_chain_free(out); return pfx_fail(err, errlen, line, "uniform '%s' needs 1..4 values", u->name); }
            u->count = n; cur->nuniforms++;
        } else {
            pfx_chain_free(out);
            return pfx_fail(err, errlen, line, "unknown directive '%s'", kw);
        }
        p = next;
    }
    if (!have_header) return pfx_fail(err, errlen, line, "empty preset");
    if (!out->npasses) return pfx_fail(err, errlen, line, "no passes");
    for (int i = 0; i < out->npasses; i++)
        if (!out->pass[i].shader) { int n = out->npasses; snprintf(err, (size_t)errlen, "pass '%s' has no shader", out->pass[i].name); (void)n; pfx_chain_free(out); return 0; }
    /* The window pass draws over the game rect at its own size, every time. */
    out->pass[out->npasses - 1].scale = 1.0f;
    out->pass[out->npasses - 1].reuse_generated = 0;
    for (int i = 0; i < out->npasses - 1; i++)
        if (out->pass[i].blend != PFX_BLEND_REPLACE) {
            snprintf(err, (size_t)errlen, "pass '%s': only the last pass may blend", out->pass[i].name);
            pfx_chain_free(out);
            return 0;
        }
    return 1;
}

/* ---- files --------------------------------------------------------------- */

static char *pfx_read_file(const char *path, char *err, int errlen) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, (size_t)errlen, "cannot open '%s'", path); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1 << 20)) { fclose(f); snprintf(err, (size_t)errlen, "'%s' too large", path); return NULL; }
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); snprintf(err, (size_t)errlen, "out of memory"); return NULL; }
    size_t got = fread(b, 1, (size_t)n, f); fclose(f);
    b[got] = 0;
    return b;
}

static char *pfx_file_loader(const char *arg, void *user, char *err, int errlen) {
    if (strncmp(arg, "builtin:", 8) == 0) return pfx_builtin_only(arg, NULL, err, errlen);
    const char *dir = (const char *)user;
    char path[1024];
    if (arg[0] == '/' || !dir || !*dir) snprintf(path, sizeof path, "%s", arg);
    else snprintf(path, sizeof path, "%s/%s", dir, arg);
    return pfx_read_file(path, err, errlen);
}

int pfx_chain_load_file(const char *path, PfxChain *out, char *err, int errlen) {
    memset(out, 0, sizeof *out);
    char *text = pfx_read_file(path, err, errlen);
    if (!text) return 0;
    char dir[1024]; snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
#ifdef _WIN32
    char *bs = strrchr(dir, '\\'); if (bs && (!slash || bs > slash)) slash = bs;
#endif
    if (slash) *slash = 0; else dir[0] = 0;
    const char *base = slash ? slash + 1 : path;
    (void)base;
    int ok = pfx_chain_parse(text, path, pfx_file_loader, dir, out, err, errlen);
    free(text);
    return ok;
}

int pfx_chain_set_uniform(PfxChain *c, const char *name, int count, const float *v) {
    int n = 0;
    for (int i = 0; i < c->npasses; i++)
        for (int j = 0; j < c->pass[i].nuniforms; j++) {
            PfxUniform *u = &c->pass[i].u[j];
            if (strcmp(u->name, name)) continue;
            u->count = count > 4 ? 4 : count;
            for (int k = 0; k < u->count; k++) u->v[k] = v[k];
            n++;
        }
    return n;
}
