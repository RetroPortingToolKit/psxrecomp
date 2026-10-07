/* Internal-resolution invariance on a real, hidden OpenGL context.
 *
 * Run once per scale (and mode) by run_gl_scale_invariance.py, which compares
 * the digests across runs. Checks, per run:
 *   - the native VRAM the game can read back (pack of the authoritative
 *     surface) is the same at every scale and in every mode: supersampling
 *     must never change guest-visible pixels (lines excepted, see LINE_*,
 *     which are drawn differently above 1x and checked on their own);
 *   - a line is one NATIVE pixel thick at internal resolution (S hr rows);
 *   - a request the driver cannot hold is clamped inside GL, never dropped
 *     to the software renderer;
 *   - mode "window" (PSX_GL_HIRES_WINDOW=1): the windowed high-resolution
 *     surface renders the frame region exactly like the full-VRAM surface at
 *     the same scale (the runner compares the hires digests), including copies
 *     whose source straddles or lies outside the window, fills and uploads
 *     that cross its edge, and scales past the full-VRAM limit (18x = 8K).
 *   - mode "sbs" (with or without the window): side-by-side double buffering,
 *     two 512-wide frames at x=0 and x=512 flipped every frame, plus copies
 *     between them and copies wider than the GPU limit allows at S. Both
 *     buffers must stay held at S (in two tiles when their union is too wide
 *     for one surface) and render exactly like the full-VRAM surface; every
 *     staging texture's recorded size must match its real storage, and a
 *     scratch request past the GPU limit must be refused and leave it intact.
 *     Rows SBS_XBAND_Y0.. hold a sloped primitive across both tiles: GL clips
 *     it at each tile's edge, which can move interpolated colour by one step
 *     or coverage by a subpixel along it, so that band is only checked for
 *     being rendered at S (not an upscaled 1x image), not digested.
 *   - mode "lines" (with or without the window): lines interleaved with
 *     triangles that overlap them, over native-wide. Above 1x they share one
 *     flat batch (checked); in windowed mode each batched line also keeps its
 *     GL_LINES vertices for the 1x authoritative surface, and the batches'
 *     native-wide mirrors wait in the window's queue (checked). The runner checks
 *     the window runs' native frame (lband) against the 1x run's, so the 1x
 *     surface draws exactly what 1x draws, in painter order, and their frame
 *     and wide surface at S against the full-VRAM run at the same scale.
 *   - mode "capture" (with or without the window): the frame-blend history and
 *     hold-last captures of the displayed frame. Outside the window mode they
 *     stay at the source scale (FRAME_W*S x FRAME_H*S); in the window mode they
 *     are taken at the presented letterbox size instead, each texel the source
 *     pixel under its centre (nearest filtering).
 *   - mode "mask" (with or without the window): a GP0(E6) mask-check change
 *     applies only to what is drawn after it. A line, a flat triangle and an
 *     opaque textured rect are each drawn across a mask-set rect with the
 *     check on (then off) or off (then on) before their batch is drawn: with
 *     the check on the rect's pixels stay, with it off they are overwritten,
 *     in the native VRAM and in the frame at S.
 *   - mode "twin" (with or without the window), argv[3] 0/1: [video]
 *     texture_window_batching off/on. Textured prims alternating GP0(E2h)
 *     texture windows (opaque, semi-transparent, set-mask, mask check,
 *     bilinear; inside the frame and into native-wide margins). The runner
 *     checks the native VRAM, the frame at S and the wide surface are the same
 *     with it off and on; this run checks that on never flushes for a window
 *     change except under mask check, and off flushes at every one. An oracle
 *     independent of the renderer then draws raw rects through each window
 *     into both page depths: every pixel must be the texel the PS1 window rule
 *     picks from the VRAM as read back, in the native VRAM and at S, so a
 *     window decode error both settings share still fails.
 *   - mode "passes" (built only where the renderer has render passes, the
 *     frame-rate stack; the runner defines PSX_TEST_RENDER_PASSES): with the
 *     flip-aware frame blend ready, the full-VRAM surface offers render passes
 *     and the window mode refuses them (PSX_MOD_RENDER_PASS_BACKEND), and
 *     gl_renderer_pass_begin opens nothing there: a pass backs up and restores
 *     only the authoritative surface, never the window's tiles.
 * Original source-owned scene; no retail payload. */
#include "gpu_gl_renderer.c"
#include "mod_texture_banks.c"
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a){(void)n;(void)a;return 0;}
uint32_t psx_mod_read_word(uint32_t a){(void)a;return 0;}
int g_psx_vram_dirty_tracking=0;
uint64_t s_frame_count=0;
void gpu_vram_dirty_mark_row_impl(uint32_t y){(void)y;}
void gpu_vram_dirty_mark_rect(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_vram_dirty_mark_all(void){}
int psx_netplay_active(void){return 0;}
int gpu_display_is_depth24(void){return 0;}
void gpu_get_display_info(GpuDisplayInfo *out){memset(out,0,sizeof(*out));out->width=320;out->height=240;}
int psx_ws_prim_in_backdrop(void){return 0;}
int gpu_ws_nw_flat_backdrop_enabled(void){return 0;}
int gpu_ws_background_requires_full_composite(void){return 0;}
void gpu_timeline_note(uint8_t kind,uint32_t a,uint32_t b){(void)kind;(void)a;(void)b;}
int g_ws_tex_edge_pct=0;
int psx_ws_prim_is_tagged(void){return 0;}
void psx_ws_dbg_gate_frame_snapshot(void){}
void gpu_depth24_upload_span_reset(void){}
/* Netplay unsplit view and forward-pass opt-in: off in these fixtures. */
int gpu_ws_netplay_local_viewport_width(void){return 0;}
int render_pass_netplay_enabled(void){return 0;}
/* Guest clock the renderer's stereo-pair freshness reads; test stand-ins, as
 * in test_gl_readback_region.c. */
uint64_t psx_cycle_count=0;
uint32_t g_psx_vblank_cycles=564480u;

static uint16_t vram[1024*512], peek[1024*512];
static int si_max_dim(void) { return s_gl_max_dim > 0 ? s_gl_max_dim : 1 << 30; }
static int checks, failures;
static void check(int ok, const char *label) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL %s\n", label); failures++; }
}

/* The "displayed frame": 320x240 at the VRAM origin, the window in "window"
 * mode. Lines live in LINE_* inside it, over a black fill. */
#define FRAME_W 320
#define FRAME_H 240
#define LINE_X0 200
#define LINE_Y0 100
#define LINE_X1 300
#define LINE_Y1 140

