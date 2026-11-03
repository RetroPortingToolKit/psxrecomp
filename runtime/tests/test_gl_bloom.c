/* Bloom ([video] bloom) on a real, hidden OpenGL context.
 *
 *  - off: the composed frame is the stock one (byte-identical before and
 *    after a round trip through bloom; the pass never runs);
 *  - on: a bright 2D primitive (sprite, HUD) and a 2D one drawn over a lit 3D
 *    one gain no glow at all (byte-identical to off), while an additive
 *    primitive and a PGXP-proven 3D one glow around their edges;
 *  - guest VRAM / readbacks are the same either way, with and without the
 *    render thread.
 * With --dump PATH the off frame of every scene is written raw (RGB,
 * bottom-up) so a run against another tree can be compared byte for byte
 * (BLOOM_FIXTURE_NO_BLOOM builds it without the bloom API). */
#define GL_PRESENT_TEST_HOOK(gen) bloom_capture_hook(gen)
#include <stdint.h>
static uint64_t pfx_fixture_clock(void);
#define PFX_NOW_TICKS() pfx_fixture_clock()
static void bloom_capture_hook(int gen);
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
static uint64_t s_pfx_test_clock;
static uint64_t pfx_fixture_clock(void) {
    return s_pfx_test_clock ? s_pfx_test_clock : SDL_GetPerformanceCounter();
}
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
static void bloom_capture_hook(int gen) {
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

enum { SC_2D, SC_ADD, SC_3D, SC_HUD_OVER_3D, SC_PARTIAL, SC_N };
enum { SC_DEPTH_ONLY = 99 };
static const char *k_scene[SC_N] = { "2d", "additive", "3d", "hud-over-3d", "partial" };
/* SC_PARTIAL's grey HUD panels (native x, y, w, h): one beside the lamp, one
 * covering part of it. */
static const int k_hud[2][4] = { { 204, 100, 40, 40 }, { 150, 110, 30, 20 } };

/* A PGXP-proven triangle: precise positions plus GTE SZ, as gpu.c sends. */
static void tri3d(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    glb_set_precise_triangle(1, x0 * 65536, y0 * 65536, x1 * 65536, y1 * 65536,
                             x2 * 65536, y2 * 65536);
    glb_set_depth_triangle(1, 2000.0f, 2000.0f, 2000.0f);
    glb_draw_flat_triangle(x0, y0, x1, y1, x2, y2, c);
}

static uint8_t *frame(int scene, float bloom, uint64_t *vram_hash) {
#ifndef BLOOM_FIXTURE_NO_BLOOM
    gl_renderer_set_bloom(bloom);
#else
    (void)bloom;
#endif
    glb_set_draw_area(0, 0, FRAME_W - 1, FRAME_H - 1); glb_set_draw_offset(0, 0);
    glb_set_mask_bits(0, 0); glb_set_semi_transparency(0, 0);
    glb_fill_rect(0, 0, FRAME_W, FRAME_H, 0x0000);
    /* A dark 3D ground everywhere: lit (may take a halo), below the threshold. */
    if (scene != SC_DEPTH_ONLY) {
        tri3d(0, 0, FRAME_W, 0, 0, FRAME_H, 0x0842);
        tri3d(FRAME_W, 0, FRAME_W, FRAME_H, 0, FRAME_H, 0x0842);
    }
    switch (scene) {
    case SC_2D:     /* a white HUD panel */
        glb_draw_flat_rect(120, 90, 80, 60, 0x7fff);
        break;
    case SC_ADD:    /* an additive light halo over black */
        glb_set_semi_transparency(1, 1);
        glb_draw_flat_triangle(120, 150, 200, 150, 160, 80, 0x7fff);
        glb_set_semi_transparency(0, 0);
        break;
    case SC_DEPTH_ONLY:
    case SC_3D:     /* a lit 3D polygon */
        tri3d(120, 150, 200, 150, 160, 80, 0x7fff);
        break;
    case SC_PARTIAL:       /* a visible lamp with grey HUD panels beside and over it */
        tri3d(120, 150, 200, 150, 160, 80, 0x7fff);
        for (int i = 0; i < 2; i++) glb_draw_flat_rect(k_hud[i][0], k_hud[i][1], k_hud[i][2], k_hud[i][3], 0x4210);
        break;
    case SC_HUD_OVER_3D:   /* the same 3D polygon under an opaque panel */
        tri3d(120, 150, 200, 150, 160, 80, 0x7fff);
        glb_draw_flat_rect(100, 70, 120, 90, 0x7fff);
        break;
    }
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0, 0, FRAME_W, FRAME_H, peek), "vram readback");
    *vram_hash = fnv(peek, FRAME_W * FRAME_H * 2, 0xcbf29ce484222325ull);
    const int n0 = s_cap_n;
    if (s_rth_on) gl_renderer_render_thread_frame_boundary();
    gl_renderer_present_vram(0, 0, FRAME_W, FRAME_H, 0, 0);
    if (s_rth_on) {
        gl_renderer_render_thread_frame_boundary();
        gl_renderer_render_thread_sync("bloom-capture");
    }
    check(s_cap_n == n0 + 1 && s_cap, "present captured");
    uint8_t *c = (uint8_t *)malloc((size_t)s_cap_w * s_cap_h * 3);
    memcpy(c, s_cap, (size_t)s_cap_w * s_cap_h * 3);
    return c;
}

