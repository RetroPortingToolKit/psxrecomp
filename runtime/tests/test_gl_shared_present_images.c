/* Real GL image/ownership regression fixture. Reuse source-owned renderer
 * stubs; the ordinary frame-generation fixture keeps its original main. */
#define PSX_SHARED_PRESENT_IMAGES 1
#define GL_PRESENT_TEST_HOOK(gen) ((void)(gen))
#define GL_PRESENT_THREAD_TEST_HOOK(slot) ((void)(slot))
#define main unused_frame_generation_main
#include "test_gl_frame_gen.c"
#undef main

static uint64_t image_digest(GLuint tex, int w, int h) {
    size_t bytes = (size_t)w * h * 4;
    uint8_t *pixels = (uint8_t *)malloc(bytes);
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    uint64_t result = fnv(pixels, bytes, 0xcbf29ce484222325ull);
    free(pixels);
    return result;
}

static uint64_t compose_hold(void) {
    check(gl_renderer_present_hold_last() == 1, "held image presents");
    check(s_hold_kind == HOLD_DRAWABLE && s_hold_image.tex == s_hold_tex,
          "native hold upgrades into private drawable storage");
    return image_digest(s_hold_tex, s_hold_tw, s_hold_th);
}

int main(void) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    SDL_Window *win = SDL_CreateWindow("Completed image ownership test", 0, 0,
                                      640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win) return 77;
    gr_set_backend(GR_BACKEND_OPENGL);
    gr_init(vram);
    gr_set_scale(3);
    if (!gl_renderer_init_context(win)) return 77;
    gl_renderer_set_swap_interval(0);
    gl_renderer_set_interpolation(1, 120, 120, 60, 0);
    const int sw = 960, sh = 720;
    uint8_t *pattern = (uint8_t *)malloc((size_t)sw * sh * 4);
    for (int y = 0; y < sh; y++) for (int x = 0; x < sw; x++) {
        size_t p = ((size_t)y * sw + x) * 4;
        pattern[p] = (uint8_t)(x * 17 + y);
        pattern[p+1] = (uint8_t)(y * 13);
        pattern[p+2] = (uint8_t)(x ^ (y * 3));
        pattern[p+3] = 255;
    }
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, pattern);
    p_glGenFramebuffers(1, &fbo);
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, fbo);
    p_glFramebufferTexture2D(PSXGL_FRAMEBUFFER, PSXGL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    check(p_glCheckFramebufferStatus(PSXGL_FRAMEBUFFER) == PSXGL_FRAMEBUFFER_COMPLETE,
          "pattern source framebuffer complete");
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER, 0);
    for (int resized = 0; resized < 2; resized++) {
        if (resized) { SDL_SetWindowSize(win, 720, 400); SDL_PumpEvents(); }
        for (int hiw = 0; hiw < 2; hiw++) for (int force = 0; force < 2; force++)
        for (int linear = 0; linear < 2; linear++) {
            s_hiw = hiw;
            int captured = interp_capture(fbo, 0, 0, 320, 240, linear, force,
                                           resized ? GL_PRES_WIDE : GL_PRES_VRAM, 0, 0, 1);
            check(captured == INTERP_CAPTURED, "fresh native history produced");
            GLuint history = s_interp_tex[s_interp_cur];
            int width = s_interp_w, height = s_interp_h;
            uint64_t original = image_digest(history, width, height);
            hold_capture_native_fbo(fbo, 0, 0, 320, 240, force, linear);
            uint64_t reference = compose_hold();
            hold_publish_interpolated(captured, fbo, 0, 0, 320, 240, force, linear);
            check(s_hold_image.tex == history && history != s_hold_tex,
                  "fresh hold shares immutable history and private capture stays distinct");
            check(s_hold_image.w == width && s_hold_image.h == height,
                  "published image carries its capture dimensions");
            interp_reset_history();
            check(s_hold_image.tex == history && s_hold_image.w == width && s_hold_image.h == height,
                  "reset retains the completed held image until next publication");
            check(compose_hold() == reference, "shared and copied pre-OSD compositions match exactly");
            check(image_digest(history, width, height) == original,
                  "drawable upgrade never writes into sampled temporal history");
        }
    }
    /* Keep HOLD_NATIVE published across ordinary two-slot rotation. No reset
     * or drawable upgrade may hide a stale/dangling descriptor here. */
    s_hiw = 0;
    uint64_t previous = 0;
    for (int frame = 0; frame < 4; frame++) {
        for (size_t p = 0; p < (size_t)sw * sh * 4; p++)
            pattern[p] = (uint8_t)(p * 13 + frame * 29);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, pattern);
        int captured = interp_capture(fbo, 0, 0, 320, 240, 1, 0, GL_PRES_VRAM, 0, 0, 1);
        if (frame)
            check(image_digest(s_interp_tex[s_interp_prev], sw, sh) == previous,
                  "rotation retains the preceding completed image");
        hold_publish_interpolated(captured, fbo, 0, 0, 320, 240, 0, 1);
        check(s_hold_kind == HOLD_NATIVE && s_hold_image.tex == s_interp_tex[s_interp_cur],
              "each fresh rotation republishes the newest native image");
        previous = image_digest(s_hold_image.tex, sw, sh);
    }
    GLuint newest = s_interp_tex[s_interp_cur];
    GLuint older = s_interp_tex[s_interp_prev];
    uint64_t older_digest = image_digest(older, sw, sh);
    hold_capture_native_fbo(fbo, 0, 0, 320, 240, 0, 1);
    uint64_t rotated_reference = compose_hold();
    hold_publish_interpolated(INTERP_CAPTURED, fbo, 0, 0, 320, 240, 0, 1);
    check(compose_hold() == rotated_reference, "rotated newest image matches original copied hold");
    check(image_digest(newest, sw, sh) == previous && image_digest(older, sw, sh) == older_digest,
          "drawable upgrade preserves both rotating history slots");

    /* A resize and capture reallocation while the old native descriptor is
     * published must publish new dimensions before the next hold consumer. */
    hold_publish_interpolated(INTERP_CAPTURED, fbo, 0, 0, 320, 240, 0, 1);
    SDL_SetWindowSize(win, 480, 360);
    SDL_PumpEvents();
    s_hiw = 1;
    int changed = interp_capture(fbo, 0, 0, 320, 240, 1, 1, GL_PRES_WIDE, 0, 0, 1);
    hold_publish_interpolated(changed, fbo, 0, 0, 320, 240, 1, 1);
    check(s_hold_kind == HOLD_NATIVE && s_hold_image.w == s_interp_w &&
          s_hold_image.h == s_interp_h && s_hold_image.w < sw,
          "geometry-changing capture replaces live native descriptor and dimensions");
    hold_capture_native_fbo(fbo, 0, 0, 320, 240, 1, 1);
    uint64_t resized_reference = compose_hold();
    hold_publish_interpolated(changed, fbo, 0, 0, 320, 240, 1, 1);
    check(compose_hold() == resized_reference, "resized live native image matches copied hold");
    s_hiw = 0;
    gl_renderer_set_interpolation_source(1);
    int first = interp_capture(fbo, 0, 0, 320, 240, 0, 0, GL_PRES_VRAM, 0, 0, 1);
    check(first == INTERP_CAPTURED, "FLIP initial image captured");
    uint64_t stale = image_digest(s_interp_tex[s_interp_cur], sw, sh);
    memset(pattern, 0x73, (size_t)sw * sh * 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, pattern);
    int duplicate = interp_capture(fbo, 0, 0, 320, 240, 0, 0, GL_PRES_VRAM, 0, 0, 0);
    check(duplicate == INTERP_REUSED, "FLIP duplicate reuses history");
    hold_publish_interpolated(duplicate, fbo, 0, 0, 320, 240, 0, 0);
    check(s_hold_image.tex == s_hold_tex && image_digest(s_hold_tex, sw, sh) != stale,
          "duplicate FLIP holds current source using original copy, not reused history");
    check(glGetError() == GL_NO_ERROR, "no GL error across image ownership transitions");
    gl_renderer_shutdown();
    check(s_hold_kind == HOLD_NONE && s_hold_image.tex == 0,
          "context teardown invalidates held image descriptor");
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(pattern);
    printf("checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
