/* [video] fmv_chroma_smoothing on a real, hidden OpenGL context.
 *
 * A synthetic 4:2:0-style 24-bit frame (constant luma, chroma constant per
 * 2x2 block and flipping between saturated hues block to block, plus a sharp
 * luma edge on the right) is presented through gl_renderer_present, the
 * CPU/24-bit present path:
 *  - off, and on for a frame that is not a 24-bit scanout, are byte-identical;
 *  - on, chroma steps between neighbouring output pixels shrink a lot while
 *    luma stays (the flat-luma area keeps its luma within a few levels, and
 *    the sharp luma edge stays as sharp);
 *  - the smoothed-present counter counts only 24-bit presents.
 * Original source-owned scene; no retail payload. */
#define GL_PRESENT_TEST_HOOK(gen) cap_hook(gen)
static void cap_hook(int gen);
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
void gpu_hd_textures_begin_upload(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t* p){(void)x;(void)y;(void)w;(void)h;(void)p;}
void gpu_hd_textures_invalidate(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_copy(int sx,int sy,int x,int y,int w,int h){(void)sx;(void)sy;(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_end_copy(void){}
#ifndef PSX_NO_DEBUG_TOOLS
int present_image_ring_accepting(void){return 0;}
int present_image_ring_thumb_w(int w,int h){(void)w;(void)h;return 0;}
void present_image_ring_push(uint32_t f,const uint8_t* p,int w,int pitch,int flip){(void)f;(void)p;(void)w;(void)pitch;(void)flip;}
void present_image_ring_push_argb(uint32_t f,const uint32_t* p,int w,int h,int pitch){(void)f;(void)p;(void)w;(void)h;(void)pitch;}
#endif
void present_shot_done(int ok) { (void)ok; }
int host_osd_needs_present(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
void psx_host_sleep_micros(unsigned us) { SDL_Delay((us + 999u) / 1000u); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }

static uint8_t *s_cap; static int s_cap_w, s_cap_h, s_cap_n;
static void cap_hook(int gen) {
    (void)gen;
    SDL_GL_GetDrawableSize(s_win, &s_cap_w, &s_cap_h);
    free(s_cap);
    s_cap = (uint8_t *)malloc((size_t)s_cap_w * s_cap_h * 3);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, s_cap_w, s_cap_h, GL_RGB, GL_UNSIGNED_BYTE, s_cap);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    s_cap_n++;
}

#define SW 160
#define SH 120
static uint32_t src[SW * SH];
static uint32_t rgb(double y, double cb, double cr) {
    double r = y + 1.402 * cr, g = y - 0.344136 * cb - 0.714136 * cr, b = y + 1.772 * cb;
    int R = (int)(r < 0 ? 0 : r > 1 ? 255 : r * 255 + 0.5);
    int G = (int)(g < 0 ? 0 : g > 1 ? 255 : g * 255 + 0.5);
    int B = (int)(b < 0 ? 0 : b > 1 ? 255 : b * 255 + 0.5);
    return 0xff000000u | ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;   /* BGRA bytes */
}
static void frame_src(void) {
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            int blk = ((x >> 1) + (y >> 1)) & 1;
            if (x < 120) src[y * SW + x] = rgb(0.5, blk ? 0.15 : -0.15, blk ? -0.15 : 0.15);
            else src[y * SW + x] = rgb(x < 140 ? 0.15 : 0.85, 0.0, 0.0);   /* sharp grey luma edge */
        }
}
static uint8_t *present(int d24) {
    const int n0 = s_cap_n;
    gl_renderer_note_present_depth24(d24);
    gl_renderer_present(src, SW, SH, 0, 1, 0);
    check(s_cap_n == n0 + 1 && s_cap, "present captured");
    uint8_t *c = (uint8_t *)malloc((size_t)s_cap_w * s_cap_h * 3);
    memcpy(c, s_cap, (size_t)s_cap_w * s_cap_h * 3);
    return c;
}
static double luma(const uint8_t *p) { return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; }
static double cb(const uint8_t *p) { return -0.168736 * p[0] - 0.331264 * p[1] + 0.5 * p[2]; }
/* Over the chroma-block area of the letterboxed image (middle rows, left
 * 70%): mean |Cb step| between horizontal neighbours, and max luma error
 * against the flat luma. Then the luma edge's width in output pixels. */
static double s_mean_sat;
static void stats(const uint8_t *c, double *step, double *lerr, int *edge_w) {
    double s = 0, sat = 0; int n = 0; *lerr = 0;
    int y0 = s_cap_h / 3, y1 = 2 * s_cap_h / 3;
    for (int y = y0; y < y1; y += 3)
        for (int x = s_cap_w / 8 + 4; x < s_cap_w * 55 / 100; x++) {
            const uint8_t *p = c + ((size_t)y * s_cap_w + x) * 3;
            s += fabs(cb(p + 3) - cb(p)); n++;
            int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]);
            int mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
            sat += mx - mn;
            double e = fabs(luma(p) - 127.5);
            if (e > *lerr) *lerr = e;
        }
    *step = s / n;
    s_mean_sat = sat / n;
    int w = 0;
    const int y = s_cap_h / 2;
    for (int x = s_cap_w * 125 / 160; x < s_cap_w * 155 / 160; x++) {
        double l = luma(c + ((size_t)y * s_cap_w + x) * 3);
        w += l > 60 && l < 195;
    }
    *edge_w = w;
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("FMV chroma hidden test", 0, 0, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    glb_init(vram); glb_set_scale(1); gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    frame_src();
    check(gl_renderer_fmv_chroma_smoothing() == 0, "off by default");
    uint8_t *off = present(1);
    gl_renderer_set_fmv_chroma_smoothing(1);
    uint8_t *not24 = present(0);
    const size_t bytes = (size_t)s_cap_w * s_cap_h * 3;
    check(memcmp(off, not24, bytes) == 0, "a 15-bit present is untouched");
    check(gl_renderer_fmv_chroma_frames() == 0, "only 24-bit presents are smoothed");
    uint8_t *on = present(1);
    check(gl_renderer_fmv_chroma_frames() == 1, "24-bit present smoothed");
    double s_off, s_on, l_off, l_on; int e_off, e_on;
    stats(off, &s_off, &l_off, &e_off);
    const double sat_off = s_mean_sat;
    stats(on, &s_on, &l_on, &e_on);
    const double sat_on = s_mean_sat;
    printf("mean saturation off=%.1f on=%.1f\n", sat_off, sat_on);
    printf("drawable=%dx%d cb_step off=%.2f on=%.2f luma_err off=%.1f on=%.1f edge_w off=%d on=%d\n",
           s_cap_w, s_cap_h, s_off, s_on, l_off, l_on, e_off, e_on);
    check(s_on < s_off / 3.0, "chroma block steps shrink");
    /* The blocks alternate between two hues, so the tent's average keeps a
     * known fraction of the colour: never grey. A flat-chroma frame keeps
     * its saturation exactly (below). */
    check(sat_on > 2.0, "colour is not lost");
    check(l_on < 6.0, "luma kept");
    check(e_on <= e_off + 1, "luma edge stays as sharp");
    /* Uniform chroma inside letterbox bars: smoothing must keep the colour
     * exactly (within rounding). A tent that lost the source size would
     * sample the black corner and turn the movie grey. */
    /* A letterboxed movie: black bars around a uniform colour. */
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++)
            src[y * SW + x] = (y < 12 || y >= SH - 12) ? rgb(0.0, 0.0, 0.0) : rgb(0.45, 0.12, -0.10);
    uint8_t *flat_on = present(1);
    gl_renderer_set_fmv_chroma_smoothing(0);
    uint8_t *flat_off = present(1);
    int worst = 0;
    /* Away from the bars and the image's outer columns (the tent
     * legitimately mixes at an edge). */
    for (int y = 3 * s_cap_h / 8; y < 5 * s_cap_h / 8; y++)
        for (int x = 3 * 8; x < (s_cap_w - 8) * 3; x++) {
            size_t i = (size_t)y * s_cap_w * 3 + x;
            int d = abs((int)flat_on[i] - (int)flat_off[i]);
            if (d > worst) worst = d;
        }
    printf("flat chroma worst channel delta=%d\n", worst);
    check(worst <= 2, "uniform chroma kept exactly");
    free(flat_on); free(flat_off);
    frame_src();
    uint8_t *off2 = present(1);
    check(memcmp(off, off2, bytes) == 0, "off again is byte-identical");
    free(off); free(not24); free(on); free(off2);
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
