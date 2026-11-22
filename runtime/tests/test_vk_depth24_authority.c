/* Real hidden Vulkan device, production renderer/native raster/scanout decoder,
 * and authored RGB888/texture words. Only frontend providers are substituted. */
#define PSX_HAVE_VULKAN 1
#ifndef PSX_VK_RENDERER_SOURCE
#define PSX_VK_RENDERER_SOURCE "../src/gpu_vk_renderer.c"
#endif
#include PSX_VK_RENDERER_SOURCE
#include <assert.h>
static uint16_t vram[1024*512],peek[128*64];
void gpu_depth24_upload_span_reset(void){}
static int failures;
static void check(int value,const char* what){if(!value){fprintf(stderr,"FAIL: %s\n",what);++failures;}}
static int fixture_peek(int x,int y,int w,int h,uint16_t* out){vkb_vram_transfer_out(x,y,w,h,out);return 1;}
size_t vk_pipeline_file_read(const char*a,const char*b,void*c,size_t d){(void)a;(void)b;(void)c;(void)d;return 0;}
void vk_pipeline_file_write(const char*a,const char*b,const void*c,size_t d){(void)a;(void)b;(void)c;(void)d;}
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a){(void)n;(void)a;return 0;}
/* mod_texture_banks.c refuses inside a netplay host-output hook (mod_runtime.cpp); none here. */
int psx_mod_host_output_scope(void){return 0;}
uint32_t psx_mod_read_word(uint32_t a){(void)a;return 0;}
int g_psx_vram_dirty_tracking=0;
int g_psx_render_pass_active=0; /* standalone fixture has no title render pass */
uint64_t s_frame_count=0;
void gpu_vram_dirty_mark_row_impl(uint32_t y){(void)y;}
void gpu_vram_dirty_mark_rect(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_vram_dirty_mark_all(void){}
int psx_netplay_active(void){return 0;}
/* The renderer's facade hooks (gpu_render.c) for the render thread, which
 * these fixtures never start. The HD fixture links the real facade. */
#ifndef PSX_TEST_HD_TEXTURE_PACK
GrBackend gr_backend(void){return GR_BACKEND_VULKAN;}
void gr_refresh_backend(void){}
#endif
/* These renderer-only fixtures have no frontend telemetry/menu consumer. */
void psx_video_telemetry_compose(int r,int s,int sw,int sh,int dw,int dh,int vw,int vh) {
    (void)r;(void)s;(void)sw;(void)sh;(void)dw;(void)dh;(void)vw;(void)vh;
}
void psx_video_telemetry_present(int result) {(void)result;}
int psx_host_menu_overlay_image(const uint32_t **p,int *w,int *h) {
    *p=NULL;*w=*h=0;return 0;
}

/* This standalone renderer test has no frontend OSD/rewind providers. */
int host_osd_image(const uint32_t**p,int*w,int*h){*p=NULL;*w=*h=0;return 0;}
int host_osd_volume_image(const uint32_t**p,int*w,int*h){*p=NULL;*w=*h=0;return 0;}
int psx_rewind_overlay_image(const uint32_t**p,int*w,int*h){*p=NULL;*w=*h=0;return 0;}
float psx_rewind_slide(void){return 0;}
void host_osd_present_done(void){}
static int test_depth24;
int gpu_display_is_depth24(void){return test_depth24;}
void gpu_get_display_info(GpuDisplayInfo *out){memset(out,0,sizeof(*out));out->width=320;out->height=240;out->depth24=test_depth24;}
static int test_backdrop_phase, test_flat_backdrop;
int psx_ws_prim_in_backdrop(void){return test_backdrop_phase;}
int gpu_ws_nw_flat_backdrop_enabled(void){return test_flat_backdrop;}
static int test_background_full_composite;
int gpu_ws_background_requires_full_composite(void){return test_background_full_composite;}

static void gpu_rgb555_to_rgb888(uint16_t c,uint8_t*r,uint8_t*g,uint8_t*b){*r=(uint8_t)(((c&31)<<3)|((c&31)>>2));*g=(uint8_t)((((c>>5)&31)<<3)|(((c>>5)&31)>>2));*b=(uint8_t)((((c>>10)&31)<<3)|(((c>>10)&31)>>2));}
#include "vk_depth24_scanout.inc"

static uint16_t movie[480*240], guest[480*240], atlas[16*16];
static uint32_t scanout[320*240];
static void mode(int depth24) {
    test_depth24=depth24;
    const GpuRenderBackend *b=vk_backend_get();
    if(b->display_mode_changed)b->display_mode_changed();
}
static void movie_read(const char *phase) {
    vkb_vram_transfer_out(0,0,480,240,guest);
    unsigned bad=0;for(unsigned i=0;i<480*240;++i)bad+=guest[i]!=movie[i];
    printf("%s guest words changed=%u\n",phase,bad);
    check(!bad,"guest read preserves exact RGB888 words");
    GpuDisplayInfo info;gpu_get_display_info(&info);
    unsigned bad_rgb=0;
    for(unsigned y=0;y<240;++y)for(unsigned x=0;x<320;++x){
        uint8_t r,g,b;gpu_display_pixel_rgb_in(vram,&info,x,y,&r,&g,&b);
        uint32_t pixel=0xff000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|b;
        scanout[y*320+x]=pixel;bad_rgb+=pixel!=0xff1153c9u;
    }
    printf("%s RGB888 scanout changed=%u\n",phase,bad_rgb);
    check(!bad_rgb,"production RGB888 scanout preserves CPU movie");
    vk_renderer_present_cpu(scanout,320,240,0,1);
}
int main(void) {
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 77;
    SDL_Window *win=SDL_CreateWindow("Vulkan depth24 hidden test",0,0,128,128,SDL_WINDOW_VULKAN|SDL_WINDOW_HIDDEN);
    if(!win){SDL_Quit();return 77;}
    vkb_init(vram);vkb_set_scale(1);
    if(!vk_renderer_init_context(win)){vk_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();return 77;}
    vkb_set_draw_area(0,0,1023,511);vkb_set_draw_offset(0,0);
    vkb_set_mask_bits(0,0);vkb_set_semi_transparency(0,0);
    vkb_draw_flat_rect(0,0,480,240,0x001f);flush_geometry();gpu_sync();
    check(s_gpu_dirty,"real 15-bit GPU draw makes readback pending");
    mode(1);
    for(unsigned i=0;i<sizeof movie;++i)((uint8_t*)movie)[i]=(uint8_t[]){17,83,201}[i%3];
    vkb_vram_transfer_in(0,0,480,240,movie);
    movie_read("15-to-24 upload");
    vkb_vram_transfer_in(0,0,480,240,movie);
    vkb_set_draw_area(800,400,831,431);vkb_set_draw_offset(2,3);vkb_set_mask_bits(1,0);
    vkb_draw_flat_rect(806,405,4,4,0x7c00);flush_geometry();gpu_sync();
    check(s_gpu_dirty,"GPU work during depth24 reaches real dirty path");
    movie_read("dirty GPU outside movie");
    check(vkb_vram_read(808,408)==0xfc00,"guest sees ordinary GPU draw during CPU-authoritative movie");
    vkb_set_draw_area(0,0,1023,511);vkb_set_draw_offset(0,0);vkb_set_mask_bits(0,0);
    for(int i=0;i<16*16;++i)atlas[i]=0x1234;
    vkb_vram_transfer_in(512,256,16,16,atlas);
    vkb_set_color_modulation(128,128,128,1);vkb_set_texture_filter(1);
    vkb_draw_textured_rect(900,400,8,8,0,0,0,0,(uint16_t)(8|16|(2<<7)));
    check(vkb_vram_read(902,402)==0x1234,"native textured draw stays exact under GPU filtering");
    vkb_copy_rect(900,400,902,400,8,8);
    check(vkb_vram_read(904,402)==0x1234,"CPU-authoritative overlapping copy preserves source");
    /* Model deserializer's CPU VRAM replacement, then run the real restage hook. */
    memset(vram,0,sizeof vram);
    for(int y=0;y<240;++y)memcpy(vram+y*1024,movie+y*480,480*2);
    vk_renderer_restage_vram_after_savestate();
    movie_read("current-build VRAM restage");
    for(int i=0;i<16*16;++i)atlas[i]=0x1234;
    vkb_vram_transfer_in(512,256,16,16,atlas);
    mode(0);
    vkb_vram_write(900,500,0x5678); /* exercise production lazy transition too */
    vkb_draw_flat_rect(0,0,64,64,0x03e0);flush_geometry();gpu_sync();
    vkb_vram_transfer_out(512,256,16,16,guest);
    unsigned bad_atlas=0;for(int i=0;i<16*16;++i)bad_atlas+=guest[i]!=atlas[i];
    printf("return-to-15 queued atlas changed=%u\n",bad_atlas);
    check(!bad_atlas,"queued texture upload survives movie exit");
    check(vkb_vram_read(8,8)==0x03e0,"15-bit GPU draws own guest readback again");
    vk_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();
    printf("Vulkan depth24 failures=%d\n",failures);return failures?1:0;
}
