/* Exact indexed-page decode cache. Included after TEX_FS and batch helpers.
 * Array layers contain native 1555 words, not filtered/display colours. */
#ifndef GL_TEXTURE_2D_ARRAY
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#endif
typedef void (APIENTRY *TcImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei,
                                  GLint, GLenum, GLenum, const void *);
typedef void (APIENTRY *TcAttachLayer)(GLenum, GLenum, GLuint, GLint, GLint);
#define TC_CAP 256
typedef struct {
    GLuint source, palette;
    int x, y, depth, cx, cy, valid, pinned;
    uint64_t stamp, used, checked_epoch;
    int next;
} TexelCacheEntry;
static TexelCacheEntry s_tc[TC_CAP];
static uint64_t s_tc_tiles[32][64], s_tc_epoch, s_tc_clock;
static uint64_t s_tc_hits, s_tc_misses, s_tc_evictions;
static GLuint s_tc_array, s_tc_fbo, s_tc_prog;
static GLint s_tc_page, s_tc_clut, s_tc_depth;
static int s_tc_ready = -1, s_tc_capacity = TC_CAP;
static TcAttachLayer s_tc_attach;
static int s_tc_pins[TC_CAP], s_tc_pin_n;
static int s_tc_heads[1024], s_tc_last = -1;
static int texel_cache_lookup_fast(void) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PSX_GL_TEXEL_LOOKUP_FAST");
        enabled = !(e && e[0] == '0');
    }
    return enabled;
}
static unsigned texel_cache_hash(GLuint source, GLuint palette, int x, int y, int depth, int cx, int cy) {
    uint32_t h = source * 0x9e3779b9u ^ palette * 0x85ebca6bu;
    h ^= (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    h ^= (uint32_t)cx * 83492791u ^ (uint32_t)cy * 2654435761u ^ (uint32_t)depth;
    h ^= h >> 16;
    return h & 1023u;
}
static int texel_cache_matches(const TexelCacheEntry *c, GLuint source, GLuint palette,
                              int x, int y, int depth, int cx, int cy) {
    return c->valid && c->source==source && c->palette==palette && c->x==x && c->y==y &&
           c->depth==depth && c->cx==cx && c->cy==cy;
}
static void texel_cache_pin(int slot) {
    if (!s_tc[slot].pinned) { s_tc[slot].pinned=1; s_tc_pins[s_tc_pin_n++]=slot; }
}
static void texel_cache_unpin(void) {
    for (int i=0; i<s_tc_pin_n; ++i) s_tc[s_tc_pins[i]].pinned=0;
    s_tc_pin_n=0;
}

static void texel_cache_dirty(int x0, int y0, int x1, int y1) {
    if (s_tc_ready != 1) return;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > 1023) x1 = 1023; if (y1 > 511) y1 = 511;
    if (x1 < x0 || y1 < y0) return;
    uint64_t stamp = ++s_tc_epoch;
    for (int y=y0/16; y<=y1/16; ++y)
        for (int x=x0/16; x<=x1/16; ++x) s_tc_tiles[y][x] = stamp;
}
static uint64_t texel_cache_region(int x, int y, int w, int h) {
    uint64_t stamp = 0;
    for (int ty=y/16; ty<=(y+h-1)/16; ++ty)
        for (int tx=x/16; tx<=(x+w-1)/16; ++tx) {
            uint64_t v = s_tc_tiles[ty & 31][tx & 63];
            if (v > stamp) stamp = v;
        }
    return stamp;
}
static void texel_cache_reset(void) {
    memset(s_tc, 0, sizeof s_tc);
    memset(s_tc_heads, 0, sizeof s_tc_heads);
    s_tc_last = -1;
    memset(s_tc_tiles, 0, sizeof s_tc_tiles);
    s_tc_epoch = s_tc_clock = s_tc_hits = s_tc_misses = s_tc_evictions = 0;
    s_tc_array = s_tc_fbo = s_tc_prog = 0;
    s_tc_ready = -1;
    s_tc_pin_n = 0;
}
static int texel_cache_init(void) {
    if (s_tc_ready >= 0) return s_tc_ready;
    s_tc_ready = 0;
    const char *e = getenv("PSX_GL_TEXEL_CACHE");
    if (e && e[0] == '0') return 0;
    e = getenv("PSX_GL_TEXEL_CACHE_LAYERS");
    s_tc_capacity = e ? atoi(e) : TC_CAP;
    if (s_tc_capacity < 4) s_tc_capacity = 4;
    if (s_tc_capacity > TC_CAP) s_tc_capacity = TC_CAP;
    TcImage3D image = (TcImage3D)SDL_GL_GetProcAddress("glTexImage3D");
    s_tc_attach = (TcAttachLayer)SDL_GL_GetProcAddress("glFramebufferTextureLayer");
    if (!image || !s_tc_attach) return 0;
    /* Reuse the literal native address/decode functions from TEX_FS. The
     * decoder has no window or UV bounds: fetch_texel applies those before
     * it samples the decoded layer. This avoids a second decode definition. */
    const char *helpers = strstr(TEX_FS, "int vram_at(");
    const char *fetch = strstr(TEX_FS, "int fetch_texel(");
    const char *native = strstr(TEX_FS, "  if (v_depth == 0)");
    const char *end = strstr(TEX_FS, "vec3 col5(");
    if (!helpers || !fetch || !native || !end) return 0;
    char *fs = malloc(strlen(TEX_FS) + 512);
    if (!fs) return 0;
    snprintf(fs, strlen(TEX_FS) + 512,
        "#version 330\nuniform usampler2D u_vram, u_palette;\n"
        "uniform ivec2 v_tpage, v_clut; uniform int v_depth;\n"
        "layout(location=0) out uint decoded_word;\n%.*s"
        "int decode_native(int u,int v){\n%.*s"
        "void main(){decoded_word=uint(decode_native(int(gl_FragCoord.x),int(gl_FragCoord.y)));}\n",
        (int)(fetch-helpers), helpers, (int)(end-native), native);
    s_tc_prog = build_program(PRESENT_VS, fs);
    free(fs);
    if (!s_tc_prog) return 0;
    glGenTextures(1, &s_tc_array);
    p_glActiveTexture(PSXGL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_tc_array);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    p_glBindBuffer(PSXGL_PIXEL_UNPACK_BUFFER, 0);
    image(GL_TEXTURE_2D_ARRAY, 0, PSXGL_R16UI, 256, 256, s_tc_capacity, 0,
          PSXGL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
    p_glActiveTexture(PSXGL_TEXTURE0);
    if (glGetError() != GL_NO_ERROR) return 0;
    p_glGenFramebuffers(1, &s_tc_fbo);
    s_tc_page = p_glGetUniformLocation(s_tc_prog, "v_tpage");
    s_tc_clut = p_glGetUniformLocation(s_tc_prog, "v_clut");
    s_tc_depth = p_glGetUniformLocation(s_tc_prog, "v_depth");
    p_glUseProgram(s_tc_prog);
    p_glUniform1i(p_glGetUniformLocation(s_tc_prog, "u_vram"), 0);
    p_glUniform1i(p_glGetUniformLocation(s_tc_prog, "u_palette"), 1);
    s_tc_ready = 1;
    return 1;
}
static void texel_cache_bind(void) {
    p_glActiveTexture(PSXGL_TEXTURE0 + 3);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_tc_array);
    p_glActiveTexture(PSXGL_TEXTURE0);
}
static int texel_cache_get(GLuint source, GLuint palette, int x, int y, int depth, int cx, int cy) {
    if (depth >= 2 || source != s_raw_tex || palette != s_raw_tex ||
        !texel_cache_init()) return -1;
    const int fast = texel_cache_lookup_fast();
    const unsigned bucket = texel_cache_hash(source,palette,x,y,depth,cx,cy);
    if (fast) {
        int hit = s_tc_last;
        if (hit < 0 || !texel_cache_matches(&s_tc[hit],source,palette,x,y,depth,cx,cy)) {
            hit = s_tc_heads[bucket] - 1;
            while (hit >= 0 && !texel_cache_matches(&s_tc[hit],source,palette,x,y,depth,cx,cy))
                hit = s_tc[hit].next - 1;
        }
        if (hit >= 0) {
            TexelCacheEntry *c = &s_tc[hit];
            if (c->checked_epoch != s_tc_epoch) {
                uint64_t stamp = source == s_raw_tex ? texel_cache_region(x,y,depth ? 128 : 64,256) : 0;
                uint64_t ps = palette == s_raw_tex ? texel_cache_region(cx,cy,depth ? 256 : 16,1) : 0;
                if (ps > stamp) stamp = ps;
                if (c->stamp == stamp) c->checked_epoch = s_tc_epoch;
            }
            if (c->checked_epoch == s_tc_epoch) {
                c->used=++s_tc_clock; texel_cache_pin(hit); s_tc_last=hit; s_tc_hits++; return hit;
            }
        }
    }
    uint64_t stamp = source == s_raw_tex ? texel_cache_region(x,y,depth ? 128 : 64,256) : 0;
    uint64_t ps = palette == s_raw_tex ? texel_cache_region(cx,cy,depth ? 256 : 16,1) : 0;
    if (ps > stamp) stamp = ps;
    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    for (int i=0; i<s_tc_capacity; ++i) {
        TexelCacheEntry *c = &s_tc[i];
        if (!fast && texel_cache_matches(c,source,palette,x,y,depth,cx,cy)) {
            if (c->stamp == stamp) { c->used=++s_tc_clock; texel_cache_pin(i); s_tc_hits++; return i; }
        }
        if (!c->valid) { if (oldest != 0) { slot=i; oldest=0; } }
        else if (!c->pinned && c->used < oldest) { oldest=c->used; slot=i; }
    }
    s_tc_misses++;
    if (slot < 0) return -1; /* All layers pinned: preserve the original batch. */
    if (s_tc[slot].valid) {
        /* Only layers without deferred CPU draw references can be reused.
         * GPU readers were already submitted, so command ordering protects
         * them. Never split a mask-checked batch to make cache space. */
        s_tc_evictions++;

    }
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, s_tc_fbo);
    s_tc_attach(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, s_tc_array, 0, slot);
    if (p_glCheckFramebufferStatus(PSXGL_FRAMEBUFFER) != PSXGL_FRAMEBUFFER_COMPLETE) return -1;
    glViewport(0,0,256,256);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    p_glUseProgram(s_tc_prog);
    p_glUniform2i(s_tc_page,x,y); p_glUniform2i(s_tc_clut,cx,cy); p_glUniform1i(s_tc_depth,depth);
    p_glActiveTexture(PSXGL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,source);
    p_glActiveTexture(PSXGL_TEXTURE0+1); glBindTexture(GL_TEXTURE_2D,palette);
    p_glActiveTexture(PSXGL_TEXTURE0+3); glBindTexture(GL_TEXTURE_2D_ARRAY,0);
    p_glBindVertexArray(s_empty_vao); glDrawArrays(GL_TRIANGLES,0,3);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,s_hr_fbo);
    p_glActiveTexture(PSXGL_TEXTURE0);
    if (s_tc[slot].valid) {
        TexelCacheEntry *c = &s_tc[slot];
        unsigned old = texel_cache_hash(c->source,c->palette,c->x,c->y,c->depth,c->cx,c->cy);
        int *link = &s_tc_heads[old];
        while (*link && *link != slot+1) link = &s_tc[*link-1].next;
        if (*link) *link = c->next;
    }
    s_tc[slot]=(TexelCacheEntry){.source=source,.palette=palette,.x=x,.y=y,.depth=depth,
        .cx=cx,.cy=cy,.valid=1,.stamp=stamp,.used=++s_tc_clock,
        .checked_epoch=s_tc_epoch,.next=s_tc_heads[bucket]};
    s_tc_heads[bucket]=slot+1;
    s_tc_last=slot;
    texel_cache_pin(slot);
    return slot;
}
