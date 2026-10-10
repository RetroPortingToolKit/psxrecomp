/* Real GL + real format/decoder + renderer facade. Source-owned fixtures. */
#define PSX_TEST_HD_TEXTURE_PACK 1
/* Every presented image (real or generated) passes here before its swap. */
static void fg_fixture_present(int generated);
#define GL_PRESENT_TEST_HOOK(gen) fg_fixture_present(gen)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "../third_party/stb_image.h"
#define main scale_fixture_main
#include "test_gl_scale_invariance.c"
#undef main
#include "gpu_render.c"
#include "duckstation_texture_pack.h"
#include "hd_texture_pack.h"
#include "png_write.h"

const GpuRenderBackend* vk_backend_get(void) { return NULL; }
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

static uint16_t source_words[4*4];
static uint16_t reference[1024*512];
static uint8_t pixels[64*64*4];
static const uint16_t texture_page=8u|(2u<<7);
static uint16_t part_words[16];
static uint16_t part_palette[16];
static const uint16_t part_p4_words[2]={0x1010,0x1132};

static void part_png(const char* root,const DuckTextureKey* key,int width,int height,int pattern) {
    char stem[256],path[2048];
    check(duck_texture_format_name(key,stem,sizeof(stem)),"partial fixture filename");
    snprintf(path,sizeof(path),"%s/replacements/%s.png",root,stem);
    uint8_t rgba[8*8*4];
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        uint8_t* pixel=rgba+(y*width+x)*4;
        pixel[0]=255; pixel[1]=pixel[2]=0; pixel[3]=255;
        if(pattern==0 && x==1) { pixel[0]=0;pixel[3]=128; }
        if(pattern==0 && x==2) { pixel[0]=0;pixel[2]=255;pixel[3]=127; }
        if(pattern==1) {
            pixel[0]=pixel[1]=0;pixel[2]=255;
            if(x==0) pixel[3]=0;
            else if(x==1) pixel[3]=243;
            else if(x==2) pixel[2]=pixel[3]=0;
            else {pixel[2]=0;pixel[3]=128;}
        }
        if(pattern==3) {pixel[0]=(x&1)?0:255;pixel[2]=(x&1)?255:0;}
    }
    FILE* file=fopen(path,"wb");check(file!=NULL,"open partial fixture PNG");
    if(file){check(png_write_rgba(file,rgba,width,height),"write partial fixture PNG");fclose(file);}
}
static void pack_config(const char* root,int linear) {
    char path[2048];snprintf(path,sizeof(path),"%s/config.yaml",root);
    FILE* file=fopen(path,"wb");check(file!=NULL,"open fixture config");
    if(file){fprintf(file,"DumpC16Textures: true\nMaxVRAMWriteSplits: 16\nMaxReplacementCacheVRAMUsage: 1\nReplacementScaleLinearFilter: %s\n",linear?"true":"false");fclose(file);}
}
static void pack_parts(const char* root) {
    for(int y=0;y<2;++y) for(int x=0;x<8;++x)
        part_words[y*8+x]=x==2?0x83e0:x==4?0:x==5?0x8000:x==6?0x7c00:0x03e0;
    for(int i=0;i<16;++i) part_palette[i]=i==2?0x83e0:i==3?0x7c00:0x03e0;
    DuckTextureKey key={0};
    key.source_hash=duck_texture_hash_words_le(part_words,16);
    key.source_width_words=8;key.source_height=2;key.width=2;key.height=2;
    key.depth=HD_TEXTURE_DEPTH_16BPP;key.kind=DUCK_TEXTURE_UPLOAD;
    part_png(root,&key,8,8,0);
    key.offset_x=2;key.semitransparent=1;part_png(root,&key,4,4,1);
    /* This image is added later to test reload/filtering, including reruns. */
    key.offset_x=6;key.semitransparent=0;
    char stem[256],path[2048];
    if(duck_texture_format_name(&key,stem,sizeof(stem))) {
        snprintf(path,sizeof(path),"%s/replacements/%s.png",root,stem);remove(path);
    }
    key=(DuckTextureKey){0};
    key.source_hash=duck_texture_hash_words_le(part_p4_words,2);
    key.source_width_words=2;key.source_height=1;key.width=4;key.height=1;
    key.depth=HD_TEXTURE_DEPTH_4BPP;key.kind=DUCK_TEXTURE_UPLOAD;
    key.palette_max=1;key.palette_hash=duck_texture_hash_words_le(part_palette,2);
    part_png(root,&key,8,4,2);
}

static void pack_png(const char* root,int st) {
    DuckTextureKey key={0};
    key.source_hash=duck_texture_hash_words_le(source_words,16);
    key.source_width_words=4; key.source_height=4;
    key.width=4; key.height=4; key.kind=DUCK_TEXTURE_UPLOAD;
    key.depth=HD_TEXTURE_DEPTH_16BPP; key.semitransparent=st;
    char stem[256],path[2048];
    check(duck_texture_format_name(&key,stem,sizeof(stem)),"fixture filename");
    snprintf(path,sizeof(path),"%s/replacements/%s.png",root,stem);
    uint8_t rgba[16*16*4];
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) {
        uint8_t* at=rgba+(y*16+x)*4;
        at[0]=(x&1)?0:255; at[1]=0; at[2]=(x&1)?255:0; at[3]=255;
        if(st) {
            const uint8_t alpha[6]={0,127,128,242,243,255};
            at[3]=alpha[x%6];
            if(y>=8) at[0]=at[1]=at[2]=0;
        } else if(y>=8) {
            at[0]=at[1]=at[2]=0;
            at[3]=(x&1)?128:127;
        }
    }
    FILE* file=fopen(path,"wb");
    check(file!=NULL,"open fixture PNG");
    if(file) { check(png_write_rgba(file,rgba,16,16),"write fixture PNG"); fclose(file); }
}
static void state(void) {
    gr_set_draw_area(0,0,1023,511); gr_set_draw_offset(0,0);
    gr_set_mask_bits(0,0); gr_set_semi_transparency(0,0);
    gr_set_texture_window(0); gr_set_color_modulation(128,128,128,1);
}
static void native_scene(void) {
    state();
    gr_vram_transfer_in(512,0,4,4,source_words);
    gr_fill_rect(0,0,64,64,0x4210);
    gr_draw_textured_rect(16,16,4,4,0,0,0,0,texture_page);
    for(int mode=0;mode<4;++mode) {
        gr_set_semi_transparency(1,mode);
        gr_draw_textured_rect(20+mode*5,20,4,4,0,0,0,0,texture_page);
    }
    gr_set_semi_transparency(0,0);
    gr_set_mask_bits(1,0); gr_draw_flat_rect(40,40,8,8,0x001f);
    gr_set_mask_bits(0,1); gr_draw_textured_rect(38,38,12,12,0,0,0,0,texture_page);
    gr_set_mask_bits(0,0);
    gr_set_texture_filter(1);
    gr_set_perspective_triangle(1,1.0f,0.5f,0.25f);
    gr_set_precise_triangle(1,(4<<16)+32768,30<<16,14<<16,30<<16,4<<16,38<<16);
    gr_draw_textured_triangle(4,30,0,0,14,30,3,0,4,38,0,3,0,0,texture_page);
    gr_set_texture_filter(0);
    gr_draw_line(3,50,53,50,0x7fff);
    gr_copy_rect(16,16,2,2,4,4);
    uint16_t masked[4]={0x7c00,0x7c00,0x7c00,0x7c00};
    gr_set_mask_bits(0,1); gr_vram_transfer_in(40,40,4,1,masked);
    gr_set_mask_bits(0,0);
    gr_fill_rect(1022,510,4,4,0x1234);
}
/* Smooth motion with an HD pack: presented images, read back before swap. */
static int fg_rec = 0;
static uint64_t fg_real_seq, fg_n_real, fg_n_gen, fg_last_real;
static int fg_gen_hud_hd, fg_gen_hud_bad, fg_real_hud_hd;
static uint64_t fg_fnv(const void* p, size_t n, uint64_t h) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}
static void fg_fixture_present(int generated) {
    if (!fg_rec) return;
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(s_win, &ww, &wh);
    uint8_t* px = (uint8_t*)malloc((size_t)ww * wh * 4);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, ww, wh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    /* Only the HUD can be red: its replacement (native texels are green/black). */
    int red = 0, green = 0;
    for (size_t i = 0; i < (size_t)ww * wh; ++i) {
        const uint8_t* q = px + i * 4;
        if (q[0] > 200 && q[1] < 40 && q[2] < 40) red++;
        if (q[1] > 200 && q[0] < 40 && q[2] < 40) green++;
    }
    (void)green;
    const int hd = red > 0;   /* the native texels hold no red */
    if (generated) { fg_n_gen++; if (hd) fg_gen_hud_hd++; else fg_gen_hud_bad++; }
    else {
        uint64_t d = fg_fnv(px, (size_t)ww * wh * 4, 0xcbf29ce484222325ull);
        if (d != fg_last_real) { fg_last_real = d; fg_real_seq = fg_fnv(&d, sizeof d, fg_real_seq); fg_n_real++; }
        if (hd) fg_real_hud_hd++;
    }
    free(px);
}

