/* Real GL + real format/decoder + renderer facade. Source-owned fixtures. */
#define PSX_TEST_HD_TEXTURE_PACK 1
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
    if(file){fprintf(file,"DumpC16Textures: true\nMaxVRAMWriteSplits: 16\nReplacementScaleLinearFilter: %s\n",linear?"true":"false");fclose(file);}
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
    /* HD residency stays synchronous: startup refuses the combination, and
     * live activation parks an existing thread before switching authority. */
    check(!gl_renderer_render_thread_start(2),"HD replacements refuse render-thread startup");
    /* Keep the negative-control run safe when checking an unfixed renderer. */
    if(gl_renderer_render_thread_active()) gl_renderer_render_thread_stop();
    gpu_hd_textures_shutdown();
    check(gl_renderer_render_thread_start(2),"render thread starts with HD disabled");
    gl_renderer_set_frame_generation(1);
    gl_renderer_render_thread_frame_boundary();
    state(); gr_fill_rect(96,96,4,1,0x1234);
    gl_renderer_render_thread_frame_boundary();
    check(gpu_hd_textures_configure(beetle_root,1,0,error,sizeof(error)),"HD opens while render thread is running");
    check(rt_held(),"HD activation takes and holds the GL context");
    check(vram[96*1024+96]==0x1234,"HD activation retains the queued native draw");
    char fg_diag[4096]; gl_renderer_frame_gen_json(fg_diag,sizeof(fg_diag));
    check(strstr(fg_diag,"\"active\":0")!=NULL,"Smooth motion is inactive under HD authority");
    gl_renderer_render_thread_frame_boundary();
    check(rt_held(),"HD replacement mode remains synchronous across frames");
    state(); gr_fill_rect(128,96,4,1,0x2345);
    gr_vram_upload_begin(128,96,4,1);
    vram[96*1024+128]=0x7c00; /* A partial GP0(A0) payload, as gpu.c writes it. */
    uint16_t partial_read=0; gr_vram_transfer_out(129,96,1,1,&partial_read);
    check(vram[96*1024+128]==0x7c00 && partial_read==0x2345,
          "partial upload preserves payload and preceding native draws");
    check(gpu_hd_textures_reload(error,sizeof(error)),"HD reload is safe while the thread is parked");
    check(rt_held() && vram[96*1024+128]==0x7c00 && vram[96*1024+129]==0x2345,
          "HD reload retains partial-upload VRAM");
    /* Aborted uploads have no extra private-copy tracking to leave behind. */
    gr_init(vram); state(); gr_fill_rect(128,96,4,1,0x3456);
    gr_vram_transfer_out(128,96,1,1,&partial_read);
    check(partial_read==0x3456,"reset after an incomplete upload keeps native draws visible");
    gr_set_mask_bits(1,0); gr_draw_flat_rect(192,96,4,1,0x0421);
    gr_set_mask_bits(0,1); gr_vram_upload_begin(192,96,4,1);
    check(gr_vram_read(192,96)==0x8421,
          "A0 mask check sees the preceding masked native draw");
    state();
    gpu_hd_textures_shutdown();
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"render thread resumes after HD replacements are disabled");
    state(); gr_fill_rect(160,96,4,1,0x4567);
    gl_renderer_render_thread_frame_boundary();
    gr_vram_transfer_out(160,96,1,1,&partial_read);
    check(partial_read==0x4567 && vram[96*1024+160]==0x4567,
          "resumed rendering retains native readbacks");
    gl_renderer_render_thread_frame_boundary();
    check(gpu_hd_textures_configure(argv[1],0,1,error,sizeof(error)),"dump-only session opens with render thread running");
    check(rt_held(),"texture dumping also parks the render thread");
    gl_renderer_render_thread_frame_boundary();
    check(rt_held(),"dump-only mode remains synchronous across frames");
    gr_vram_transfer_in(512,0,4,4,source_words);
    gr_draw_textured_rect(8,8,4,4,0,0,0,0,texture_page); capture();
    check(vram[8*1024+8]==source_words[0],"dump-only draw retains native VRAM");
    gpu_hd_textures_set_dump_enabled(0);
    gl_renderer_render_thread_frame_boundary();
    check(!rt_held(),"render thread resumes when dumping is disabled");
    gpu_hd_textures_shutdown();
    gl_renderer_set_frame_generation(0);
    check(gpu_hd_textures_configure(beetle_root,1,0,error,sizeof(error)),"HD session restored for teardown checks");
    state(); gr_fill_rect(224,96,4,1,0x5678);
    gl_renderer_render_thread_frame_boundary();
    gl_renderer_render_thread_stop();
    check(!gl_renderer_render_thread_active(),"thread stops safely after HD transitions");
    check(vram[96*1024+224]==0x5678,"stopping the parked thread preserves final HD native draws");
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