static uint64_t fnv(const void *pp, size_t n, uint64_t h) {
    const uint8_t *p = (const uint8_t *)pp;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

static void scene(void) {
    /* A 4-bit texture page at (512,0) with a 16-entry CLUT at (512,256). */
    static uint16_t page[64*64], clut[16], patch[16*8];
    for (int i = 0; i < 64*64; i++) page[i] = (uint16_t)((i * 0x1357u) ^ (i >> 3));
    for (int i = 0; i < 16; i++) clut[i] = (uint16_t)(i ? (0x0421u * (uint16_t)i) | ((i & 1) << 15) : 0);
    for (int i = 0; i < 16*8; i++) patch[i] = (uint16_t)(0x0C63 + i * 0x0101);
    glb_vram_transfer_in(512, 0, 64, 64, page);
    glb_vram_transfer_in(512, 256, 16, 1, clut);

    glb_set_draw_area(0, 0, 1023, 511);
    glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0);
    glb_set_semi_transparency(0, 0);
    glb_set_color_modulation(128, 128, 128, 0);

    glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0x1084);
    glb_draw_flat_rect(10, 10, 100, 60, 0x03e0);
    glb_draw_gouraud_triangle(20, 100, 0x001f, 180, 120, 0x7c00, 60, 230, 0x03ff);
    glb_draw_flat_triangle(200, 20, 310, 90, 230, 200, 0x5294);
    /* Textured rect (4-bit, CLUT at 512,256) and a textured triangle. */
    glb_draw_textured_rect(120, 12, 64, 48, 0, 0, 512, 256, 0x0008);
    glb_draw_shaded_textured_triangle(150, 140, 0, 0, 0x808080,
                                      300, 150, 63, 4, 0x6090b0,
                                      170, 235, 8, 63, 0xb09060, 512, 256, 0x0008, 0);
    /* Semi-transparency, every mode. */
    for (int mode = 0; mode < 4; mode++) {
        glb_set_semi_transparency(1, mode);
        glb_draw_flat_rect(20 + mode * 30, 180, 40, 30, (uint16_t)(0x2108 + mode * 0x0842));
    }
    glb_set_semi_transparency(0, 0);
    /* Mask set, then mask-check: the second rect must not overwrite masked px. */
    glb_set_mask_bits(1, 0);
    glb_draw_flat_rect(250, 150, 30, 30, 0x4210);
    glb_set_mask_bits(0, 1);
    glb_draw_flat_rect(240, 140, 50, 50, 0x7fff);
    glb_set_mask_bits(0, 0);
    /* Clipped primitive. */
    glb_set_draw_area(40, 40, 90, 90);
    glb_draw_flat_rect(0, 0, 200, 200, 0x3def);
    glb_set_draw_area(0, 0, 1023, 511);
    /* Copies: overlapping inside the frame; from an off-screen area in; and
     * one whose source straddles the frame's right edge (x 300..339). */
    glb_copy_rect(10, 10, 14, 13, 80, 50);
    glb_draw_flat_rect(400, 300, 32, 32, 0x7c1f);
    glb_copy_rect(400, 300, 280, 200, 32, 32);
    glb_draw_flat_rect(318, 150, 30, 20, 0x2d6b);
    glb_copy_rect(300, 150, 250, 160, 40, 20);
    /* A fill and an upload that cross the frame's edge. */
    glb_fill_rect(300, 220, 40, 10, 0x1234);
    glb_vram_transfer_in(312, 60, 16, 8, patch);
    /* A render-to-texture round trip: draw, then sample what was drawn. */
    glb_draw_flat_rect(576, 64, 32, 32, 0x03e0);
    glb_draw_textured_rect(200, 200, 32, 32, 0, 64, 512, 256, 0x0009);

    /* Lines over a black band. */
    glb_fill_rect(LINE_X0, LINE_Y0, LINE_X1 - LINE_X0, LINE_Y1 - LINE_Y0, 0);
    glb_draw_line(LINE_X0 + 5, LINE_Y0 + 10, LINE_X0 + 80, LINE_Y0 + 10, 0x7fff);   /* horizontal */
    glb_draw_line(LINE_X0 + 5, LINE_Y0 + 20, LINE_X0 + 45, LINE_Y0 + 35, 0x03ff);   /* diagonal */
    glb_draw_shaded_line(LINE_X0 + 90, LINE_Y0 + 2, 0x001f, LINE_X0 + 92, LINE_Y0 + 38, 0x7c00); /* steep */
}

/* ---- mode "sbs": side-by-side 512-wide double buffering ----------------- */
#define SBS_W 512
#define SBS_H 240
#define SBS_TPAGE 0x001C      /* 4-bit page at (768, 256) */
#define SBS_CLUT_X 768
#define SBS_CLUT_Y 496
#define SBS_LINE_Y0 200       /* line band per frame, masked in the native digest */
#define SBS_LINE_Y1 232
#define SBS_XBAND_Y0 480      /* a sloped primitive across both tiles */

/* S x S blocks of rows [y0, y1) (native) of a buffer image that are not one
 * colour: an image rendered at S has them along every sloped edge, an
 * upscaled 1x image has none. */
static long sbs_detail_blocks(const uint32_t *img, int ow, int scale, int y0, int y1) {
    long n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < SBS_W; x++) {
            const uint32_t *b = img + (size_t)y * scale * ow + (size_t)x * scale;
            uint32_t c = b[0] & 0xFFFFFFu;
            int same = 1;
            for (int j = 0; j < scale && same; j++)
                for (int i = 0; i < scale; i++)
                    if ((b[(size_t)j * ow + i] & 0xFFFFFFu) != c) { same = 0; break; }
            n += !same;
        }
    return n;
}

/* Present stand-in: the GL present asks the window for the displayed rect. */
static void sbs_show(int base) {
    if (s_hiw) check(hiw_ensure(base, base + SBS_W) != NULL, "displayed buffer held at S");
}

