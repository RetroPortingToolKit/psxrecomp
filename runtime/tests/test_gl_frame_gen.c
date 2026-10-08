/* Frame generation on a real, hidden OpenGL context
 * (docs/FRAME_GENERATION.md). Run by run_gl_frame_gen.py with the render
 * thread on and frame generation off (argv[2] == 0) or on (1, forced: every
 * flip generates, whatever the surplus). A 30 Hz "game" double-buffers at
 * x = 0 / x = 512 and presents every VBlank (each game frame twice), paced at
 * 60 Hz: a fill, textured / shaded / flat triangles that move a little each
 * game frame, a fixed HUD rect, render-to-texture and a VRAM copy outside the
 * display, and every fifth frame a triangle that jumps (never matched). The
 * triangles carry projection sources (fixture_sources) as gpu.c records them
 * for GTE-projected geometry, so in-between frames follow the camera.
 * argv[3] == "wide" presents through a native-wide surface instead of VRAM;
 * argv[5] == "late" flips to each frame one VBlank after the next one has
 * started drawing (as R4 does).
 * Printed: real (digest of every distinct real presented image, in order; a
 * present that shows the same image again is not counted: generation replaces
 * those with in-between frames), presents, generated, and
 * with generation on the endpoint checks: the in-between image composed at
 * phase 1 equals the newer real frame's image, at phase 0 the older one's
 * (the last two game frames move without a jump, so every triangle pairs).
 * Original source-owned scene; no retail payload. */
static void fixture_present(int generated);
#define GL_PRESENT_TEST_HOOK(gen) fixture_present(gen)
/* argv[6] == "pt": the present thread is on; every image it puts in the
 * window (read back after its copy, before its swap) must be the image the
 * composing side presented, in the same order. */
static void fixture_window(int slot);
#define GL_PRESENT_THREAD_TEST_HOOK(slot) fixture_window(slot)
#include "gpu_gl_renderer.c"
#include "gpu_hd_texture_stubs.inc"
#include "mod_texture_banks.c"
/* gpu_render.c (the facade) observes draws for HD texture packs: no pack here. */
int gpu_hd_textures_active(void){return 0;}
int gpu_hd_textures_dump_enabled(void){return 0;}
void gpu_hd_textures_observe_draw(uint16_t tp,uint16_t cx,uint16_t cy,const int l[4],
    uint32_t w,int semi){(void)tp;(void)cx;(void)cy;(void)l;(void)w;(void)semi;}
void gpu_hd_textures_set_vram(const uint16_t* v){(void)v;}
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t* words){
    (void)x;(void)y;(void)w;(void)h;(void)words;}
