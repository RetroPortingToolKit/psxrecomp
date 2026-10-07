/* Real GL fixture, sharing the source-owned renderer stubs and setup helpers. */
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
void present_shot_done(int ok) { (void)ok; }
int host_osd_needs_present(void) { return 0; }
int psx_present_vsync_owns_cadence(void) { return 0; }
void latency_ring_mark(LatencyStage stage) { (void)stage; }
void psx_host_sleep_ms(unsigned ms) { SDL_Delay(ms); }
int present_shot_take(char* out,int n) { (void)out; (void)n; return 0; }
int host_osd_image(const uint32_t** p,int* w,int* h) { (void)p; (void)w; (void)h; return 0; }
int host_osd_volume_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
void host_osd_present_done(void) {}
int psx_rewind_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }
float psx_rewind_slide(void) { return 0; }
int psx_savestate_menu_overlay_image(const uint32_t** p,int* w,int* h) { return host_osd_image(p,w,h); }

static void upload_checker(int depth,int stp) {
    uint16_t page[128*128], palette[256];
    for(int i=0;i<256;++i) palette[i]=0x03e0;
    palette[1]=0x001f; palette[2]=0x7c00 | (stp?0x8000:0);
    const int pitch=depth==0?32:depth==1?64:128;
    for(int y=0;y<128;++y) for(int x=0;x<pitch;++x) {
        uint16_t p=0;
        if(depth==2) p=((x+y)&1)?palette[1]:palette[2];
        else {
            int bits=depth==0?4:8, count=16/bits;
            for(int n=0;n<count;++n) p|=(uint16_t)((((x*count+n+y)&1)?1:2)<<(n*bits));
        }
        page[y*pitch+x]=p;
    }
    glb_vram_transfer_in(512,0,pitch,128,page);
    glb_vram_transfer_in(512,256,256,1,palette);
}
static void triangle(int mode,int depth,int shift,int tracked) {
    glb_set_texture_filter(mode);
    glb_set_draw_area(0,0,319,239); glb_set_draw_offset(0,0);
    glb_set_mask_bits(0,0); glb_set_semi_transparency(0,0);
    glb_set_color_modulation(128,128,128,1);
    glb_fill_rect(0,0,64,64,0x03e0);
    glb_set_precise_triangle(tracked,20<<16,20<<16,44<<16,20<<16,20<<16,44<<16);
    glb_draw_textured_triangle(20,20,shift,0,44,20,127+shift,0,20,44,shift,127,512,256,(uint16_t)(8|(depth<<7)));
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0,0,64,64,peek),"filter readback");
}
static int chroma(void) {
    int total=0;
    for(int y=23;y<29;++y) for(int x=23;x<29;++x) {
        uint16_t c=peek[y*64+x]; total+=abs((c&31)-((c>>10)&31));
        check(((c>>5)&31)==0,"palette decode does not pick unrelated green entries");
    }
    return total;
}
static void atlas_edge(int depth) {
    uint16_t page[256*256], palette[256];
    const int word_pixels=depth==0?4:depth==1?2:1, pitch=256/word_pixels;
    for(int i=0;i<256;++i) palette[i]=0x03e0;
    palette[2]=0x7c00;
    for(int y=0;y<256;++y) for(int x=0;x<pitch;++x) {
        const int inside=y<127 && x<128/word_pixels;
        page[y*pitch+x]=depth==0?(inside?0x2222:0x1111):depth==1?(inside?0x0202:0x0101):(inside?0x7c00:0x03e0);
    }
    glb_vram_transfer_in(512,0,pitch,256,page);
    glb_vram_transfer_in(512,256,256,1,palette);
    glb_set_texture_filter(2); glb_fill_rect(0,0,64,64,0x001f);
    // The first covered pixel lies barely inside the triangle. Its footprint
    // extends below UV zero; wrapping before clamping would sample green at 255.
    const int fraction=64881; // 0.99 pixels; renderer adds half a pixel
    glb_set_precise_triangle(1,(20<<16)+fraction,(20<<16)+fraction,
        (44<<16)+fraction,(20<<16)+fraction,(20<<16)+fraction,(44<<16)+fraction);
    glb_draw_textured_triangle(20,20,0,0,44,20,127,0,20,44,0,127,512,256,(uint16_t)(8|(depth<<7)));
    gl_renderer_sync_cpu();
    check(gl_renderer_fbo_peek(0,0,64,64,peek),"atlas-edge readback");
    check((peek[21*64+21]&0x7fff)==0x7c00,"footprint clamps before wrapping into far atlas row");
}
static void native_hold_images(void) {
    GLuint textures[2];
    const uint8_t colors[2][4] = {{255,0,0,255},{0,0,255,255}};
    const uint32_t phases[2] = {0,32768};
    glGenTextures(2,textures);
    for(int i=0;i<2;++i) {
        glBindTexture(GL_TEXTURE_2D,textures[i]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,colors[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    for(int hold=0;hold<2;++hold) {
        uint32_t lo,hi; float blend; uint8_t pixel[4];
        check(render_pass_gen_select_mode(phases,2,0.25,hold,&lo,&hi,&blend),"native phase selection");
        glDisable(GL_SCISSOR_TEST);
        interp_draw_textures(textures[lo],textures[hi],blend,0,0,0,8,8);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,0);
        glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        if(hold) check(pixel[0]>250 && pixel[2]<5,"HOLD presents original native red image without ghosting");
        else check(pixel[0]>60 && pixel[2]>60,"explicit blend still mixes red and blue images");
    }
    glDeleteTextures(2,textures);
}
int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0) return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win=SDL_CreateWindow("Filtering hidden test",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win) return 2;
    glb_init(vram); glb_set_scale(1); gl_renderer_set_swap_interval(0);
    if(!gl_renderer_init_context(win)) return 2;
    native_hold_images();
    for(int depth=0;depth<3;++depth) {
        upload_checker(depth,0);
        triangle(0,depth,0,1); int nearest=chroma();
        triangle(2,depth,0,1); int stable=chroma();
        printf("depth=%d nearest_chroma=%d stable_chroma=%d\n",depth,nearest,stable);
        check(stable<nearest/2,"minified checkerboard aliases less");
        triangle(0,depth,0,0); uint64_t hud=fnv(peek,64*64*2,0xcbf29ce484222325ull);
        triangle(2,depth,0,0);
        check(hud==fnv(peek,64*64*2,0xcbf29ce484222325ull),"untracked UI remains nearest");
        // A texture window redirects every tap into an 8x8 region. Poison
        // surrounding palette entries so filtering across its edge is visible.
        upload_checker(depth,0);
        const int word_pixels=depth==0?4:depth==1?2:1;
        const int width=8/word_pixels;
        uint16_t patch[64];
        for(int i=0;i<width*8;++i) patch[i]=depth==0?0x2222:depth==1?0x0202:0x7c00;
        glb_vram_transfer_in(512+32/word_pixels,32,width,8,patch);
        glb_set_texture_window(31u | (31u<<5) | (4u<<10) | (4u<<15));
        triangle(2,depth,0,1);
        for(int y=21;y<26;++y) for(int x=21;x<26;++x)
            check((peek[y*64+x]&0x7fff)==0x7c00,"all filter taps respect texture window");
        glb_set_texture_window(0);
        atlas_edge(depth);
    }
    // A different CLUT after a prior draw must immediately change the filtered
    // result. No stale mip chain or palette-index averaging may survive it.
    upload_checker(0,0);
    uint16_t blue[]={0,0x7c00,0x7c00};
    glb_vram_transfer_in(512,256,3,1,blue);
    triangle(2,0,0,1);
    check((peek[24*64+24]&0x7fff)==0x7c00,"live palette update changes filtered color");
    // Cutouts and STP remain the center texel's classification.
    uint16_t transparent[32*128]={0};
    glb_vram_transfer_in(512,0,32,128,transparent);
    triangle(2,0,0,1);
    check(peek[24*64+24]==0x03e0,"zero texel remains a cutout");
    upload_checker(2,1); triangle(2,2,0,1);
    check((peek[24*64+24]&0x7fff)==0x001f || (peek[24*64+24]&0xffff)==0xfc00,
          "filter never averages colors across opaque and STP classes");
    check(glGetError()==GL_NO_ERROR,"GL errors");
    printf("checks=%d failures=%d\n",checks,failures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures?1:0;
}