static void sbs_frame(int base, int k) {
    glb_set_draw_area(base, 0, base + SBS_W - 1, SBS_H - 1);
    glb_set_semi_transparency(0, 0);
    glb_set_mask_bits(0, 0);
    glb_fill_rect(base, 0, SBS_W, SBS_H, (uint16_t)(0x0c63 + k * 0x0421));
    glb_draw_gouraud_triangle(base + 20 + k * 7, 30, 0x001f, base + 480, 60 + k * 5, 0x7c00,
                              base + 100, 190, 0x03ff);
    glb_draw_flat_triangle(base + 300, 10, base + 505, 100 + k * 3, base + 350, 195, 0x5294);
    glb_draw_textured_rect(base + 200 + k, 110, 64, 48, 0, 0, SBS_CLUT_X, SBS_CLUT_Y, SBS_TPAGE);
    glb_draw_shaded_textured_triangle(base + 60, 120, 0, 0, 0x808080,
                                      base + 250, 135, 63, 4, 0x6090b0,
                                      base + 90, 195, 8, 63, 0xb09060,
                                      SBS_CLUT_X, SBS_CLUT_Y, SBS_TPAGE, 0);
    glb_set_semi_transparency(1, k & 3);
    glb_draw_flat_rect(base + 380, 140, 90, 50, (uint16_t)(0x2108 + k * 0x0842));
    glb_set_semi_transparency(0, 0);
    glb_set_mask_bits(1, 0);
    glb_draw_flat_rect(base + 30, 150, 30, 30, 0x4210);
    glb_set_mask_bits(0, 1);
    glb_draw_flat_rect(base + 20, 140, 50, 50, 0x7fff);
    glb_set_mask_bits(0, 0);
    /* Clipped by the draw area: reaches into the other buffer. */
    glb_draw_flat_rect(base + 470, 60, 100, 20, 0x3def);
    glb_fill_rect(base, SBS_LINE_Y0, SBS_W, SBS_LINE_Y1 - SBS_LINE_Y0, 0);
    glb_draw_line(base + 10, SBS_LINE_Y0 + 8, base + 500, SBS_LINE_Y0 + 20, 0x7fff);
    glb_draw_shaded_line(base + 400, SBS_LINE_Y0 + 2, 0x001f, base + 404, SBS_LINE_Y1 - 2, 0x7c00);
}

/* ---- mode "lines": lines batched with triangles --------------------------- */
static int lines_main(int scale, int window) {
    glb_set_draw_area(0, 0, 1023, 511);
    glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0);
    glb_set_semi_transparency(0, 0);
    if (window) check(hiw_ensure(0, FRAME_W) != NULL, "window covers the frame");
    glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0);
    glb_wide_configure(426, 53);
    glb_wide_set_target(0);
    glb_set_draw_area(0, 0, FRAME_W - 1, FRAME_H - 1);
    /* One batch: two lines, a triangle over parts of both, a steep shaded
     * line over the triangle, and a line into both margins. */
    glb_draw_line(10, 30, 200, 30, 0x7fff);
    glb_draw_line(20, 40, 180, 90, 0x03ff);
    glb_draw_flat_triangle(50, 25, 150, 25, 100, 80, 0x7c00);
    glb_draw_shaded_line(90, 22, 0x001f, 110, 110, 0x7c1f);
    glb_draw_line(-40, 60, 360, 70, 0x5ef7);
    if (scale > 1) {
        if (s_fb_n != 4 * 6 + 3 || s_fbl_n != (window ? 4 : 0))
            fprintf(stderr, "batch verts=%d lines=%d\n", s_fb_n, s_fbl_n);
        check(s_fb_n == 4 * 6 + 3, "lines and the triangle share one flat batch");
        check(s_fbl_n == (window ? 4 : 0), "a windowed line keeps its GL_LINES vertices");
    }
    /* A key change (semi-transparent), then lines around a triangle. */
    glb_set_semi_transparency(1, 1);
    glb_draw_line(30, 100, 290, 100, 0x7fff);
    glb_draw_flat_triangle(200, 50, 300, 60, 250, 115, 0x03e0);
    glb_draw_line(250, 40, 260, 118, 0x001f);
    glb_draw_shaded_line(240, 118, 0x7c00, 180, 20, 0x03e0);
    glb_set_semi_transparency(0, 0);
    glb_draw_line(300, 140, 400, 150, 0x7fff);
    glb_draw_flat_triangle(10, 150, 80, 150, 40, 200, 0x5294);
    glb_draw_line(5, 160, 90, 190, 0x7fff);
    glb_set_draw_area(0, 0, 1023, 511);
    {
        /* Windowed: the batches' native-wide mirrors wait in the window's
         * queue (replayed in one pass per surface at the next sync point). */
        int wq = 0;
        for (int i = 0; i < s_hq_n; i++) wq += s_hq[i].wfbo != 0;
        if (window && wq < 3) fprintf(stderr, "queued wide mirrors=%d\n", wq);
        check(window ? wq >= 3 : s_hq_n == 0, "windowed: native-wide mirrors queued");
    }
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    uint64_t lband = 0xcbf29ce484222325ull;   /* the native frame */
    for (int y = 0; y < FRAME_H; y++)
        lband = fnv(&peek[y * 1024], FRAME_W * sizeof peek[0], lband);
    int fw = FRAME_W * scale, fh = FRAME_H * scale, ow = 0, oh = 0;
    uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
    int n = img ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, img, fw * fh, &ow, &oh) : 0;
    check(n == fw * fh && ow == fw && oh == fh, "hires readback size");
    uint64_t hires = n ? fnv(img, (size_t)fw * fh * 4, 0xcbf29ce484222325ull) : 0;
    free(img);
    uint64_t wide = 0;
    {
        int ww = 426 * scale, wh = 512 * scale, gw = 0, gh = 0;
        uint32_t *wb = (uint32_t *)malloc((size_t)ww * wh * 4);
        int got = wb ? glb_wide_dump_full(wb, ww * wh, &gw, &gh, 0) : 0;
        check(got == ww * wh && gw == ww && gh == wh, "wide surface dump size");
        if (got) wide = fnv(wb, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        free(wb);
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("lband=%016llx\n", (unsigned long long)lband);
    printf("hires=%016llx\n", (unsigned long long)hires);
    printf("wide=%016llx\n", (unsigned long long)wide);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

/* ---- mode "capture": blend-history and hold-last capture size ------------ */
/* Read back texture `tex` when its storage is w x h (else -1), and count the
 * texels that are not the source pixel under their centre (nearest), the
 * source being sw x sh RGBA8. */
static long capture_mismatches(const uint8_t *cap, int w, int h,
                               const uint8_t *src, int sw, int sh);
static long capture_check(GLuint tex, uint8_t *cap, int w, int h,
                          const uint8_t *src, int sw, int sh) {
    GLint tw = 0, th = 0;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    if (tw != w || th != h) { glBindTexture(GL_TEXTURE_2D, 0); return -1; }
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, cap);
    glBindTexture(GL_TEXTURE_2D, 0);
    return capture_mismatches(cap, w, h, src, sw, sh);
}
static long capture_mismatches(const uint8_t *cap, int w, int h,
                               const uint8_t *src, int sw, int sh) {
    long bad = 0;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = (int)(((double)i + 0.5) * sw / w), sy = (int)(((double)j + 0.5) * sh / h);
            if (memcmp(cap + ((size_t)j * w + i) * 4, src + ((size_t)sy * sw + sx) * 4, 3)) bad++;
        }
    return bad;
}