/* MinGW has no setenv/unsetenv. */
static void fg_force_env(int on) {
#ifdef _WIN32
    _putenv(on?"PSX_FRAME_GEN_FORCE=1":"PSX_FRAME_GEN_FORCE=");
#else
    if(on) setenv("PSX_FRAME_GEN_FORCE","1",1); else unsetenv("PSX_FRAME_GEN_FORCE");
#endif
}
/* Adversarial review cases (#568): a GP0(A0) payload streams into gpu.c's
 * array word by word, as gpu.c writes it; the render thread must stay held
 * until the commit or later payload words are overwritten by the publication
 * of its private copy. */
static uint16_t adv_snapshot[1024*512];
static void adv_payload_word(int x,int y,uint16_t w) { vram[y*1024+x]=w; }
static void adv_hd_cases(const char* root) {
    char error[512]={0};
    uint16_t back=0;
    const uint16_t done[4]={0x7c00,0x7c01,0x7c02,0x7c03};
    /* ADV1: HD switched on live while an A0 is still streaming. */
    gpu_hd_textures_shutdown();
    check(gl_renderer_render_thread_start(2)==1,"ADV1: render thread starts with HD off");
    gl_renderer_render_thread_frame_boundary();
    state(); gr_fill_rect(256,96,4,1,0x2345);
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"ADV1: render thread runs before the upload");
    gr_vram_upload_begin(256,96,4,1);
    adv_payload_word(256,96,done[0]);
    check(gpu_hd_textures_configure(root,1,0,error,sizeof(error)),"ADV1: HD opens mid-A0");
    check(vram[96*1024+256]==done[0],"ADV1: HD activation keeps the received payload word");
    adv_payload_word(257,96,done[1]);
    gl_renderer_render_thread_frame_boundary();   /* the payload spans a vblank */
    check(!s_hd_native_authority,"ADV1: HD authority waits for the streaming A0's commit");
    adv_payload_word(258,96,done[2]);
    gl_renderer_render_thread_sync("adv1");       /* a sync point mid-payload */
    gl_renderer_render_thread_frame_boundary();
    gl_renderer_render_thread_sync("adv1b");
    check(vram[96*1024+256]==done[0] && vram[96*1024+257]==done[1] && vram[96*1024+258]==done[2],
          "ADV1: payload words written before and after HD activation survive");
    adv_payload_word(259,96,done[3]);
    gr_vram_upload_commit(256,96,4,1,done);
    check(s_hd_native_authority,"ADV1: the commit switches to HD authority");
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"ADV1: the commit releases the context");
    state(); gr_fill_rect(264,96,1,1,0x1111);
    gl_renderer_render_thread_frame_boundary();
    gr_vram_transfer_out(256,96,4,1,(uint16_t[4]){0});
    check(!memcmp(&vram[96*1024+256],done,sizeof(done)) && vram[96*1024+264]==0x1111,
          "ADV1: committed upload stays in native VRAM");
    /* ADV2a: savestate loaded while an A0 is streaming and resumed mid-A0:
     * the full-VRAM section (a 1024x512 TRACK_UPLOAD) then gpu.c's GP0 state. */
    gl_renderer_render_thread_frame_boundary();
    state(); gr_fill_rect(256,100,4,1,0x2345);
    gr_vram_upload_begin(256,100,4,1);
    adv_payload_word(256,100,done[0]);
    memcpy(adv_snapshot,vram,sizeof(adv_snapshot));   /* saved mid-A0 */
    gr_vram_transfer_in(0,0,1024,512,adv_snapshot);
    gr_vram_upload_set_open(1);
    gl_renderer_render_thread_frame_boundary();
    check(rt_held(),"ADV2: full-VRAM load mid-A0 keeps the context");
    adv_payload_word(257,100,done[1]);
    gl_renderer_render_thread_frame_boundary();
    gr_vram_transfer_out(259,100,1,1,&back);
    check(vram[100*1024+256]==done[0] && vram[100*1024+257]==done[1] && back==0x2345,
          "ADV2: payload words after a mid-A0 load survive");
    adv_payload_word(258,100,done[2]); adv_payload_word(259,100,done[3]);
    gr_vram_upload_commit(256,100,4,1,done);
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"ADV2: the commit after a load releases the context");
    /* ADV2b: loaded from an idle, running thread: gpu.c's GP0 state first,
     * then the full-VRAM section, then the payload resumes. */
    state(); gr_fill_rect(256,104,4,1,0x2345);
    gl_renderer_render_thread_frame_boundary();
    gl_renderer_render_thread_sync("adv-save");   /* a save syncs */
    memcpy(adv_snapshot,vram,sizeof(adv_snapshot));
    adv_snapshot[104*1024+256]=done[0];
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"ADV2b: render thread runs before the load");
    gr_vram_upload_set_open(1);
    gr_vram_transfer_in(0,0,1024,512,adv_snapshot);
    gl_renderer_render_thread_frame_boundary();
    check(rt_held(),"ADV2b: a state restored mid-A0 keeps the context");
    adv_payload_word(257,104,done[1]);
    gl_renderer_render_thread_frame_boundary();
    gr_vram_transfer_out(259,104,1,1,&back);
    check(vram[104*1024+256]==done[0] && vram[104*1024+257]==done[1] && back==0x2345,
          "ADV2b: loaded VRAM and resumed payload words survive");
    adv_payload_word(258,104,done[2]); adv_payload_word(259,104,done[3]);
    gr_vram_upload_commit(256,104,4,1,done);
    /* A state restored outside A0 ends a streaming upload's hold. */
    gr_vram_upload_begin(256,108,4,1);
    gr_vram_upload_set_open(0);
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"ADV2: a state restored outside A0 releases the context");
    /* Reverse authority transition: disable HD mid-A0, committing the actual
     * public payload words so a canned buffer cannot hide lost readbacks. */
    state(); gr_fill_rect(288,112,4,1,0x2345);
    gr_vram_upload_begin(288,112,4,1);
    adv_payload_word(288,112,done[0]);
    check(gpu_hd_textures_configure(root,0,0,error,sizeof(error)),"disable HD mid-A0");
    gl_renderer_render_thread_frame_boundary();
    adv_payload_word(289,112,done[1]);
    gl_renderer_render_thread_sync("hd-off-sync");
    gr_vram_transfer_out(291,112,1,1,&back);
    check(vram[112*1024+288]==done[0] && vram[112*1024+289]==done[1] && back==0x2345,
          "HD-off transition preserves partial payload and untouched native words");
    adv_payload_word(290,112,done[2]); adv_payload_word(291,112,done[3]);
    uint16_t streamed[4]; memcpy(streamed,&vram[112*1024+288],sizeof(streamed));
    gr_vram_upload_commit(288,112,4,1,streamed);
    gl_renderer_render_thread_frame_boundary();
    gr_vram_transfer_out(288,112,4,1,streamed);
    check(!memcmp(streamed,done,sizeof(done)),"commit retains streamed words after HD-off");
    /* GP1 reset aborts a deferred HD activation without leaving RT parked. */
    gl_renderer_render_thread_frame_boundary();
    state(); gr_fill_rect(288,116,4,1,0x3456);
    gr_vram_upload_begin(288,116,4,1);
    adv_payload_word(288,116,done[0]);
    check(gpu_hd_textures_configure(root,1,0,error,sizeof(error)),"activate HD during another A0");
    check(s_hd_authority_pending && !s_hd_native_authority,"activation deferred before abort");
    gr_vram_upload_set_open(0);
    gl_renderer_render_thread_frame_boundary();
    check(s_hd_native_authority && !s_hd_authority_pending && !rt_held(),
          "aborted A0 completes deferred authority switch and releases hold");
    gl_renderer_render_thread_stop();
    check(!memcmp(&vram[104*1024+256],done,sizeof(done)),"ADV2: stop keeps the committed upload");
}
static void wait_ready(int st) {
    const int bounds[4]={0,0,3,3};
    int ready=0;
    for(int i=0;i<2000 && !ready;++i) {
        GpuHdTextureImage image={0};
        ready=gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,st,&image);
        gpu_hd_textures_release_image(&image);
        if(!ready) SDL_Delay(1);
    }
    check(ready,"async replacement decode completed");
}
static void capture(void) {
    uint32_t argb[64*64];
    check(gl_renderer_capture_display_hires(argb,64*4,0,0,16,16)==64*64,"high-resolution capture");
    for(int i=0;i<64*64;++i) {
        pixels[i*4]=(uint8_t)(argb[i]>>16); pixels[i*4+1]=(uint8_t)(argb[i]>>8);
        pixels[i*4+2]=(uint8_t)argb[i]; pixels[i*4+3]=(uint8_t)(argb[i]>>24);
    }
}
static const uint8_t* sample(int x,int y) { return pixels+(y*64+x)*4; }