void gpu_hd_textures_invalidate(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_upload(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_copy(int sx,int sy,int dx,int dy,int w,int h){
    (void)sx;(void)sy;(void)dx;(void)dy;(void)w;(void)h;}
void gpu_hd_textures_end_copy(void){}
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a){(void)n;(void)a;return 0;}
uint32_t psx_mod_read_word(uint32_t a){(void)a;return 0;}
int g_psx_vram_dirty_tracking=0;
uint64_t s_frame_count=0;
void gpu_vram_dirty_mark_row_impl(uint32_t y){(void)y;}
void gpu_vram_dirty_mark_rect(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_vram_dirty_mark_all(void){}
int psx_netplay_active(void){return 0;}
const GpuRenderBackend *vk_backend_get(void){return NULL;}
int gpu_display_is_depth24(void){return 0;}
void gpu_get_display_info(GpuDisplayInfo *out){memset(out,0,sizeof(*out));out->width=320;out->height=240;}
/* Guest state the backend reads per primitive. It changes with every
 * primitive here, so a replay that read it live (instead of the value
 * captured when the primitive was recorded) would draw a different image. */
static int g_prim_id;
static int g_bg_full;
int psx_ws_prim_in_backdrop(void){return (g_prim_id % 3) == 0;}
int psx_ws_prim_is_tagged(void){return (g_prim_id % 5) == 0;}
int gpu_ws_nw_flat_backdrop_enabled(void){return 0;}
int gpu_ws_background_requires_full_composite(void){return g_bg_full;}
int g_ws_tex_edge_pct=0;
void psx_ws_dbg_gate_frame_snapshot(void){}
void gpu_depth24_upload_span_reset(void){}
int gpu_ws_netplay_local_viewport_width(void){return 0;}
int render_pass_netplay_enabled(void){return 0;}
uint64_t psx_cycle_count=0;
uint32_t g_psx_vblank_cycles=564480u;
/* Frontend hooks of the present path (host overlays, pacing, rings). */
int host_osd_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
int host_osd_volume_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
int host_osd_needs_present(void){return 0;}
void host_osd_present_done(void){}
int psx_rewind_overlay_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
float psx_rewind_slide(void){return 0.0f;}
int psx_savestate_menu_overlay_image(const uint32_t **p,int *w,int *h){*p=NULL;*w=*h=0;return 0;}
int present_shot_take(char *out,int n){(void)out;(void)n;return 0;}
void present_shot_done(int ok){(void)ok;}
void psx_host_sleep_ms(uint32_t ms){(void)ms;}
/* Called on every present. On the render thread it also stands in for a slow
 * swap, so replay runs well behind recording and anything it read live from
 * the "guest" (instead of what was recorded) would be a later value. */
int psx_present_vsync_owns_cadence(void){return 0;}
void latency_ring_mark(LatencyStage s){(void)s;}
void gpu_timeline_note(uint8_t k,uint32_t a,uint32_t b){(void)k;(void)a;(void)b;}

static uint16_t vram[1024*512];
static int checks, failures;
static void check(int ok, const char *label) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", label); failures++; }
}
static uint64_t fnv(const void *pp, size_t n, uint64_t h) {
    const uint8_t *p = (const uint8_t *)pp;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* Presented images: every swap (render thread or not) passes here first. */
static uint64_t real_seq = 0xcbf29ce484222325ull;
static uint64_t n_real = 0, n_gen = 0;
static uint32_t *img_last[2];   /* the last two real images */
static int img_w, img_h;
static uint64_t read_back(uint32_t *dst) {
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    img_w = ww; img_h = wh;
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    return fnv(dst, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
}
#define MAX_SHOWN 4096
static int pt_mode;
static uint64_t composed_d[MAX_SHOWN], window_d[MAX_SHOWN];
static int n_composed, n_window;
static void fixture_window(int slot) {
    (void)slot;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    uint32_t *px = (uint32_t *)malloc((size_t)ww * wh * 4);
    p_glBindFramebuffer_raw(PSXGL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (n_window < MAX_SHOWN)
        window_d[n_window] = fnv(px, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
    n_window++;
    free(px);
}
static void fixture_present(int generated) {
    if (pt_mode) {
        int ww = 0, wh = 0;
        SDL_GL_GetDrawableSize(s_win, &ww, &wh);
        uint32_t *px = (uint32_t *)malloc((size_t)ww * wh * 4);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
        if (n_composed < MAX_SHOWN)
            composed_d[n_composed] = fnv(px, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        n_composed++;
        free(px);
    }
    if (generated) { n_gen++; return; }
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    for (int i = 0; i < 2; i++)
        if (!img_last[i]) img_last[i] = (uint32_t *)malloc((size_t)ww * wh * 4);
    uint32_t *t = img_last[0]; img_last[0] = img_last[1]; img_last[1] = t;
    static uint64_t last_d = 0;
    uint64_t d = read_back(img_last[1]);
    if (d == last_d) {   /* the same image shown again: not a new real frame */
        uint32_t *u = img_last[0]; img_last[0] = img_last[1]; img_last[1] = u;
        return;
    }
    last_d = d;
    if (getenv("FG_DUMP")) {
        printf("R %llu %016llx\n", (unsigned long long)n_real, (unsigned long long)d);
        char fn[64]; snprintf(fn, sizeof fn, "real%02llu.rgba", (unsigned long long)n_real);
        FILE *f = fopen(fn, "wb"); fwrite(img_last[1], 4, (size_t)ww * wh, f); fclose(f);
    }
    real_seq = fnv(&d, sizeof d, real_seq);
    n_real++;
}

#define FRAME_W 320
#define FRAME_H 240

static void textures(void) {
    static uint16_t page[64*64], clut[16];
    for (int i = 0; i < 64*64; i++) page[i] = (uint16_t)((i * 0x1357u) ^ (i >> 3));
    for (int i = 0; i < 16; i++) clut[i] = (uint16_t)(i ? (0x0421u * (uint16_t)i) | ((i & 1) << 15) : 0);
    /* Outside both buffers (x 0..319, 512..831; y 0..239). */
    for (int i = 0; i < 64*64; i++) vram[(i / 64) * 1024 + 896 + i % 64] = page[i];
    gr_vram_transfer_in(896, 0, 64, 64, page);
    gr_vram_transfer_in(512, 300, 16, 1, clut);
}

static int wide_mode;
/* What gpu.c records for a GTE-projected triangle (gl_renderer_fg_source):
 * vertex identities and camera-space positions that project (H = z = 1000)
 * to the triangle's screen positions relative to its buffer. */
/* Structurally different: a channel off by more than 48 (sub-pixel
 * placement at a high scale resamples textures by a few LSB). */
static int px_far(uint32_t a, uint32_t b) {
    for (int c = 0; c < 24; c += 8) { int d = (int)((a >> c) & 255) - (int)((b >> c) & 255); if (d > 48 || d < -48) return 1; }
    return 0;
}

static void fixture_sources(int i, int bx, int x, int y) {
    static const int off[4][6] = {
        { 0, 0, 30, 2, 3, 32 }, { 0, 0, 28, 0, 0, 28 }, { 0, 0, 26, 4, 6, 30 }, { 0, 0, 24, 20, -4, 26 } };
    const int *o = off[i % 4];
    uint32_t id[3]; int32_t pc[9], h[3], xs[3], ys[3];
    for (int k = 0; k < 3; k++) {
        id[k] = (uint32_t)(i * 3 + k + 1);
        xs[k] = x + o[2 * k]; ys[k] = y + o[2 * k + 1];
        pc[3 * k] = xs[k] - bx; pc[3 * k + 1] = ys[k]; pc[3 * k + 2] = 1000; h[k] = 1000;
    }
    gl_renderer_fg_source(id, pc, h, xs, ys);
}

/* One game frame drawn into buffer bx. Positions move by whole pixels with
 * g; every fifth frame triangle 0 jumps across the screen. */
static void game_frame_part(int g, int bx, int part) {
    g_prim_id = -1;
    gr_set_draw_area(bx, 0, bx + FRAME_W - 1, FRAME_H - 1);
    gr_set_draw_offset(bx, 0);
    if (wide_mode) gr_wide_set_target(bx);
    if (part == 0) {
        gr_fill_rect(bx, 0, FRAME_W, FRAME_H, 0x1084);
        if (wide_mode) gr_wide_clear(bx, 0, FRAME_H, 0x1084);
    }
    gr_set_color_modulation(128, 128, 128, 0);
    gr_set_semi_transparency(0, 0);
    for (int i = part ? 20 : 0; i < (part ? 40 : 20); i++) {
        /* The camera pans one pixel per game frame (every triangle moves
         * alike); triangle 5 is an object moving on its own. */
        int x = bx + 10 + (i % 8) * 36 + (i == 5 ? (g * 3) % 9 : g % 9) - 4;
        int y = 12 + (i / 8) * 42 + (i == 5 ? g % 5 : 0);
        if (i == 0 && g % 5 == 0) { x = bx + 250 - (g * 37) % 200; y = 180; }
        fixture_sources(i, bx, x, y);
        switch (i % 4) {
        case 0: gr_draw_shaded_textured_triangle(x, y, 0, 0, 0x808080, x + 30, y + 2, 63, 4, 0x6090b0,
                                                 x + 3, y + 32, 8, 63, 0xb09060, 512, 300, 0x000e, 0); break;
        case 1: gr_draw_textured_triangle(x, y, 4, 4, x + 28, y, 60, 4, x, y + 28, 4, 60, 512, 300, 0x000e); break;
        case 2: gr_draw_gouraud_triangle(x, y, 0x7c00, x + 26, y + 4, 0x03e0, x + 6, y + 30, 0x001f); break;
        default: gr_draw_flat_triangle(x, y, x + 24, y + 20, x - 4, y + 26, (uint16_t)(0x2108 * (i & 3) + 0x0421)); break;
        }
    }
    if (part == 0) return;
    gr_draw_flat_rect(bx + 8, 220, 80, 12, 0x7fff);              /* HUD */
    gr_draw_flat_rect(900, 300, 32, 32, (uint16_t)(0x03e0 + g));  /* render to texture */
    gr_copy_rect(bx + 10, 10, 900, 400, 16, 16);                  /* copy out of the display */
}

static void present(int bx) {
    if (wide_mode) (void)gl_renderer_present_wide_fbo(bx, 0, FRAME_H, 0);
    else gl_renderer_present_vram(bx, 0, FRAME_W, FRAME_H, 0, 0);
    gl_renderer_render_thread_frame_boundary();
}

int main(int argc, char **argv) {
    int scale = argc > 1 ? atoi(argv[1]) : 2;
    int fg = argc > 2 && argv[2][0] == '1';
    wide_mode = argc > 3 && !strcmp(argv[3], "wide");
    int frames = argc > 4 ? atoi(argv[4]) : 24;
    /* "late": the game flips to a frame one VBlank after it starts drawing
     * the next one (as R4 does), so a flip is not a list boundary. */
    int late = argc > 5 && !strcmp(argv[5], "late");
    pt_mode = argc > 6 && !strcmp(argv[6], "pt");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SKIP no video (%s)\n", SDL_GetError());
        return 77;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Frame generation hidden test", 0, 0, 320, 240,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window (%s)\n", SDL_GetError()); return 77; }
    {
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GLContext probe = SDL_GL_CreateContext(win);
        if (!probe) {
            fprintf(stderr, "SKIP no GL 3.3 core context (%s)\n", SDL_GetError());
            return 77;
        }
        SDL_GL_MakeCurrent(win, NULL);
        SDL_GL_DeleteContext(probe);
    }
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(scale);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "FAIL context\n"); return 2; }
    textures();
    if (wide_mode) gr_wide_configure(426, 53);
    if (pt_mode) gl_renderer_set_present_thread(1, 3);
    check(gl_renderer_render_thread_start(2) == 1, "render thread started");
    if (pt_mode) check(gl_renderer_present_thread_active(), "present thread started");
    if (fg) {
#ifdef _WIN32
        _putenv("PSX_FRAME_GEN_FORCE=1");
#else
        setenv("PSX_FRAME_GEN_FORCE", "1", 1);
#endif
        gl_renderer_set_frame_generation(1);
        check(gl_renderer_frame_generation() == 1, "frame generation on");
    }
    gl_renderer_frame_gen_configure(120.0, 59.94);
    for (int g = 0; g < frames; g++) {
        const int bx = (g & 1) ? 512 : 0, prev = bx ^ 512;
        if (late) {
            game_frame_part(g, bx, 0);
            present(prev);    /* flips to the previous frame */
            SDL_Delay(16);
            game_frame_part(g, bx, 1);
            present(prev);
            SDL_Delay(16);
            continue;
        }
        game_frame_part(g, bx, 0);
        game_frame_part(g, bx, 1);
        present(bx);          /* the game frame's first VBlank: a flip */
        SDL_Delay(16);
        present(bx);          /* its second: the same buffer again */
        SDL_Delay(16);
    }
    SDL_Delay(60);
    gl_renderer_render_thread_sync("test");   /* the render thread parks; we hold the context */
    if (fg) {
        char js[1024];
        gl_renderer_frame_gen_json(js, sizeof js);
        printf("fg {%s}\n", js);
        /* Endpoints: compose (no swap) at phase 1 and 0 between the last two
         * game frames and compare with their real images. */
        int ww = 0, wh = 0;
        SDL_GL_GetDrawableSize(s_win, &ww, &wh);
        uint32_t *img = (uint32_t *)malloc((size_t)ww * wh * 4);
        check(img_last[0] && img_last[1], "two real images kept");
        check(fg_generate(1.0, 0) == 1, "compose at phase 1");
        uint64_t d1 = read_back(img);
        uint64_t r1 = fnv(img_last[1], (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        if (getenv("FG_DUMP")) {
            FILE *f = fopen("p1.rgba", "wb"); fwrite(img, 4, (size_t)ww * wh, f); fclose(f);
            f = fopen("r1.rgba", "wb"); fwrite(img_last[1], 4, (size_t)ww * wh, f); fclose(f);
            f = fopen("r0.rgba", "wb"); fwrite(img_last[0], 4, (size_t)ww * wh, f); fclose(f);
            printf("dump %dx%d\n", ww, wh);
        }
        check(d1 == r1, "phase 1 equals the newer real frame");
        check(fg_generate(0.0, 0) == 1, "compose at phase 0");
        uint64_t d0 = read_back(img);
        uint64_t r0 = fnv(img_last[0], (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        /* Every projected triangle is where the older frame had it; what the
         * in-between camera uncovers (here: the background beside a moved
         * triangle, through its transparent texels) shows the newer frame
         * (frame_gen.h). So each phase-0 pixel is the older frame's, or the
         * newer frame's where they differ. */
        if (getenv("FG_DUMP")) { FILE *f = fopen("p0.rgba", "wb"); fwrite(img, 4, (size_t)ww * wh, f); fclose(f); }
        long diff = 0, neither = 0;
        for (int i = 0; i < ww * wh; i++)
            if (img[i] != img_last[0][i]) { diff++; neither += px_far(img[i], img_last[0][i]) && px_far(img[i], img_last[1][i]); }
        printf("phase0_diff_px=%ld (neither frame %ld) of %d camera=%u object=%u\n", diff, neither, ww * wh,
               s_fg_fit.camera, s_fg_fit.object);
        (void)d0; (void)r0;
        check(s_fg_fit.ok && s_fg_fit.camera + s_fg_fit.object == 120, "every projected vertex placed");
        check(neither * 1000 < (long)ww * wh, "phase 0: the older real frame, uncovered areas the newer (<0.1%)");
        check(fg_generate(0.5, 0) == 1, "compose at phase 0.5");
        uint64_t dm = read_back(img);
        if (getenv("FG_DUMP")) {
            FILE *f = fopen("pm.rgba", "wb"); fwrite(img, 4, (size_t)ww * wh, f); fclose(f);
        }
        check(dm != r0 && dm != r1, "phase 0.5 is neither endpoint");
        /* With an HD texture pack the CPU raster is native VRAM's authority;
         * an in-between frame is presentation only and must not draw there. */
        gl_renderer_set_hd_texture_mode(1);
        static uint16_t before[1024 * 512];
        memcpy(before, s_vram, sizeof before);
        check(fg_generate(0.5, 0) == 1, "compose at phase 0.5 under HD authority");
        check(memcmp(before, s_vram, sizeof before) == 0,
              "generated frame leaves native VRAM unchanged under HD authority");
        gl_renderer_set_hd_texture_mode(0);
        free(img);
        check(n_gen > 0, "generated frames were presented");
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("real=%016llx\n", (unsigned long long)real_seq);
    printf("presents=%llu generated=%llu\n", (unsigned long long)n_real, (unsigned long long)n_gen);
    gl_renderer_shutdown();   /* stops the render and present threads: all shown */
    if (pt_mode) {
        int same = n_composed == n_window && n_composed <= MAX_SHOWN && n_composed > 0;
        for (int i = 0; same && i < n_composed; i++) same = composed_d[i] == window_d[i];
        printf("pt composed=%d shown=%d\n", n_composed, n_window);
        check(same, "the window shows every composed image, in order");
    }
    printf("checks=%d failures=%d\n", checks, failures);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return failures ? 1 : 0;
}