/* Mode "mask": see the header. Row MASK_ROW crosses the mask-set rect
 * (MASK_X0.., MASK_W wide); every primitive covers that whole stretch. */
#define MASK_X0 100
#define MASK_W 40
#define MASK_ROW 120
static int mask_main(int scale, int window) {
    static uint16_t page[64 * 64];          /* 15-bit texels, white, STP 0 */
    for (int i = 0; i < 64 * 64; i++) page[i] = 0x7fff;
    glb_vram_transfer_in(512, 0, 64, 64, page);
    glb_set_draw_area(0, 0, 1023, 511);
    glb_set_draw_offset(0, 0);
    glb_set_semi_transparency(0, 0);
    glb_set_color_modulation(128, 128, 128, 0);
    if (window) check(hiw_ensure(0, FRAME_W) != NULL, "window covers the frame");
    static const char *const kinds[] = { "line", "triangle", "textured" };
    int fw = FRAME_W * scale, fh = FRAME_H * scale;
    uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
    for (int k = 0; k < 3; k++) {
        for (int on = 1; on >= 0; on--) {
            glb_set_mask_bits(0, 0);
            glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0);
            glb_set_mask_bits(1, 0);                       /* the rect sets bit 15 */
            glb_draw_flat_rect(MASK_X0, MASK_ROW - 20, MASK_W, MASK_W, 0x001f);
            glb_set_mask_bits(0, on);
            if (k == 0) glb_draw_line(MASK_X0 - 10, MASK_ROW, MASK_X0 + MASK_W + 10, MASK_ROW, 0x7fff);
            else if (k == 1) glb_draw_flat_triangle(80, MASK_ROW - 10, 200, MASK_ROW - 10,
                                                    80, MASK_ROW + 40, 0x7fff);
            else glb_draw_textured_rect(MASK_X0 - 10, MASK_ROW - 2, MASK_W + 20, 5,
                                        0, 0, 0, 0, 0x0108);
            glb_set_mask_bits(0, !on);                     /* E6 before the batch draws */
            gl_renderer_sync_cpu();
            check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
            int kept = 0, drawn = 0;
            for (int x = MASK_X0; x < MASK_X0 + MASK_W; x++) {
                uint16_t p = peek[MASK_ROW * 1024 + x] & 0x7fff;
                kept += p == 0x001f;
                drawn += p == 0x7fff;
            }
            int ow = 0, oh = 0, hkept = 0, hdrawn = 0;
            int n = img ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, img,
                                                         fw * fh, &ow, &oh) : 0;
            check(n == fw * fh && ow == fw && oh == fh, "hires readback size");
            if (n) {
                const uint32_t *row = img + (size_t)(MASK_ROW * scale + scale / 2) * ow;
                for (int x = MASK_X0 * scale; x < (MASK_X0 + MASK_W) * scale; x++) {
                    uint32_t c = row[x] & 0xFFFFFFu;   /* red rect; white (texels at 8 bits) */
                    hkept += c == 0xF80000u;
                    hdrawn += c == 0xF8F8F8u || c == 0xFFFFFFu;
                }
            }
            printf("mask %s check-%s: native kept=%d drawn=%d, at S kept=%d drawn=%d\n",
                   kinds[k], on ? "on" : "off", kept, drawn, hkept, hdrawn);
            int want_kept = on ? MASK_W : 0, want_drawn = on ? 0 : MASK_W;
            char label[96];
            snprintf(label, sizeof label, "mask %s check-%s: native VRAM", kinds[k], on ? "on" : "off");
            check(kept == want_kept && drawn == want_drawn, label);
            snprintf(label, sizeof label, "mask %s check-%s: frame at S", kinds[k], on ? "on" : "off");
            check(hkept == want_kept * scale && hdrawn == want_drawn * scale, label);
        }
    }
    free(img);
    glb_set_mask_bits(0, 0);
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

/* ---- mode "twin": texture-window batching --------------------------------
 * argv[3] "0"/"1" = [video] texture_window_batching off/on. Textured prims that
 * alternate GP0(E2h) windows: opaque polys and rects inside the frame and into
 * the native-wide margins, semi-transparent ones, a run under set-mask, a run
 * under mask check (which must still split at every window change) and a
 * bilinear run. The runner compares the native VRAM, the frame at S and the
 * wide surface of the two runs; this run checks the window flushes and the
 * window oracle (twin_oracle). */
#define TWIN_TPAGE4  0x0008   /* 4-bit page at (512, 0), CLUT at (512, 256) */
#define TWIN_TPAGE15 0x0109   /* 15-bit page at (576, 0) */
static const uint32_t twin_windows[4] = {
    0,                                          /* no window */
    0x1Cu,                                      /* u repeats every 32 */
    0x18u | (0x1Eu << 5) | (0x08u << 10) | (0x02u << 15),
    0x1Fu | (0x1Fu << 5) | (0x05u << 10) | (0x0Au << 15),   /* one 8x8 tile */
};
static void twin_quad(int i, int x, int y, int w, int h, uint16_t tp) {
    glb_set_texture_window(twin_windows[i & 3]);
    if (i % 3 == 2) {
        glb_draw_textured_rect(x, y, w, h, 3 + i, 5 + 2 * i, 512, 256, tp);
        return;
    }
    int u0 = 2 * i, v0 = i, u1 = u0 + 90 + i, v1 = v0 + 70;
    glb_draw_shaded_textured_triangle(x, y, u0, v0, 0x808080, x + w, y + 3, u1, v0, 0x6090b0,
                                      x, y + h, u0, v1, 0xb09060, 512, 256, tp, 0);
    glb_draw_shaded_textured_triangle(x + w, y + 3, u1, v0, 0x6090b0, x, y + h, u0, v1, 0xb09060,
                                      x + w - 7, y + h + 2, u1, v1, 0x80a080, 512, 256, tp, 0);
}
/* The window oracle: what the GP0(E2h) window must sample, from the PS1 rule
 * and the VRAM as read back, so the off/on comparison cannot hide a decode
 * error both modes share. Raw opaque rects (no dither, one texel per native
 * pixel), one per window and page, drawn back to back so they share batches
 * when the key is on. Every pixel is checked in the native VRAM and, at its
 * centre, in the frame at S. Counts the pixels each window drew. */