static void composition_scene(const char* root) {
    const uint16_t tp=12u|(2u<<7);
    const int bounds[4]={0,0,7,1};
    state();gr_vram_transfer_in(768,0,8,2,part_words);
    int ready=0;
    for(int i=0;i<2000 && !ready;++i) {
        GpuHdTextureImage image={0};
        if(gpu_hd_textures_acquire_draw(tp,0,0,bounds,0,1,&image))
            ready=image.alpha_mode==4 && image.width==32 && image.height==8 &&
                image.rgba[8*4+2]>200 && image.rgba[8*4+3]==128;
        gpu_hd_textures_release_image(&image);if(!ready) SDL_Delay(1);
    }
    if(!ready) {
        GpuHdTextureImage image={0};
        if(gpu_hd_textures_acquire_draw(tp,0,0,bounds,0,1,&image))
            fprintf(stderr,"composition mode=%u size=%ux%u pixel8=%u,%u,%u,%u\n",image.alpha_mode,image.width,image.height,
                image.rgba[32],image.rgba[33],image.rgba[34],image.rgba[35]);
        gpu_hd_textures_release_image(&image);
    }
    check(ready,"adjacent mixed-density replacements decode and compose");
    gr_fill_rect(0,0,16,16,0x001f);gr_set_semi_transparency(1,1);
    gr_draw_textured_rect(2,2,8,2,0,0,0,0,tp);capture();
    check(sample(8,8)[0]>200 && sample(8,8)[1]<20,"first partial region displays its replacement");
    check(sample(9,8)[0]==0 && sample(9,8)[1]==0 && sample(9,8)[2]==0,
          "composed non-ST alpha128 keeps occupied black");
    check(sample(10,8)[0]>200 && sample(10,8)[1]<20 && sample(10,8)[2]<20,
          "replacement cutout exposes destination rather than native base");
    check(sample(16,8)[0]>200 && sample(16,8)[1]<20 && sample(16,8)[2]>200,
          "ST colored alpha0 blends once without native-base double blend");
    check(sample(18,8)[0]<20 && sample(18,8)[2]>200,"composed ST alpha243 remains opaque");
    check(sample(20,8)[0]>200 && sample(20,8)[2]<20,"composed all-zero ST cutout retains destination");
    check(sample(24,8)[0]>200 && sample(24,8)[1]<20,"native transparent hole remains cutout");
    check(sample(32,8)[0]<20 && sample(32,8)[2]>200,"uncovered native opaque texel retains native color");
    uint16_t output[8];gr_vram_transfer_out(2,2,8,1,output);
    check(output[0]==0x03e0 && output[2]==0x03ff && output[6]==0x7c00,
          "composition does not change guest native draw or STP behavior");
    GpuHdTextureImage first={0},again={0};
    check(gpu_hd_textures_acquire_draw(tp,0,0,bounds,0,1,&first),"composed cache acquire");
    check(gpu_hd_textures_acquire_draw(tp,0,0,bounds,0,1,&again) && first.cache_key==again.cache_key,
          "unchanged native content reuses composition cache");
    const uint64_t old_key=first.cache_key;
    gpu_hd_textures_release_image(&first);gpu_hd_textures_release_image(&again);
    gr_vram_write(775,0,0x7c00);
    check(gpu_hd_textures_acquire_draw(tp,0,0,bounds,0,1,&again) && again.cache_key!=old_key &&
          again.rgba[28*4+2]>200 && again.rgba[28*4+1]<20,
          "native hole write refreshes composed content without touching replacements");
    gpu_hd_textures_release_image(&again);

    /* Reduced palette key uses entries 0..1; native holes use other colors.
     * Updating an unused replacement-key color must still refresh the holes. */
    const uint16_t p4_tp=13;
    const int p4_bounds[4]={0,16,7,16};
    state();gr_vram_transfer_in(0,300,16,1,part_palette);
    gr_vram_transfer_in(832,16,2,1,part_p4_words);
    ready=0;
    for(int i=0;i<2000 && !ready;++i) {
        if(gpu_hd_textures_acquire_draw(p4_tp,0,300,p4_bounds,0,0,&again))
            ready=again.alpha_mode==4 && again.rgba[0]>200 && again.rgba[1]<20;
        gpu_hd_textures_release_image(&again);if(!ready) SDL_Delay(1);
    }
    check(ready,"word-aligned P4 partial replacement composes");
    check(gpu_hd_textures_acquire_draw(p4_tp,0,300,p4_bounds,0,0,&first) && first.rgba[8*4+1]>200 && first.rgba[8*4+3]==128,
          "native P4 hole decodes current CLUT and native STP");
    const uint64_t palette_key=first.cache_key;gpu_hd_textures_release_image(&first);
    gr_vram_write(2,300,0x7c00);
    check(gpu_hd_textures_acquire_draw(p4_tp,0,300,p4_bounds,0,0,&again) && again.cache_key!=palette_key &&
          again.rgba[8*4+2]>200 && again.rgba[8*4+1]<20 && again.rgba[8*4+3]==255,
          "CLUT write outside replacement palette range refreshes native holes");
    gpu_hd_textures_release_image(&again);
    gr_fill_rect(0,0,16,16,0x001f);gr_draw_textured_rect(2,8,8,1,0,16,0,300,p4_tp);capture();
    check(sample(8,32)[0]>200 && sample(24,32)[2]>200 && sample(28,32)[2]>200,
          "P4 replacement and updated native palette holes render together");
    check(vram[8*1024+2]==0x03e0 && vram[8*1024+6]==0x7c00,
          "P4 replacement presentation leaves native VRAM intact");

    /* Adding another lower-density image exercises scale filtering. The
     * native guest still sees blue; the replacement alternates red and blue. */
    DuckTextureKey key={0};key.source_hash=duck_texture_hash_words_le(part_words,16);
    key.source_width_words=8;key.source_height=2;key.offset_x=6;key.width=2;key.height=2;
    key.depth=HD_TEXTURE_DEPTH_16BPP;key.kind=DUCK_TEXTURE_UPLOAD;
    part_png(root,&key,4,4,3);char error[256]={0};
    for(int linear=0;linear<2;++linear) {
        pack_config(root,linear);check(gpu_hd_textures_reload(error,sizeof(error)),"mixed-density filtering reload");
        state();gr_vram_transfer_in(768,0,8,2,part_words);ready=0;
        for(int i=0;i<2000 && !ready;++i) {
            if(gpu_hd_textures_acquire_draw(tp,0,0,bounds,0,0,&again))
                ready=again.alpha_mode==4 && again.width==32 && again.rgba[24*4]>200;
            gpu_hd_textures_release_image(&again);if(!ready) SDL_Delay(1);
        }
        check(ready,"mixed-density scaling fixture ready");
        gr_fill_rect(0,0,16,16,0x03e0);gr_draw_textured_rect(2,2,8,2,0,0,0,0,tp);capture();
        const uint8_t* scaled=sample(33,8);
        check(linear ? scaled[0]>170 && scaled[0]<220 && scaled[2]>40 && scaled[2]<90 :
              scaled[0]>240 && scaled[2]<20,"root scale-filter option changes lower-density replacement colors");
        check(vram[2*1024+8]==0x7c00,"replacement scaling filter never changes native VRAM");
    }
}