/* Window pixels of SC_PARTIAL's HUD panels that differ between a and b. */
static long hud_changed(const uint8_t *a, const uint8_t *b) {
    const int sx = s_cap_w / FRAME_W, sy = s_cap_h / FRAME_H;
    long n = 0;
    for (int i = 0; i < 2; i++)
        for (int y = k_hud[i][1] * sy; y < (k_hud[i][1] + k_hud[i][3]) * sy; y++)
            for (int x = k_hud[i][0] * sx; x < (k_hud[i][0] + k_hud[i][2]) * sx; x++) {
                const size_t o = ((size_t)(s_cap_h - 1 - y) * s_cap_w + x) * 3;   /* bottom-up */
                n += memcmp(a + o, b + o, 3) != 0;
            }
    return n;
}

/* Brightness the pass added on the dark ground around the shapes (the glow). */
static long glow(const uint8_t *off, const uint8_t *on) {
    long sum = 0;
    for (int i = 0; i < s_cap_w * s_cap_h; i++) {
        if (off[i * 3] > 0x50 || off[i * 3 + 1] > 0x50 || off[i * 3 + 2] > 0x50) continue;
        for (int c = 0; c < 3; c++) sum += abs((int)on[i * 3 + c] - (int)off[i * 3 + c]);
    }
    return sum;
}

int main(int argc, char **argv) {
    const char *dump = argc > 2 && !strcmp(argv[1], "--dump") ? argv[2] : NULL;
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SKIP no video\n"); return 77; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#if defined(__APPLE__)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Bloom hidden test", 0, 0, 640, 480,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) { fprintf(stderr, "SKIP no window\n"); return 77; }
    glb_init(vram); glb_set_scale(2); gl_renderer_set_swap_interval(0);
    if (!gl_renderer_init_context(win)) { fprintf(stderr, "SKIP no GL context\n"); return 77; }
    uint8_t *off[SC_N]; uint64_t h_off[SC_N];
    for (int s = 0; s < SC_N; s++) off[s] = frame(s, 0.0f, &h_off[s]);
    const size_t bytes = (size_t)s_cap_w * s_cap_h * 3;
    if (dump) {
        FILE *f = fopen(dump, "wb");
        for (int s = 0; s < SC_N; s++)
            check(f && fwrite(off[s], 1, bytes, f) == bytes, "dump");
        if (f) fclose(f);
    }