#define TWIN_OR_W 70
#define TWIN_OR_H 20
static uint16_t twin_texel(const uint16_t *v, uint16_t tp, int u, int t) {
    int px = (tp & 15) * 64, py = ((tp >> 4) & 1) * 256;
    if (((tp >> 7) & 3) == 2) return v[((py + t) & 511) * 1024 + ((px + u) & 1023)];
    int idx = (v[((py + t) & 511) * 1024 + ((px + (u >> 2)) & 1023)] >> ((u & 3) * 4)) & 15;
    return v[256 * 1024 + 512 + idx];                 /* CLUT at (512, 256) */
}
static void twin_oracle(int scale, int drawn[4]) {
    /* Texels for the windows that reach past the scene's 64x64 pages. */
    static uint16_t more[64 * 128];
    for (int i = 0; i < 64 * 128; i++)
        more[i] = (uint16_t)(((i * 0x1d0bu) ^ (i >> 4)) | ((i & 7) == 5 ? 0x8000u : 0));
    glb_vram_transfer_in(512, 64, 64, 64, more);              /* 4-bit rows 64.. */
    glb_vram_transfer_in(576, 64, 64, 64, more + 64 * 64);    /* 15-bit rows 64.. */
    glb_vram_transfer_in(640, 0, 64, 128, more);              /* 15-bit u 64..127 */
    glb_set_draw_area(0, 0, FRAME_W - 1, FRAME_H - 1);
    glb_set_mask_bits(0, 0);
    glb_set_semi_transparency(0, 0);
    glb_set_texture_filter(0);
    glb_set_color_modulation(128, 128, 128, 1);           /* raw: the texel as is */
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, vram), "oracle: VRAM before");
    int fw = FRAME_W * scale, fh = FRAME_H * scale, ow = 0, oh = 0;
    uint32_t *before = (uint32_t *)malloc((size_t)fw * fh * 4);
    uint32_t *after = (uint32_t *)malloc((size_t)fw * fh * 4);
    int nb = before ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, before, fw * fh, &ow, &oh) : 0;
    check(nb == fw * fh, "oracle: frame at S before");
    for (int i = 0; i < 8; i++) {
        glb_set_texture_window(twin_windows[i & 3]);
        glb_draw_textured_rect(4 + (i & 3) * 78, 2 + (i >> 2) * 22, TWIN_OR_W, TWIN_OR_H,
                               9 * i + 3, 5 * i + 1, 512, 256, i < 4 ? TWIN_TPAGE4 : TWIN_TPAGE15);
    }
    glb_set_texture_window(0);
    glb_set_color_modulation(128, 128, 128, 0);
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "oracle: native peek");
    int na = after ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, after, fw * fh, &ow, &oh) : 0;
    check(na == fw * fh, "oracle: frame at S after");
    long bad = 0, bad_hi = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t w = twin_windows[i & 3];
        int mx = w & 31, my = (w >> 5) & 31, ox = (w >> 10) & 31, oy = (w >> 15) & 31;
        int x0 = 4 + (i & 3) * 78, y0 = 2 + (i >> 2) * 22;
        for (int dy = 0; dy < TWIN_OR_H; dy++)
            for (int dx = 0; dx < TWIN_OR_W; dx++) {
                int u = (9 * i + 3 + dx) & 255, t = (5 * i + 1 + dy) & 255;
                u = (u & ~(mx * 8)) | ((ox & mx) * 8);
                t = (t & ~(my * 8)) | ((oy & my) * 8);
                uint16_t texel = twin_texel(vram, i < 4 ? TWIN_TPAGE4 : TWIN_TPAGE15, u, t);
                int x = x0 + dx, y = y0 + dy;
                uint16_t want = texel ? texel : vram[y * 1024 + x];
                if (texel) drawn[i & 3]++;
                if (peek[y * 1024 + x] != want) {
                    if (!bad) fprintf(stderr, "oracle: window %d page %d px (%d,%d) native %04x want %04x\n",
                                      i & 3, i >> 2, x, y, peek[y * 1024 + x], want);
                    bad++;
                }
                if (nb == fw * fh && na == fw * fh) {
                    size_t c = (size_t)(y * scale + scale / 2) * fw + (size_t)(x * scale + scale / 2);
                    uint32_t got = after[c] & 0xFFFFFFu;     /* BGRA: R bits 16.., B bits 0.. */
                    int ok = texel ? (((got >> 19) & 31) == (texel & 31) &&
                                      ((got >> 11) & 31) == ((texel >> 5) & 31) &&
                                      ((got >> 3) & 31) == ((texel >> 10) & 31))
                                   : got == (before[c] & 0xFFFFFFu);
                    bad_hi += !ok;
                }
            }
    }
    free(before);
    free(after);
    printf("twin_oracle drawn=%d,%d,%d,%d native_bad=%ld hires_bad=%ld\n",
           drawn[0], drawn[1], drawn[2], drawn[3], bad, bad_hi);
    check(bad == 0, "oracle: native VRAM is what the texture window samples");
    check(bad_hi == 0, "oracle: frame at S is what the texture window samples");
    for (int k = 0; k < 4; k++) check(drawn[k] > TWIN_OR_W * TWIN_OR_H / 2, "oracle: every window draws");
}
static int twin_main(int scale, int window, int on) {
    static uint16_t page[64 * 64], page15[64 * 64], clut[16];
    for (int i = 0; i < 64 * 64; i++) page[i] = (uint16_t)((i * 0x2469u) ^ (i >> 2) ^ 0x1111u);
    for (int i = 0; i < 64 * 64; i++)
        page15[i] = (uint16_t)(((i * 0x0843u) & 0x7fffu) | (((i >> 5) & 3) == 1 ? 0x8000u : 0));
    for (int i = 0; i < 16; i++) clut[i] = (uint16_t)(i ? (0x0421u * (uint16_t)i) | ((i & 3) == 3 ? 0x8000u : 0) : 0);
    glb_vram_transfer_in(512, 0, 64, 64, page);
    glb_vram_transfer_in(576, 0, 64, 64, page15);
    glb_vram_transfer_in(512, 256, 16, 1, clut);
    gl_renderer_set_texture_window_batching(on);
    check(gl_renderer_get_texture_window_batching() == on, "texture-window batching set");
    glb_set_draw_area(0, 0, 1023, 511);
    glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0);
    glb_set_semi_transparency(0, 0);
    glb_set_color_modulation(128, 128, 128, 0);
    if (window) check(hiw_ensure(0, FRAME_W) != NULL, "window covers the frame");
    glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0x0c63);
    glb_wide_configure(426, 53);
    glb_wide_set_target(0);
    glb_set_draw_area(0, 0, FRAME_W - 1, FRAME_H - 1);
    uint64_t r5 = s_batch_reason[5], total = s_batch_total;
    /* A: 24 opaque prims, a new window each (23 changes); 4-bit and 15-bit,
     * some reaching the margins. */
    for (int i = 0; i < 24; i++)
        twin_quad(i, -40 + (i % 6) * 66, 8 + (i / 6) * 40, 70 + (i & 1) * 20, 44,
                  (i & 4) ? TWIN_TPAGE15 : TWIN_TPAGE4);
    /* B: semi-transparent, every mode (drawn one per batch either way). */
    for (int i = 0; i < 4; i++) {
        glb_set_semi_transparency(1, i);
        twin_quad(i + 1, 20 + i * 70, 150, 80, 40, TWIN_TPAGE15);
    }
    glb_set_semi_transparency(0, 0);
    /* C1: set-mask, 4 prims (3 changes). */
    glb_set_mask_bits(1, 0);
    for (int i = 0; i < 4; i++) twin_quad(i, 30 + i * 60, 180, 50, 30, TWIN_TPAGE4);
    /* C2: mask check, 6 overlapping prims (5 changes; split in both modes). */
    glb_set_mask_bits(0, 1);
    for (int i = 0; i < 6; i++) twin_quad(i + 2, 10 + i * 40, 170, 90, 50, TWIN_TPAGE15);
    glb_set_mask_bits(0, 0);
    /* D: bilinear, 4 prims (3 changes). */
    glb_set_texture_filter(1);
    for (int i = 0; i < 4; i++) twin_quad(i + 3, -20 + i * 95, 205, 90, 30, TWIN_TPAGE4);
    flush_tex_batch();
    glb_set_texture_filter(0);
    glb_set_texture_window(0);
    uint64_t flushes = s_batch_reason[5] - r5, batches = s_batch_total - total;
    printf("twin_flushes=%llu batches=%llu\n", (unsigned long long)flushes,
           (unsigned long long)batches);
    check(flushes == (on ? 5u : 34u), on ? "batching on: window flushes only under mask check"
                                         : "batching off: every window change flushes");
    int drawn[4] = {0, 0, 0, 0};
    twin_oracle(scale, drawn);
    glb_set_draw_area(0, 0, 1023, 511);
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    uint64_t digest = fnv(peek, sizeof peek, 0xcbf29ce484222325ull);
    int fw = FRAME_W * scale, fh = FRAME_H * scale, ow = 0, oh = 0;
    uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
    int n = img ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, img, fw * fh, &ow, &oh) : 0;
    check(n == fw * fh && ow == fw && oh == fh, "hires readback size");
    uint64_t hires = n ? fnv(img, (size_t)fw * fh * 4, 0xcbf29ce484222325ull) : 0;
    free(img);
    uint64_t wide = 0;
    {
        int ww = 426 * scale, wh = 512 * scale, gw = 0, gh = 0;
        uint32_t *wb = (uint32_t *)malloc((size_t)ww * wh * 4);
        int got = wb ? glb_wide_dump_full(wb, ww * wh, &gw, &gh, 0) : 0;
        check(got == ww * wh && gw == ww && gh == wh, "wide surface dump size");
        if (got) wide = fnv(wb, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        free(wb);
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("digest=%016llx\n", (unsigned long long)digest);
    printf("hires=%016llx\n", (unsigned long long)hires);
    printf("wide=%016llx\n", (unsigned long long)wide);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

#if defined(PSX_TEST_RENDER_PASSES)
/* Mode "passes": see the header. */
static int passes_main(int scale, int window) {
    (void)scale;
    gl_renderer_set_interpolation(1, 120.0, 120.0, 60.0, 2);
    gl_renderer_set_interpolation_source(1);
    s_interp_valid = 2;                    /* as after the first presented frame */
    uint32_t why = gl_renderer_pass_unavailable();
    printf("passes: unavailable=%u\n", (unsigned)why);
    if (window) {
        check(why == PSX_MOD_RENDER_PASS_BACKEND, "window mode refuses render passes");
        check(!gl_renderer_pass_begin(0, 0, FRAME_W, FRAME_H, 1, 1, 0) && !s_pass_active,
              "window mode: pass_begin opens nothing");
    } else {
        check(why == PSX_MOD_RENDER_PASS_READY, "full-VRAM surface offers render passes");
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
#endif

static int capture_main(int scale, int window) {
    GLuint fbo = s_hr_fbo;
    int sx = 0;
    if (window) {
        const HiwTile *T = hiw_ensure(0, FRAME_W);
        check(T != NULL, "window covers the frame");
        if (!T) return 1;
        fbo = T->fbo; sx = -T->x0;
    }
    scene();
    flush_flat_batch();
    flush_tex_batch();
    flush_cpu_upload();
    hiw_flush_queue();
    int S = s_out_scale, sw = FRAME_W * S, sh = FRAME_H * S;
    int ww = 0, wh = 0, lx, ly, lw, lh, cw = sw, ch = sh;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    letterbox_rect_aspect(ww, wh, 4, 3, &lx, &ly, &lw, &lh);
    if (window && (long)lw * lh < (long)sw * sh) { cw = lw; ch = lh; }
    uint8_t *src = (uint8_t *)malloc((size_t)sw * sh * 4);
    uint8_t *cap = (uint8_t *)malloc((size_t)cw * ch * 4);
    if (!src || !cap) { free(src); free(cap); check(0, "capture buffers"); return 1; }
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(sx * S, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, src);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);

    /* Blend history: nearest filtering (linear = 0), a 4:3 present. */
    s_interp_enabled = 1;
    s_interp_suspended = 0;
    check(interp_capture(fbo, sx, 0, FRAME_W, FRAME_H, 0, 1, GL_PRES_VRAM,
                         0, 0, 1) == 1,
          "blend history captured");
    if (s_interp_w != cw || s_interp_h != ch)
        fprintf(stderr, "blend capture %dx%d want %dx%d (source %dx%d)\n",
                s_interp_w, s_interp_h, cw, ch, sw, sh);
    check(s_interp_w == cw && s_interp_h == ch, "blend capture size");
    check(s_interp_src_w == sw && s_interp_src_h == sh, "blend source band at the output scale");
    long bad = capture_check(s_interp_tex[s_interp_cur], cap, cw, ch, src, sw, sh);
    if (bad) fprintf(stderr, "blend capture: %ld of %d texels differ (-1: storage size)\n",
                     bad, cw * ch);
    check(bad >= 0 && bad * 1000 <= (long)cw * ch, "blend capture holds the source (nearest)");

    /* Hold-last snapshot of the same band. */
    hold_capture_native_fbo(fbo, sx, 0, FRAME_W, FRAME_H, 1, 0);
    check(s_hold_tw == cw && s_hold_th == ch, "hold-last capture size");
    bad = capture_check(s_hold_tex, cap, cw, ch, src, sw, sh);
    if (bad) fprintf(stderr, "hold capture: %ld of %d texels differ (-1: storage size)\n",
                     bad, cw * ch);
    check(bad >= 0 && bad * 1000 <= (long)cw * ch, "hold-last capture holds the source (nearest)");
    free(src);
    free(cap);
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("capture=%dx%d source=%dx%d\n", cw, ch, sw, sh);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

static int sbs_main(int scale) {
    static uint16_t page[64*64], clut[16], patch[40*6];
    for (int i = 0; i < 64*64; i++) page[i] = (uint16_t)((i * 0x2469u) ^ (i >> 2));
    for (int i = 0; i < 16; i++) clut[i] = (uint16_t)(i ? (0x0842u * (uint16_t)i) | ((i & 2) << 14) : 0);
    for (int i = 0; i < 40*6; i++) patch[i] = (uint16_t)(0x1ce7 + i * 0x0103);
    glb_vram_transfer_in(768, 256, 64, 64, page);
    glb_vram_transfer_in(SBS_CLUT_X, SBS_CLUT_Y, 16, 1, clut);
    glb_set_draw_offset(0, 0);
    glb_set_color_modulation(128, 128, 128, 0);
    /* Draw the back buffer, flip; four frames, so each buffer is last drawn
     * while both are shown (held) at S. */
    for (int k = 0; k < 4; k++) {
        int back = (k & 1) ? 0 : SBS_W;
        sbs_frame(back, k);
        sbs_show(back);
    }
    /* Front to back and back to front (across the tiles), and a fill and an
     * upload that cross x=512. */
    glb_set_draw_area(0, 0, 1023, 511);
    glb_copy_rect(40, 40, 552, 50, 200, 100);
    glb_copy_rect(900, 100, 400, 120, 124, 50);
    glb_fill_rect(490, 225, 44, 10, 0x1234);
    glb_vram_transfer_in(492, 236, 40, 6, patch);
    /* Rows 400..470 across all of VRAM (each triangle inside one buffer, a
     * rect across x=512), then copies 1000 px wide: past the columns one
     * GPU-limit-wide staging can hold at 16x and up. One moves right to left
     * over itself (shift -6, overlapping rows too). */
    glb_draw_gouraud_triangle(0, 400, 0x7c1f, 511, 404, 0x03e0, 256, 470, 0x001f);
    glb_draw_gouraud_triangle(512, 402, 0x03e0, 1023, 400, 0x7c1f, 700, 468, 0x7fff);
    glb_draw_flat_triangle(0, 470, 511, 430, 200, 405, 0x2d6b);
    glb_draw_flat_triangle(512, 405, 1023, 470, 600, 440, 0x5ad6);
    glb_draw_flat_rect(300, 404, 500, 30, 0x1f3c);
    glb_copy_rect(8, 400, 0, 420, 1000, 16);
    glb_copy_rect(0, 440, 6, 444, 1000, 20);
    /* The band across both tiles (see the header). */
    glb_draw_gouraud_triangle(0, 482, 0x7c00, 1023, 486, 0x001f, 512, 509, 0x03e0);
    sbs_show(0);
    sbs_show(SBS_W);
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    for (int y = SBS_LINE_Y0; y < SBS_LINE_Y1; y++)
        for (int x = 0; x < 1024; x++) peek[y * 1024 + x] = 0;
    uint64_t digest = fnv(peek, sizeof peek, 0xcbf29ce484222325ull);
    uint64_t hi[2] = { 0, 0 };
    for (int b = 0; b < 2; b++) {
        int fw = SBS_W * scale, fh = 512 * scale, ow = 0, oh = 0;
        uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
        int n = img ? gl_renderer_read_display_hires(b * SBS_W, 0, SBS_W, 512, img, fw * fh,
                                                     &ow, &oh) : 0;
        check(n == fw * fh && ow == fw && oh == fh, "buffer readback at internal resolution");
        /* Line bands: drawn as quads above 1x in both surfaces, compared too.
         * Top row first; the cross-tile band is left out. */
        if (n) hi[b] = fnv(img, (size_t)fw * SBS_XBAND_Y0 * scale * 4, 0xcbf29ce484222325ull);
        if (n && scale > 1) {
            long frame = sbs_detail_blocks(img, ow, scale, 0, SBS_H);
            long band = sbs_detail_blocks(img, ow, scale, SBS_XBAND_Y0, 512);
            if (frame < 500 || band < 100)
                fprintf(stderr, "buffer %d detail blocks frame=%ld band=%ld\n", b, frame, band);
            check(frame >= 500, "buffer rendered at S (not an upscaled 1x image)");
            check(band >= 100, "cross-tile band rendered at S");
        }
        free(img);
    }
    /* Staging textures: recorded size == real storage, within the limit. */
    GLint tw = 0, th = 0;
    p_glActiveTexture(PSXGL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    check(tw == s_scratch_w && th == s_scratch_h, "scratch size recorded == storage");
    if (s_hiw) {
        glBindTexture(GL_TEXTURE_2D, s_hiw_scratch_tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        check(tw == s_hiw_scratch_w && th == s_hiw_scratch_h,
              "window copy scratch size recorded == storage");
        check(s_hiw_scratch_w <= si_max_dim(), "window copy scratch within the GPU limit");
        check(!s_hiw_scratch_refused_logged, "no window copy staging refused");
    }
    check(!s_scratch_refused_logged, "no scratch growth refused");
    if (s_hiw) {
        /* One surface while the union of both buffers fits the limit, else
         * one tile per buffer. */
        int want = (long)1024 * scale <= si_max_dim() ? 1 : 2;
        if (s_hiw_n != want) fprintf(stderr, "tiles=%d want %d\n", s_hiw_n, want);
        check(s_hiw_n == want, "tile count for the layout");
    }
    /* A scratch request past the GPU limit is refused, logged and harmless. */
    {
        int w0 = s_scratch_w, h0 = s_scratch_h;
        check(!scratch_ensure(si_max_dim() + 1, 8), "over-limit scratch refused");
        check(s_scratch_w == w0 && s_scratch_h == h0, "refused scratch keeps its size");
        check(s_scratch_refused_logged == 1, "refusal logged");
        check(scratch_ensure(w0, h0 + 8) && s_scratch_h == h0 + 8, "scratch still grows");
        glBindTexture(GL_TEXTURE_2D, s_scratch_tex);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
        check(tw == s_scratch_w && th == s_scratch_h, "scratch storage after refusal");
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("tiles=%d\n", s_hiw_n);
    printf("digest=%016llx\n", (unsigned long long)digest);
    printf("hires=%016llx\n", (unsigned long long)hi[0]);
    printf("wide=%016llx\n", (unsigned long long)hi[1]);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    int scale = argc > 1 ? atoi(argv[1]) : 1;
    const char *mode = argc > 2 ? argv[2] : "scene";
    int window = !strcmp(mode, "window");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Scale invariance hidden test", 0, 0, 128, 128,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) return 2;
    for (int i = 0; i < 1024*512; i++) vram[i] = 0;
    glb_init(vram);
    glb_set_scale(scale);
    gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "FAIL context\n"); return 2; }
    GlScaleInfo si;
    gl_renderer_scale_info(&si);
    printf("driver=%s max_dim=%d requested=%d effective=%d windowed=%d hr_scale=%d\n",
           (const char *)glGetString(GL_VERSION), si.max_dim, si.requested, si.effective,
           si.windowed, si.hr_scale);

    if (!strcmp(mode, "clamp")) {
        /* Whatever was requested, the backend stays GL and never allocates
         * past the driver limit. With the window allowed (default) a request
         * past the full-VRAM limit keeps its scale for the displayed area. */
        int full = psx_gl_clamp_full_vram_scale(scale, GL_MAX_INTERNAL_SCALE, si.max_dim,
                                                psx_gl_budget_bytes_from_env(getenv("PSX_GL_VRAM_BUDGET_MB")),
                                                NULL);
        check(s_raster_ok == 1, "GL pipeline kept (no software fallback)");
        if (si.windowed) {
            check(si.hr_scale == 1, "windowed: authoritative surface at 1x");
            check(si.effective > full && (long)si.effective * 512 <= si.max_dim,
                  "windowed: effective scale above the full-VRAM clamp, rows within the limit");
        } else {
            check(si.effective == full, "effective scale is the limit-clamped request");
        }
        check(psx_gl_full_vram_fits(si.hr_scale, si.max_dim, 0), "hr surface within driver limit");
        check(glGetError() == GL_NO_ERROR, "GL error");
        printf("checks=%d failures=%d\n", checks, failures);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return failures ? 1 : 0;
    }

    check(si.effective == scale, "requested scale allocated");
    if (!strcmp(mode, "sbs")) {
        int rc = sbs_main(scale);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (!strcmp(mode, "capture")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = capture_main(scale, si.windowed);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (!strcmp(mode, "lines")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = lines_main(scale, si.windowed);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
#if defined(PSX_TEST_RENDER_PASSES)
    if (!strcmp(mode, "passes")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = passes_main(scale, si.windowed);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
#endif
    if (!strcmp(mode, "twin")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = twin_main(scale, si.windowed, argc > 3 && argv[3][0] == '1');
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (!strcmp(mode, "mask")) {
        if (si.windowed) check(si.hr_scale == 1, "window mode engaged");
        int rc = mask_main(scale, si.windowed);
        gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }
    if (window) {
        check(si.windowed && si.hr_scale == 1, "window mode engaged");
        check(hiw_ensure(0, FRAME_W) != NULL, "window covers the frame");
    }
    scene();
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, 1024, 512, peek), "native peek");
    /* The mask-checked white rect kept the masked rect under it (the part
     * the later copy does not cover). */
    check(peek[152 * 1024 + 255] == 0xC210 && peek[185 * 1024 + 285] == 0x7fff,
          "mask check kept the masked rect");
    /* Digest of guest-visible VRAM, the line band masked out. */
    for (int y = LINE_Y0; y < LINE_Y1; y++)
        for (int x = LINE_X0; x < LINE_X1; x++) peek[y * 1024 + x] = 0;
    uint64_t digest = fnv(peek, sizeof peek, 0xcbf29ce484222325ull);

    /* The frame at internal resolution. */
    int fw = FRAME_W * scale, fh = FRAME_H * scale;
    uint32_t *img = (uint32_t *)malloc((size_t)fw * fh * 4);
    int ow = 0, oh = 0;
    int n = img ? gl_renderer_read_display_hires(0, 0, FRAME_W, FRAME_H, img, fw * fh, &ow, &oh) : 0;
    check(n == fw * fh && ow == fw && oh == fh, "hires readback size");
    if (n) {
        /* Line thickness: the horizontal line covers S rows at x=230; the
         * steep line S columns at y=120. */
        int col = 230 * scale + scale / 2, rows = 0;
        for (int yy = LINE_Y0 * scale; yy < LINE_Y1 * scale; yy++)
            if ((img[yy * ow + col] & 0xFFFFFFu) == 0xF8F8F8u) rows++;
        if (rows != scale) fprintf(stderr, "horizontal line rows=%d scale=%d\n", rows, scale);
        check(rows == scale, "horizontal line is one native pixel thick");
        int row = 120 * scale + scale / 2, cols = 0;
        for (int xx = 285 * scale; xx < 298 * scale; xx++)
            if ((img[row * ow + xx] & 0xFFFFFFu) != 0) cols++;
        if (cols != scale) fprintf(stderr, "steep line cols=%d scale=%d\n", cols, scale);
        check(cols == scale, "steep line is one native pixel thick");
    }
    uint64_t hires = n ? fnv(img, (size_t)fw * fh * 4, 0xcbf29ce484222325ull) : 0;
    free(img);

    /* Native-wide surface (16:9 on the 320 frame: 426 wide, 53 each side) at
     * the same scale: margin-reaching draws are mirrored into it and its
     * centre comes from the frame (the window in "window" mode). */
    glb_wide_configure(426, 53);
    glb_wide_set_target(0);
    glb_set_draw_area(0, 0, 319, 239);
    glb_draw_flat_rect(-40, 20, 90, 30, 0x5ad6);                      /* into the left margin */
    glb_draw_gouraud_triangle(250, 60, 0x001f, 372, 90, 0x7c00, 280, 150, 0x03e0); /* right */
    glb_draw_textured_rect(300, 160, 64, 40, 0, 0, 512, 256, 0x0008);  /* textured, right */
    glb_draw_line(-30, 225, 350, 225, 0x7fff);                         /* line across both */
    glb_set_draw_area(0, 0, 1023, 511);
    uint64_t wide = 0;
    {
        int ww = 426 * scale, wh = 512 * scale, gw = 0, gh = 0;
        uint32_t *wb = (uint32_t *)malloc((size_t)ww * wh * 4);
        int got = wb ? glb_wide_dump_full(wb, ww * wh, &gw, &gh, 0) : 0;
        check(got == ww * wh && gw == ww && gh == wh, "wide surface dump size");
        if (got) wide = fnv(wb, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        free(wb);
    }
    check(glGetError() == GL_NO_ERROR, "GL error");
    printf("digest=%016llx\n", (unsigned long long)digest);
    printf("hires=%016llx\n", (unsigned long long)hires);
    printf("wide=%016llx\n", (unsigned long long)wide);
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown();
    SDL_DestroyWindow(win);
    SDL_Quit();
    return failures ? 1 : 0;
}