static void cache_eviction_scene(const char* root) {
    uint16_t pressure_words[16]; for(int i=0;i<16;++i) pressure_words[i]=0x1234;
    DuckTextureKey key={0}; key.kind=DUCK_TEXTURE_UPLOAD;key.depth=HD_TEXTURE_DEPTH_16BPP;
    key.source_hash=duck_texture_hash_words_le(pressure_words,16);
    key.source_width_words=key.width=16; key.source_height=key.height=1;
    char stem[256],path[2048],error[256]={0};
    check(duck_texture_format_name(&key,stem,sizeof(stem)),"eviction fixture identity");
    snprintf(path,sizeof(path),"%s/replacements/%s.png",root,stem);
    uint8_t* rgba=(uint8_t*)malloc(1024*256*4);memset(rgba,255,1024*256*4);
    FILE* file=fopen(path,"wb");check(file!=NULL,"eviction fixture PNG opens");
    if(file){check(png_write_rgba(file,rgba,1024,256),"eviction fixture PNG writes");fclose(file);}free(rgba);
    pack_config(root,0);check(gpu_hd_textures_reload(error,sizeof(error)),"eviction fixture reload");
    state();gr_vram_transfer_in(512,0,4,4,source_words);gr_vram_transfer_in(768,0,8,2,part_words);
    wait_ready(0);wait_ready(1);
    const int bounds[4]={0,0,3,3},parts[4]={0,0,7,1},pressure[4]={0,64,15,64};
    const uint16_t tp=12u|(2u<<7);GpuHdTextureImage image={0};uint64_t composed_key=0;
    for(int i=0;i<2000 && !composed_key;++i) {
        if(gpu_hd_textures_acquire_draw(tp,0,0,parts,0,1,&image) && image.width==32 && image.rgba[24*4]>200)
            composed_key=image.cache_key;
        gpu_hd_textures_release_image(&image);if(!composed_key) SDL_Delay(1);
    }
    check(composed_key!=0,"complete composition ready before decoder eviction");
    gr_fill_rect(0,0,16,16,0x03e0);gr_draw_textured_rect(2,2,4,4,0,0,0,0,texture_page);capture();
    GpuHdTextureDiag before,after;gpu_hd_textures_get_diag(&before);
    gr_vram_transfer_in(640,64,16,1,pressure_words);int ready=0;
    for(int i=0;i<2000 && !ready;++i) {
        ready=gpu_hd_textures_acquire_draw(10u|(2u<<7),0,0,pressure,0,0,&image);
        gpu_hd_textures_release_image(&image);if(!ready) SDL_Delay(1);
    }
    check(ready,"full CPU budget pressure image decoded");gpu_hd_textures_get_diag(&after);
    check(after.decoded_bytes==1024*1024 && after.decode_evictions>before.decode_evictions,
          "pressure image evicts original decoded replacements");
    check(gpu_hd_textures_acquire_gl_draw(texture_page,0,0,bounds,0,0,&image) && !image.rgba,
          "GPU-resident replacement remains ready after CPU eviction");gpu_hd_textures_release_image(&image);
    check(gpu_hd_textures_acquire_draw(tp,0,0,parts,0,1,&image) && image.cache_key==composed_key,
          "complete composition survives eviction of all source PNGs");gpu_hd_textures_release_image(&image);
    gr_draw_textured_rect(2,2,4,4,0,0,0,0,texture_page);capture();
    check(sample(8,8)[0]>200,"GPU-resident replacement still draws original HD detail");
    SDL_Delay(20);gpu_hd_textures_get_diag(&before);
    check(before.decoded_images==after.decoded_images && before.gl_uploads==after.gl_uploads,
          "warm draws do not decode or upload again after CPU eviction");
    pack_config(root,0);check(gpu_hd_textures_reload(error,sizeof(error)),"eviction fixture cleanup reload");
    state();gr_vram_transfer_in(512,0,4,4,source_words);wait_ready(0);wait_ready(1);
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--native-baseline")) {
        char* native_args[]={"hd-baseline","4","twin","1"};
        return scale_fixture_main(4,native_args);
    }
    if(argc!=2) return 2;
    for(int i=0;i<16;++i) source_words[i]=i%4==3?0:i%4==2?0x8000:i%4==1?0x83e0:0x03e0;
    pack_png(argv[1],0); pack_png(argv[1],1);pack_parts(argv[1]);pack_config(argv[1],0);
    gr_set_backend(GR_BACKEND_SOFTWARE); gr_init(reference);
    sw_set_faithful_authority(1); native_scene(); sw_set_faithful_authority(0);
    if(SDL_Init(SDL_INIT_VIDEO)!=0) return 2;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_Window* win=SDL_CreateWindow("HD pack regression",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win) return 2;
    gr_set_backend(GR_BACKEND_OPENGL); gr_init(vram); gr_set_scale(4);
    char error[512]={0};
    check(gpu_hd_textures_configure(argv[1],1,1,error,sizeof(error)),error);
    gl_renderer_set_swap_interval(0);
    if(!gl_renderer_init_context(win)) return 2;
    if(s_hiw) check(hiw_ensure(0,64)!=NULL,"high-resolution window allocated before draws");
    state(); gr_vram_transfer_in(512,0,4,4,source_words);
    wait_ready(0); wait_ready(1);
    check(gpu_hd_textures_reload(error,sizeof(error)),"live pack reload succeeds");
    /* No new upload: a reload must retain identities of resident textures. */
    wait_ready(0); wait_ready(1);
    state();gr_copy_rect(512,0,540,0,4,4);
    const int copied_bounds[4]={28,0,31,3};GpuHdTextureImage copied={0};
    check(!gpu_hd_textures_acquire_draw(texture_page,0,0,copied_bounds,0,0,&copied),
          "default VRAM copy does not invent source upload provenance at destination");
    gpu_hd_textures_release_image(&copied);
    gr_fill_rect(540,0,4,4,0);
    native_scene(); gl_renderer_sync_cpu();
    check(memcmp(vram,reference,sizeof(vram))==0,"HD native VRAM matches faithful software across all operations");
    uint16_t readback[64*64];
    gr_vram_transfer_out(0,0,64,64,readback);
    int unchanged=1;
    for(int y=0;y<64;++y) for(int x=0;x<64;++x) unchanged&=readback[y*64+x]==reference[y*1024+x];
    check(unchanged,"guest GPUREAD stays native with replacements");

    /* Source rectangle is four native texels, replacement is sixteen. The
     * first native texel contains alternating red and blue HD columns. */
    state(); gr_fill_rect(0,0,16,16,0x03e0);
    gr_draw_textured_rect(4,4,4,4,0,0,0,0,texture_page); capture();
    const uint8_t* a=sample(16,16); const uint8_t* b=sample(17,16);
    check(a[0]>200 && a[2]<20 && b[2]>200 && b[0]<20,
          "replacement detail survives inside one native texel at 4x");
    check(vram[4*1024+4]==0x03e0,"presentation red/blue never enters guest VRAM");
    check(sample(16,24)[1]>200,"non-ST alpha127 cuts out black");
    check(sample(17,24)[0]==0 && sample(17,24)[1]==0 && sample(17,24)[2]==0,
          "non-ST alpha128 retains opaque black");

    /* ST alpha bytes encode PSX classes, including alpha0 with colored RGB.
     * Mode 1 is additive, so the green destination survives STP texels. */
    state(); gr_fill_rect(0,0,16,16,0x03e0); gr_set_semi_transparency(1,1);
    gr_draw_textured_rect(4,4,4,4,0,0,0,0,texture_page); capture();
    check(sample(16,16)[0]>200 && sample(16,16)[1]>200,"ST colored alpha0 stays semitransparent");
    check(sample(19,16)[1]>200,"ST alpha242 stays semitransparent");
    check(sample(17,16)[1]>200 && sample(18,16)[1]>200,"ST alpha127 and alpha128 stay semitransparent");
    check(sample(20,16)[0]>200 && sample(20,16)[1]<20,"ST alpha243 is opaque");
    check(sample(21,16)[2]>200 && sample(21,16)[1]<20,"ST alpha255 is opaque");
    check(sample(16,24)[1]>200,"ST black RGBA zero cuts out");
    check(sample(20,24)[1]>200,"ST opaque black follows native-zero cutout");
    check(sample(17,24)[0]==0 && sample(17,24)[1]>200 && sample(17,24)[2]==0,
          "ST black alpha127 remains occupied semitransparent black");
    gr_set_semi_transparency(0,0);
    composition_scene(argv[1]);state();wait_ready(0);wait_ready(1);
    cache_eviction_scene(argv[1]);

    /* A queued draw must consume its source before a later source overwrite.
     * The next draw falls back because upload identity has been invalidated. */
    gr_draw_textured_rect(0,0,4,4,0,0,0,0,texture_page);
    uint16_t overwrite[16]; for(int i=0;i<16;++i) overwrite[i]=0x7c00;
    gr_vram_transfer_in(512,0,4,4,overwrite);
    gr_draw_textured_rect(8,0,4,4,0,0,0,0,texture_page); capture();
    check(sample(0,0)[0]>200,"queued replacement retained before source overwrite");
    check(sample(32,0)[2]>200 && sample(32,0)[0]<20,"next draw sees new native source");
    check(vram[8]==0x7c00,"new source word retained natively");
    state(); gr_vram_transfer_in(512,0,4,4,source_words);
    gr_draw_textured_rect(513,0,3,1,0,0,0,0,texture_page);
    gr_vram_transfer_out(512,0,4,1,readback);
    check(readback[0]==0x03e0 && readback[1]==0x03e0 && readback[2]==0x03e0 && readback[3]==0x03e0,
          "self-overlap keeps sequential native texture reads");
    const int bounds[4]={0,0,3,3}; GpuHdTextureImage lease={0};
    check(gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease) && lease.alpha_mode==4 &&
          lease.rgba[4*4+1]>200 && lease.rgba[0]>200,
          "self-overlap preserves surviving replacement regions with new native holes");
    gpu_hd_textures_release_image(&lease);
    gr_vram_transfer_in(512,0,4,4,source_words);
    check(gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "upload is resident before an interrupted A0");
    gpu_hd_textures_release_image(&lease);
    gr_vram_upload_begin(512,0,4,4);
    check(!gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "A0 header invalidates identity even if payload is later aborted");
    gpu_hd_textures_release_image(&lease);
    gr_vram_upload_set_open(0);   /* GP1(01h) aborts it */
    gr_vram_transfer_in(512,0,4,4,source_words);
    gl_renderer_restage_vram_after_savestate();
    check(!gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "savestate restage cannot fabricate original upload identity");
    gpu_hd_textures_release_image(&lease);
    gr_vram_transfer_in(512,0,4,4,source_words);
    check(gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&lease),
          "fresh upload restores identity after state load");
    gpu_hd_textures_release_image(&lease);
    check(!gl_renderer_texture_banks_supported(),"texture banks explicitly refuse HD authority");
    check(gl_renderer_pass_unavailable()!=PSX_MOD_RENDER_PASS_READY,"passes refuse HD authority");
    check(gl_renderer_stereo_unavailable()!=PSX_MOD_RENDER_PASS_READY,"stereo refuses HD authority");
    GpuHdTextureDiag diag; gpu_hd_textures_get_diag(&diag);
    check(diag.applied_draws>0,"replacement diagnostics record actual GL submissions");
    /* Preserve the established Beetle numeric-key path with source origin
     * offset, native cutout and native STP authority. */
    char beetle_root[2048],beetle_png[2304],hashes[2304];
    snprintf(beetle_root,sizeof(beetle_root),"%s/beetle",argv[1]);
    snprintf(hashes,sizeof(hashes),"%s/Hashes.ini",beetle_root);
    FILE* hf=fopen(hashes,"wb"); if(hf) fclose(hf);
    snprintf(beetle_png,sizeof(beetle_png),"%s/demo-texture-replacements/%x-0.png",beetle_root,
             hd_texture_crc32_words_le(source_words,16));
    uint8_t beetle_rgba[16*16*4];
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) {
        uint8_t* at=beetle_rgba+(y*16+x)*4;
        at[0]=x<4?255:0; at[1]=x>=8?255:0; at[2]=x>=4&&x<8?255:0; at[3]=255;
    }
    FILE* bf=fopen(beetle_png,"wb"); check(bf!=NULL,"Beetle PNG open");
    if(bf){check(png_write_rgba(bf,beetle_rgba,16,16),"Beetle PNG fixture");fclose(bf);}
    check(gpu_hd_textures_configure(beetle_root,1,0,error,sizeof(error)),"Beetle session opens");
    state(); gr_vram_transfer_in(512,0,4,4,source_words);
    wait_ready(0);
    gr_vram_transfer_in(516,0,4,4,source_words);
    gr_fill_rect(0,0,16,16,0x03e0);
    gr_draw_textured_rect(4,4,2,4,5,0,0,0,texture_page); capture();
    check(sample(16,16)[2]>200 && sample(16,16)[0]<20,"Beetle partial draw keeps upload-relative UV origin");
    check(vram[4*1024+4]==0x03e0 && vram[517]==0x83e0,
          "Beetle image preserves native draw RGB and source STP");
    gr_draw_textured_rect(8,4,4,4,4,0,0,0,texture_page); capture();
    check(sample(44,16)[1]>200,"Beetle native transparent zero still cuts out replacement");
    /* Render thread: residency follows the recorded command stream and the
     * render thread's VRAM copy. The emulation thread writes its array and
     * records the overwrite before the first draw has been replayed; that
     * draw must still see the upload it was recorded after. */
    check(gl_renderer_render_thread_start(2)==1,"render thread started with an HD session");
    for(int frame=0;frame<3;++frame) {
        state(); gr_fill_rect(0,0,16,16,0x03e0);
        for(int i=0;i<16;++i) vram[i/4*1024+512+i%4]=source_words[i];
        gr_vram_transfer_in(512,0,4,4,source_words);
        gr_draw_textured_rect(4,4,4,4,0,0,0,0,texture_page);
        for(int i=0;i<16;++i) vram[i/4*1024+512+i%4]=overwrite[i];
        gr_vram_transfer_in(512,0,4,4,overwrite);
        gr_draw_textured_rect(8,4,4,4,0,0,0,0,texture_page);
        gl_renderer_render_thread_frame_boundary();
    }
    capture();
    check(sample(16,16)[0]>200 && sample(16,16)[1]<20,"render thread: replacement drawn from the upload recorded before it");
    check(sample(32,16)[2]>200 && sample(32,16)[0]<20,"render thread: later overwrite draws the new native source");
    /* HD authority rasterizes native VRAM on the render thread's copy; a
     * guest readback (a sync point) must see it in gpu.c's array. */
    gl_renderer_render_thread_frame_boundary();   /* the capture held the context */
    check(!rt_held(),"render thread owns the context again");
    state(); gr_fill_rect(96,96,4,4,0x1234);
    gr_draw_flat_rect(104,96,4,4,0x0421);
    gl_renderer_render_thread_frame_boundary();
    uint16_t back[2]={0,0};
    gr_vram_transfer_out(96,96,1,1,&back[0]); gr_vram_transfer_out(104,96,1,1,&back[1]);
    check(back[0]==0x1234 && back[1]==0x0421 && vram[96*1024+96]==0x1234 && vram[96*1024+104]==0x0421,
          "render thread: guest readback sees native draws under HD authority");
    /* Review reproduction (#568): a pending A0 keeps only received payload
     * words, its mask check sees the prior native draw, and stop publishes
     * the final HD-authoritative draws. */
    gl_renderer_render_thread_frame_boundary();
    state(); gr_fill_rect(128,96,4,1,0x2345);
    gr_vram_upload_begin(128,96,4,1);
    vram[96*1024+128]=0x7c00;
    uint16_t partial_read=0;
    gr_vram_transfer_out(129,96,1,1,&partial_read);
    check(vram[96*1024+128]==0x7c00,"received A0 word survives");
    check(partial_read==0x2345,"unwritten A0 word retains earlier draw");
    uint16_t completed[4]={0x7c00,0x2345,0x2345,0x2345};
    gr_vram_upload_commit(128,96,4,1,completed);
    gl_renderer_render_thread_frame_boundary();
    state(); gr_set_mask_bits(1,0); gr_draw_flat_rect(192,96,4,1,0x0421);
    gr_set_mask_bits(0,1); gr_vram_upload_begin(192,96,4,1);
    check(gr_vram_read(192,96)==0x8421,"A0 sees prior native mask");
    uint16_t masked[4]={0x8421,0x8421,0x8421,0x8421};
    gr_vram_upload_commit(192,96,4,1,masked);
    gl_renderer_render_thread_frame_boundary();
    state(); gr_fill_rect(160,96,4,1,0x4567);
    gl_renderer_render_thread_frame_boundary();
    gl_renderer_render_thread_stop();
    check(vram[96*1024+160]==0x4567,"stop publishes final native draw");
    /* #570's HD safety cases, with the render thread running instead of
     * parked: live activation, partial upload across a frame boundary and a
     * reload, reset after an incomplete upload, dump-only, disable/resume,
     * and the final native draw at stop. */
    gpu_hd_textures_shutdown();
    check(gl_renderer_render_thread_start(2),"render thread starts with HD disabled");
    gl_renderer_set_frame_generation(1);
    gl_renderer_render_thread_frame_boundary();
    state(); gr_fill_rect(96,96,4,1,0x1234);
    gl_renderer_render_thread_frame_boundary();
    check(gpu_hd_textures_configure(beetle_root,1,0,error,sizeof(error)),"HD opens while render thread is running");
    check(vram[96*1024+96]==0x1234,"HD activation retains the queued native draw");
    char fg_diag[4096]; gl_renderer_frame_gen_json(fg_diag,sizeof(fg_diag));
    check(strstr(fg_diag,"\"active\":1")!=NULL,"Smooth motion stays active under HD authority");
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"HD replacement mode runs on the render thread");
    state(); gr_fill_rect(128,96,4,1,0x2345);
    gr_vram_upload_begin(128,96,4,1);
    vram[96*1024+128]=0x7c00; /* A partial GP0(A0) payload, as gpu.c writes it. */
    gl_renderer_render_thread_frame_boundary();   /* the payload spans a vblank */
    check(rt_held(),"an open A0 keeps the context across a frame boundary");
    partial_read=0; gr_vram_transfer_out(129,96,1,1,&partial_read);
    check(vram[96*1024+128]==0x7c00 && partial_read==0x2345,
          "partial upload preserves payload and preceding native draws");
    check(gpu_hd_textures_reload(error,sizeof(error)),"HD reload is safe during a partial upload");
    check(vram[96*1024+128]==0x7c00 && vram[96*1024+129]==0x2345,
          "HD reload retains partial-upload VRAM");
    gr_init(vram); /* reset abandons the incomplete upload */
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"reset after an incomplete upload releases the context");
    state(); gr_fill_rect(128,96,4,1,0x3456);
    gr_vram_transfer_out(128,96,1,1,&partial_read);
    check(partial_read==0x3456,"reset after an incomplete upload keeps native draws visible");
    gl_renderer_render_thread_frame_boundary();
    state(); gr_set_mask_bits(1,0); gr_draw_flat_rect(192,96,4,1,0x0421);
    gr_set_mask_bits(0,1); gr_vram_upload_begin(192,96,4,1);
    check(gr_vram_read(192,96)==0x8421,"A0 mask check sees the preceding masked native draw");
    gr_vram_upload_commit(192,96,4,1,masked);
    state();
    gpu_hd_textures_shutdown();
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"render thread runs after HD replacements are disabled");
    state(); gr_fill_rect(160,96,4,1,0x4567);
    gl_renderer_render_thread_frame_boundary();
    gr_vram_transfer_out(160,96,1,1,&partial_read);
    check(partial_read==0x4567 && vram[96*1024+160]==0x4567,"resumed rendering retains native readbacks");
    gl_renderer_render_thread_frame_boundary();
    check(gpu_hd_textures_configure(argv[1],0,1,error,sizeof(error)),"dump-only session opens with render thread running");
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"dump-only mode runs on the render thread");
    state(); gr_vram_upload_begin(512,0,4,4);
    for(int i=0;i<16;++i) vram[i/4*1024+512+i%4]=source_words[i];
    gr_vram_upload_commit(512,0,4,4,source_words);
    gr_draw_textured_rect(8,8,4,4,0,0,0,0,texture_page);
    gl_renderer_render_thread_frame_boundary();
    gr_vram_transfer_out(8,8,1,1,&partial_read);
    check(partial_read==source_words[0] && vram[8*1024+8]==source_words[0],"dump-only draw retains native VRAM");
    gpu_hd_textures_set_dump_enabled(0);
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"render thread runs when dumping is disabled");
    gpu_hd_textures_shutdown();
    gl_renderer_set_frame_generation(0);
    check(gpu_hd_textures_configure(beetle_root,1,0,error,sizeof(error)),"HD session restored for teardown checks");
    gl_renderer_render_thread_frame_boundary();
    state(); gr_fill_rect(224,96,4,1,0x5678);
    gl_renderer_render_thread_frame_boundary();
    gl_renderer_render_thread_stop();
    check(!gl_renderer_render_thread_active(),"thread stops safely after HD transitions");
    check(vram[96*1024+224]==0x5678,"stopping the thread preserves final HD native draws");
    /* Restage (savestate, rewind): an upload rectangle seen this session comes
     * back only while its restored words still carry the replacement key. */
    {
        GpuHdTextureImage img={0};
        state(); gr_vram_transfer_in(512,0,4,4,source_words);   /* resident */
        wait_ready(0);   /* the teardown session above decodes afresh */
        gl_renderer_restage_vram_after_savestate();
        check(gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&img),"restage re-admits an unchanged learned upload");
        gpu_hd_textures_release_image(&img);
        check(gpu_hd_textures_reload(error,sizeof(error)),"same-pack reload before rewind");
        wait_ready(0);
        gl_renderer_restage_vram_after_savestate();
        check(gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&img),"same-pack reload retains learned upload for restage");
        gpu_hd_textures_release_image(&img);
        state(); gr_fill_rect(0,0,16,16,0x001f);
        gr_draw_textured_rect(4,4,4,4,0,0,0,0,texture_page); capture();
        check(sample(16,16)[0]>200 && sample(16,16)[1]<20,"reload then restage still draws HD replacement pixels");
        vram[512]^=1u; gl_renderer_restage_vram_after_savestate();
        check(!gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&img),"restage refuses a learned rectangle whose words changed");
        gpu_hd_textures_release_image(&img);
        vram[512]^=1u; gl_renderer_restage_vram_after_savestate();
    }
    /* Savestate sidecar: residency saved with the VRAM it describes restores a
     * fresh session (a cold load) over exactly that VRAM, and only then. */
    {
        uint8_t* res=NULL; size_t res_len=0; GpuHdTextureImage img={0};
        check(gpu_hd_textures_residency_save(&res,&res_len) && res_len>16,"residency saved");
        gpu_hd_textures_shutdown();
        /* Another pack (one more replacement key) over the same VRAM: the
         * residency names this pack's uploads, so it is refused there. */
        char other_root[2304],other_png[2400];
        snprintf(other_root,sizeof(other_root),"%s/beetle-other",argv[1]);
        snprintf(hashes,sizeof(hashes),"%s/Hashes.ini",other_root);
        hf=fopen(hashes,"wb"); if(hf) fclose(hf);
        const uint32_t other_keys[2]={hd_texture_crc32_words_le(source_words,16),0x12345678u};
        for(int k=0;k<2;++k) {
            snprintf(other_png,sizeof(other_png),"%s/demo-texture-replacements/%x-0.png",other_root,other_keys[k]);
            bf=fopen(other_png,"wb");
            if(bf){check(png_write_rgba(bf,beetle_rgba,16,16),"other Beetle PNG fixture");fclose(bf);}
        }
        check(gpu_hd_textures_configure(other_root,1,0,error,sizeof(error)),"other Beetle pack opens");
        gl_renderer_restage_vram_after_savestate();
        check(!gpu_hd_textures_residency_load(res,res_len),"residency refused with a different pack");
        gpu_hd_textures_shutdown();
        check(gpu_hd_textures_configure(beetle_root,1,0,error,sizeof(error)),"fresh Beetle session");
        gl_renderer_restage_vram_after_savestate();
        check(!gpu_hd_textures_acquire_draw(texture_page,0,0,bounds,0,0,&img),"a fresh session knows no upload");
        gpu_hd_textures_release_image(&img);
        static uint16_t saved_vram[1024*512];
        memcpy(saved_vram,vram,sizeof(saved_vram));
        state(); gr_fill_rect(0,0,16,16,0x001f);
        gr_draw_textured_rect(4,4,4,4,0,0,0,0,texture_page); capture();
        check(sample(16,16)[0]<20 && sample(16,16)[1]>200,"before the load the draw shows native artwork");
        /* Load the state again: the VRAM it was saved with, restaged. */
        memcpy(vram,saved_vram,sizeof(saved_vram));
        gl_renderer_restage_vram_after_savestate();
        res[16]^=0xffu;
        check(!gpu_hd_textures_residency_load(res,res_len),"corrupt residency refused");
        res[16]^=0xffu;
        vram[300]^=1u;
        check(!gpu_hd_textures_residency_load(res,res_len),"residency refused over different VRAM");
        vram[300]^=1u;
        check(gpu_hd_textures_residency_load(res,res_len),"residency restored over identical VRAM");
        wait_ready(0);
        /* A later textured draw of that upload uses the HD replacement (red
         * HD columns over native green), counted as an applied draw. */
        GpuHdTextureDiag before_draw,after_draw;
        gpu_hd_textures_get_diag(&before_draw);
        state(); gr_fill_rect(0,0,16,16,0x001f);
        gr_draw_textured_rect(4,4,4,4,0,0,0,0,texture_page); capture();
        gpu_hd_textures_get_diag(&after_draw);
        check(sample(16,16)[0]>200 && sample(16,16)[1]<20,"after the load a textured draw shows the HD replacement");
        check(after_draw.applied_draws>before_draw.applied_draws,"the restored upload's draw applied a replacement");
        free(res);
    }
    adv_hd_cases(beetle_root);
    /* Smooth motion with an HD pack: the same 30 Hz game (each frame shown
     * twice) with generation off, then forced on. Real frames and native VRAM
     * must be identical, and generated frames show the HD HUD. */
    {
        uint64_t seq[3]={0,0,0}, nreal[3]={0,0,0}, vd=0;
        static uint16_t vsnap[1024*512];
        state(); gr_vram_transfer_in(512,0,4,4,source_words); wait_ready(0);   /* decode before the thread owns residency */
        check(gl_renderer_render_thread_start(2)==1,"render thread started for Smooth motion + HD");
        /* run 0 warms every surface the scene touches (both buffers), so
         * runs 1 (off) and 2 (on) start from the same renderer state. */
        for(int run=0;run<3;++run) {
            gl_renderer_render_thread_frame_boundary();
            state(); gr_vram_transfer_in(512,0,4,4,source_words);
            if(run==2) fg_force_env(1);
            gl_renderer_set_frame_generation(run==2);
            gl_renderer_frame_gen_configure(120.0,59.94);
            fg_real_seq=0xcbf29ce484222325ull; fg_n_real=fg_n_gen=fg_last_real=0;
            fg_gen_hud_hd=fg_gen_hud_bad=fg_real_hud_hd=0;
            fg_rec=1;
            for(int g=0;g<16;++g) {
                const int bx=(g&1)?256:0;
                state(); gr_set_draw_area(bx,0,bx+63,63);
                gr_fill_rect(bx,0,64,64,0x1084);
                gr_draw_flat_rect(bx+20+g,30,8,8,0x7fff);
                /* GTE-projected scenery panning with the camera (projection
                 * sources, as gpu.c records them): reprojection's input. */
                for(int i=0;i<12;++i) {
                    const int x=bx+2+(i%4)*15+g%4, y=14+(i/4)*16;
                    uint32_t id[3]; int32_t pc[9], hh[3], xs[3]={x,x+10,x}, ys[3]={y,y,y+10};
                    for(int k=0;k<3;++k){ id[k]=(uint32_t)(i*3+k+1); pc[3*k]=xs[k]-bx; pc[3*k+1]=ys[k]; pc[3*k+2]=1000; hh[k]=1000; }
                    gl_renderer_fg_source(id,pc,hh,xs,ys);
                    gr_draw_flat_triangle(xs[0],ys[0],xs[1],ys[1],xs[2],ys[2],(uint16_t)(0x0842*(i%3+1)));
                }
                gr_draw_textured_rect(bx+4,4,4,4,0,0,0,0,texture_page);
                for(int k=0;k<2;++k) {
                    gl_renderer_present_vram(bx,0,64,64,0,0);
                    gl_renderer_render_thread_frame_boundary();
                }
            }
            gl_renderer_render_thread_sync("fg-hd");
            fg_rec=0;
            seq[run]=fg_real_seq; nreal[run]=fg_n_real;
            if(run==1) memcpy(vsnap,vram,sizeof(vsnap));
            if(run==2) vd=memcmp(vsnap,vram,sizeof(vsnap));
            if(run==2) printf("fg-hd: real=%llu generated=%llu gen_hud_hd=%d gen_hud_bad=%d real_hud_hd=%d\n",
                (unsigned long long)fg_n_real,(unsigned long long)fg_n_gen,fg_gen_hud_hd,fg_gen_hud_bad,fg_real_hud_hd);
        }
        fg_force_env(0);
        gl_renderer_set_frame_generation(0);
        gl_renderer_render_thread_stop();
        check(nreal[1]>=8 && seq[1]==seq[2] && nreal[1]==nreal[2],"Smooth motion + HD: real frames identical");
        check(vd==0,"Smooth motion + HD: generated frames write no native VRAM");
        check(fg_real_hud_hd>0,"Smooth motion + HD: real HUD is the HD replacement");
        if(!s_hiw) {
            check(fg_n_gen>0,"Smooth motion + HD: in-between frames generated");
            check(fg_gen_hud_hd>0 && fg_gen_hud_bad==0,"Smooth motion + HD: generated HUD is the HD replacement");
        }
    }
    memcpy(reference,vram,sizeof(vram));
    gl_renderer_set_cpu_auth_dual(1);
    gpu_hd_textures_shutdown();
    check(!s_hd_native_authority,"session teardown clears independent authority");
    check(gl_renderer_cpu_auth_dual(),"HD teardown preserves netplay authority");
    check(memcmp(reference,vram,sizeof(vram))==0,"teardown cannot read presentation pixels into native VRAM");
    gl_renderer_set_cpu_auth_dual(0);
    check(gl_renderer_texture_banks_supported(),"session teardown restores backend capabilities");
    check(glGetError()==GL_NO_ERROR,"HD GL errors");
    printf("scale=%d checks=%d failures=%d applied=%llu dumps=%llu\n",gr_scale(),checks,failures,
           (unsigned long long)diag.applied_draws,(unsigned long long)diag.dumped_textures);
    gl_renderer_shutdown(); SDL_DestroyWindow(win); SDL_Quit();
    return failures?1:0;
}