#ifndef BLOOM_FIXTURE_NO_BLOOM
    GlBloomInfo bi;
    gl_renderer_bloom_debug(-1.0f, -1.0f, -1, &bi);
    check(bi.passes == 0, "off never runs the bloom pass");
    for (int pass = 0; pass < 2; pass++) {   /* 0: this thread, 1: render thread */
        if (pass == 1) {
            check(gl_renderer_render_thread_start(2) == 1, "render thread started");
            gl_renderer_render_thread_frame_boundary();
        }
        long g[SC_N];
        for (int s = 0; s < SC_N; s++) {
            uint64_t h = 0;
            uint8_t *on = frame(s, 1.5f, &h);
            g[s] = glow(off[s], on);
            check(h == h_off[s], "guest VRAM identical with bloom on");
            if (s == SC_2D || s == SC_HUD_OVER_3D)
                check(memcmp(off[s], on, bytes) == 0, "2D / HUD pixels never glow");
            if (s == SC_PARTIAL) {
                const long hc = hud_changed(off[s], on);
                printf("partial: hud pixels changed=%ld\n", hc);
                check(hc == 0, "HUD panels beside and over a lamp are byte-identical");
            }
            free(on);
            uint8_t *back = frame(s, 0.0f, &h);
            check(memcmp(off[s], back, bytes) == 0 && h == h_off[s], "off after bloom is byte-identical");
            free(back);
        }
        printf("%s drawable=%dx%d glow 2d=%ld additive=%ld 3d=%ld hud-over-3d=%ld\n",
               pass ? "rt" : "st", s_cap_w, s_cap_h, g[SC_2D], g[SC_ADD], g[SC_3D], g[SC_HUD_OVER_3D]);
        check(g[SC_2D] == 0 && g[SC_HUD_OVER_3D] == 0, "no glow from 2D");
        check(g[SC_ADD] > 20000, "additive primitive glows");
        check(g[SC_3D] > 20000, "3D primitive glows");
        check(g[SC_PARTIAL] > 20000, "a partly covered lamp still glows");
    }
    /* ---- the pipeline itself: imported chains and the mask input ---- */
    {
        FILE *f = fopen("pfx_invert.glsl", "w");
        fputs("void main(){ ivec2 p = ivec2(gl_FragCoord.xy) - u_org;\n"
              "  vec4 c = texelFetch(u_tex, p, 0); o = vec4(1.0 - c.rgb, 1.0); }\n", f);
        fclose(f);
        f = fopen("pfx_invert.pfx", "w");
        fputs("pfx 1\n# a user chain: half-res copy, then invert\n"
              "pass half\n  shader builtin:blur\n  input u_tex source\n  scale 0.5\n  uniform u_dir 0 0\n"
              "pass invert\n  shader pfx_invert.glsl\n  input u_tex source\n  input u_half half\n", f);
        fclose(f);
        f = fopen("pfx_mask.pfx", "w");
        fputs("pfx 1\npass show\n  shader pfx_mask.glsl\n  input u_mask mask\n", f);
        fclose(f);
        f = fopen("pfx_mask.glsl", "w");
        fputs("void main(){ float m = texelFetch(u_mask, ivec2(gl_FragCoord.xy) - u_org, 0).r;\n"
              "  o = vec4(vec3(m), 1.0); }\n", f);
        fclose(f);
        f = fopen("pfx_screen.pfx", "wb");
        fputs("pfx 1\npass down\n  shader pfx_copy.glsl\n  input u_tex source_down\n  scale 0.5\n"
              "pass screen\n  shader pfx_half.glsl\n  input u_down down\n  blend screen\n", f);
        fclose(f);
        f = fopen("pfx_copy.glsl", "wb");
        fputs("void main(){ o = textureLod(u_tex, v_uv, 0.0); }\n", f);
        fclose(f);
        f = fopen("pfx_half.glsl", "wb");   /* red 0.5 screen-blended; u_down read so it stays bound */
        fputs("void main(){ o = vec4(0.5 + 0.0 * textureLod(u_down, v_uv, 0.0).r, 0.0, 0.0, 0.0); }\n", f);
        fclose(f);
        f = fopen("pfx_bad.pfx", "w");
        fputs("pfx 1\npass x\n  shader builtin:nope\n", f);
        fclose(f);
        f = fopen("pfx_nocompile.pfx", "w");
        fputs("pfx 1\npass x\n  shader pfx_nocompile.glsl\n  input u_tex source\n", f);
        fclose(f);
        f = fopen("pfx_nocompile.glsl", "w");
        fputs("void main(){ o = this is not glsl; }\n", f);
        fclose(f);
    }
    gl_renderer_render_thread_stop();
    for (int pass = 0; pass < 2; pass++) {   /* 0: this thread, 1: render thread */
        uint64_t h = 0;
        gl_renderer_set_post_shader("pfx_invert.pfx");
        check(gl_renderer_post_shader_loaded(), "imported chain loads");
        uint8_t *inv = frame(SC_PARTIAL, 0.0f, &h);
        long wrong = 0;
        for (size_t i = 0; i < bytes; i++) wrong += inv[i] != (uint8_t)(255 - off[SC_PARTIAL][i]);
        printf("%s import: inverted mismatches=%ld\n", pass ? "rt" : "st", wrong);
        check(wrong == 0, "imported shader runs on the whole game image");
        check(h == h_off[SC_PARTIAL], "imported chain leaves guest VRAM alone");
        free(inv);

        gl_renderer_set_post_shader("pfx_mask.pfx");
        check(gl_renderer_post_shader_loaded(), "mask chain loads");
        uint8_t *m = frame(SC_PARTIAL, 0.0f, &h);
        long hud_lit = 0, lit = 0;
        const int sx = s_cap_w / FRAME_W, sy = s_cap_h / FRAME_H;
        for (int i = 0; i < 2; i++)
            for (int y = k_hud[i][1] * sy; y < (k_hud[i][1] + k_hud[i][3]) * sy; y++)
                for (int x = k_hud[i][0] * sx; x < (k_hud[i][0] + k_hud[i][2]) * sx; x++)
                    hud_lit += m[((size_t)(s_cap_h - 1 - y) * s_cap_w + x) * 3] != 0;
        for (int i = 0; i < s_cap_w * s_cap_h; i++) lit += m[i * 3] == 255;
        printf("%s mask: hud lit=%ld lit=%ld of %d\n", pass ? "rt" : "st", hud_lit, lit, s_cap_w * s_cap_h);
        check(hud_lit == 0, "mask input: HUD panels are unlit");
        check(lit > s_cap_w * s_cap_h / 2, "mask input: the 3D ground and lamp are lit");
        free(m);

        /* A history input also works on the final window pass. The blue
         * channel observes the shader clock, so reload must reset its origin. */
        FILE *history_file=fopen("pfx_history.glsl","w");
        check(history_file!=NULL,"open history shader");
        if(history_file) {
            fputs("void main(){ float r=texelFetch(u_prev,ivec2(gl_FragCoord.xy)-u_org,0).r;"
                  "o=vec4(r*0.5+u_add,0.0,clamp(u_time/4.0,0.0,1.0),1.0); }\n",history_file);
            fclose(history_file);
        }
        history_file=fopen("pfx_history.pfx","w");
        if(history_file) {
            fputs("pfx 1\npass history\n shader pfx_history.glsl\n input u_prev history\n uniform u_add 0.25\n",history_file);
            fclose(history_file);
        }
        psx_host_set_post_uniform("u_add",0.25f,0,0,0);
        s_pfx_test_clock=100*SDL_GetPerformanceFrequency();
        gl_renderer_set_post_shader("pfx_history.pfx");
        uint8_t *hist=frame(SC_PARTIAL,0,&h);
        size_t center=((size_t)(s_cap_h/2)*s_cap_w+s_cap_w/2)*3;
        check(abs(hist[center]-64)<=1 && hist[center+2]<=1,"first history frame is cleared and starts at time zero");
        free(hist);
        s_pfx_test_clock=102*SDL_GetPerformanceFrequency();
        hist=frame(SC_PARTIAL,0,&h);
        check(abs(hist[center]-96)<=1 && abs(hist[center+2]-128)<=1,"final pass samples the previous real image and its own elapsed time");
        free(hist);
        if(!s_rth_on) {
            for(int repeat=0;repeat<2;++repeat) {
                postfx_present(0,0,0,0,s_cap_w,s_cap_h);
                bloom_capture_hook(1);
                check(abs(s_cap[center]-112)<=1,"generated presents do not advance history");
            }
        }
        psx_host_set_post_uniform("u_add",0.125f,0,0,0);
        hist=frame(SC_PARTIAL,0,&h);
        check(abs(hist[center]-80)<=1,"live uniform update reaches the actual shader");
        free(hist);
        s_pfx_test_clock=104*SDL_GetPerformanceFrequency();
        gl_renderer_set_post_shader("pfx_history.pfx");
        hist=frame(SC_PARTIAL,0,&h);
        check(abs(hist[center]-32)<=1 && hist[center+2]<=1,"reload clears history and resets shader time while retaining live values");
        free(hist);
        s_pfx_test_clock=0;

        FILE *depth_file=fopen("pfx_depth.glsl","w");
        if(depth_file) {
            fputs("void main(){ float d=texelFetch(u_depth,ivec2(gl_FragCoord.xy)-u_org,0).r;"
                  "o=vec4(clamp(d/4000.0,0.0,1.0),0.0,0.0,1.0); }\n",depth_file);
            fclose(depth_file);
        }
        depth_file=fopen("pfx_depth.pfx","w");
        if(depth_file) {
            fputs("pfx 1\npass depth\n shader pfx_depth.glsl\n input u_depth depth\n",depth_file);
            fclose(depth_file);
        }
        gl_renderer_set_pgxp_depth(1);
        gl_renderer_set_post_shader("pfx_depth.pfx");
        uint8_t *depth=frame(SC_DEPTH_ONLY,0,&h);
        size_t outside=((size_t)(s_cap_h-1-20)*s_cap_w+20)*3;
        check(abs(depth[center]-128)<=3,"depth input reconstructs the triangle's GTE screen depth");
        check(depth[outside]<=1,"depth input is zero where no 3D primitive drew");
        free(depth);
        gl_renderer_set_pgxp_depth(0);

        gl_renderer_set_post_shader("pfx_screen.pfx");
        check(gl_renderer_post_shader_loaded(), "source_down + blend screen chain loads");
        uint8_t *sc = frame(SC_PARTIAL, 0.0f, &h);
        long bad = 0;
        for (int i = 0; i < s_cap_w * s_cap_h; i++) {
            const int c0 = off[SC_PARTIAL][i * 3], want = (int)(127.5f + (float)c0 * 0.5f + 0.5f);
            bad += abs(sc[i * 3] - want) > 1 || sc[i * 3 + 1] != off[SC_PARTIAL][i * 3 + 1] ||
                   sc[i * 3 + 2] != off[SC_PARTIAL][i * 3 + 2];
        }
        printf("%s screen: mismatches=%ld\n", pass ? "rt" : "st", bad);
        check(bad == 0, "blend screen: o + dst * (1 - o) on the window, other channels untouched");
        free(sc);
        gl_renderer_set_post_shader("pfx_bad.pfx");
        check(!gl_renderer_post_shader_loaded(), "a bad preset is refused");
        gl_renderer_set_post_shader("pfx_nocompile.pfx");
        uint8_t *nc = frame(SC_PARTIAL, 0.0f, &h);
        check(memcmp(nc, off[SC_PARTIAL], bytes) == 0, "a pass that does not compile leaves the frame alone");
        free(nc);
        if (s_rth_on) gl_renderer_render_thread_sync("builds");
        const uint64_t b0 = s_pfx_builds;
        nc = frame(SC_PARTIAL, 0.0f, &h);
        if (s_rth_on) gl_renderer_render_thread_sync("builds");
        check(s_pfx_builds == b0 && memcmp(nc, off[SC_PARTIAL], bytes) == 0,
              "a failed pass is not rebuilt on the next present");
        free(nc);
        gl_renderer_set_post_shader("");
        uint8_t *back = frame(SC_PARTIAL, 0.0f, &h);
        check(memcmp(back, off[SC_PARTIAL], bytes) == 0, "no chain: byte-identical again");
        free(back);
        if (pass == 0) {
            check(gl_renderer_render_thread_start(2) == 1, "render thread restarted");
            gl_renderer_render_thread_frame_boundary();
        }
    }
    gl_renderer_render_thread_stop();
    /* The executor leaves the GL state as it found it (capabilities both ways
     * round, the viewport, the clear colour; the window target bound), with
     * every stage on (bloom, an imported chain reading the mask, FXAA). */
    {
        uint64_t h = 0;
        gl_renderer_set_post_shader("pfx_mask.pfx");
        gl_renderer_set_post_aa(GL_POST_AA_FXAA);
        free(frame(SC_3D, 1.5f, &h));           /* marks for a reused-marks pass */
        int ww = 0, wh = 0; SDL_GL_GetDrawableSize(s_win, &ww, &wh);
        for (int on = 0; on < 2; on++) {
            PfxGlState want, got;
            if (on) { glEnable(GL_BLEND); glEnable(GL_DEPTH_TEST); glEnable(GL_SCISSOR_TEST); glEnable(GL_STENCIL_TEST); }
            else { glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); }
            glClearColor(0.25f, 0.5f, 0.75f, on ? 1.0f : 0.0f);
            glViewport(3, 5, 101 + on, 77);
            p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);   /* a present draws to the window */
            p_glActiveTexture(PSXGL_TEXTURE0);
            pfx_state_save(&want);
            GLfloat cc0[4], cc1[4]; glGetFloatv(GL_COLOR_CLEAR_VALUE, cc0);
            const uint64_t n0 = s_bloom_passes, a0 = s_paa_passes;
            postfx_present(0, 1, 0, 0, ww, wh);
            pfx_state_save(&got);
            glGetFloatv(GL_COLOR_CLEAR_VALUE, cc1);
            GLint dfb = -1, rfb = -1, act = -1;
            glGetIntegerv(0x8CA6, &dfb); glGetIntegerv(0x8CAA, &rfb); glGetIntegerv(0x84E0, &act);
            check(s_bloom_passes == n0 + 1 && s_paa_passes == a0 + 1, "state test ran bloom and AA");
            check(memcmp(&want, &got, sizeof want) == 0, "the pipeline restores caps and viewport");
            check(!memcmp(cc0, cc1, sizeof cc0), "the pipeline never touches the clear colour");
            check(dfb == 0 && rfb == 0 && act == (GLint)PSXGL_TEXTURE0, "the pipeline leaves the window target bound");
        }
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST);
        gl_renderer_set_post_shader("");
        gl_renderer_set_post_aa(GL_POST_AA_OFF);
    }
    (void)k_scene;
#endif
    for (int s = 0; s < SC_N; s++) free(off[s]);
    check(glGetError() == GL_NO_ERROR, "GL errors");
    printf("checks=%d failures=%d\n", checks, failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures ? 1 : 0;
}
