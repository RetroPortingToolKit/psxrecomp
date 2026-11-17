/* Real hidden GL: execute the shipping ring against the real snapshot service. */
#define PSX_GPU_DISPLAY_HISTORY 1
#define main readback_fixture_main
#include "test_gl_readback_region.c"
#undef main
#include "psx_bss.h"
const uint16_t *gpu_get_vram(void) { return image; }
#include "display_history.inc"

/* Same inactive frontend/HD observers as the render-thread fixture. */
void gpu_hd_textures_begin_upload(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_track_upload(int x,int y,int w,int h,const uint16_t *p){(void)x;(void)y;(void)w;(void)h;(void)p;}
void gpu_hd_textures_invalidate(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_begin_copy(int sx,int sy,int x,int y,int w,int h){(void)sx;(void)sy;(void)x;(void)y;(void)w;(void)h;}
void gpu_hd_textures_end_copy(void){}
uint64_t latency_ring_current_frame(void){return 0;}
void latency_ring_replay_frame(uint64_t frame){(void)frame;}

static uint16_t cpu_before[1024*512],expected[1024*512];
static void ready(void *user,uint64_t version) { *(uint64_t *)user=version; }
static SDL_Window *open_history_context(int scale) {
    SDL_Window *win=SDL_CreateWindow("VRAM history hidden test",0,0,128,128,
        SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    if(!win)return NULL;
    glb_init(image);glb_set_scale(scale);gl_renderer_set_swap_interval(0);
    if(!gl_renderer_init_context(win)){SDL_DestroyWindow(win);return NULL;}
    glb_set_draw_area(0,0,1023,511);glb_set_mask_bits(0,0);
    glb_set_semi_transparency(0,0);glb_set_color_modulation(128,128,128,1);
    return win;
}
int main(int argc,char **argv) {
    int scale=argc>1?atoi(argv[1]):1;
    if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
    for(unsigned i=0;i<1024*512;++i)image[i]=(uint16_t)((i*17u)^0x8000u);
    SDL_Window *win=open_history_context(scale);if(!win)return 2;
    printf("driver=%s renderer=%s scale=%d\n",glGetString(GL_VERSION),glGetString(GL_RENDERER),scale);
    uint64_t material[16]={0};
    for(unsigned i=0;i<16;++i) {
        glb_vram_write(100+(int)i,100,(uint16_t)(i+1));
        material[i]=psx_presentation_gpu_snapshot();check(material[i]!=0,"material capacity retained");
    }
    for(unsigned i=0;i<64;++i) {
        glb_vram_write((int)i,20,(uint16_t)(0x4000u+i));
        s_frame_count=i;debug_server_record_display_frame();
        check(s_disp_ring[i].valid && s_disp_ring[i].gpu_version,"all64 exact frame images retained");
        check(s_disp_ring[i].frame==i,"history preserves exact frame identity");
    }
    const uint64_t evicted=s_disp_ring[0].gpu_version;
    glb_vram_write(0,200,0x7777);
    s_capture_frozen=1;s_frame_count=64;debug_server_record_display_frame();s_capture_frozen=0;
    check(s_disp_ring[0].frame==0 && s_disp_ring[0].gpu_version==evicted,"freeze retains old frame");
    check(psx_presentation_gpu_snapshot()==0,"history cannot expand material capacity16");
    uint64_t full=1;
    check(psx_presentation_gpu_history_replace_async(0,ready,&full) && full==0,"history capacity exactly64");
    debug_server_record_display_frame();
    check(s_disp_ring[0].valid && s_disp_ring[0].frame==64,"replace frees oldest before allocating");
    check(!psx_presentation_gpu_snapshot_read_vram(evicted,oracle),"evicted version cannot revive");
    memcpy(cpu_before,image,sizeof image);memcpy(expected,image,sizeof image);
    /* Leave real flat-batch draws pending: capture must flush them before
     * selecting an epoch, without synchronizing canonical CPU VRAM. */
    glb_draw_flat_rect(200,200,17,3,0x4321);
    for(unsigned y=200;y<203;++y)for(unsigned x=200;x<217;++x)expected[y*1024+x]=0x4321;
    s_frame_count=65;debug_server_record_display_frame();
    check(!memcmp(image,cpu_before,sizeof image),"capture cannot change native CPU VRAM");
    check(disp_ring_materialize(&s_disp_ring[1]),"late explicit complete-frame export");
    check(!memcmp(expected,s_disp_ring[1].vram,sizeof expected),"all native texels/mask bits include pending batch");
    for(unsigned i=2;i<11;++i) {
        check(disp_ring_materialize(&s_disp_ring[i]),"older frame materializes on demand");
        int same=1;
        for(unsigned j=0;j<64;++j) {
            const uint16_t want=j<=i?(uint16_t)(0x4000u+j)
                :(uint16_t)(((20u*1024u+j)*17u)^0x8000u);
            if(s_disp_ring[i].vram[20*1024+j]!=want)same=0;
        }
        check(same,"old frame is independent of subsequent native writes");
    }
    check(!memcmp(cpu_before,image,sizeof image),"history export leaves guest VRAM unchanged");
    for(unsigned i=0;i<16;++i)psx_presentation_gpu_release(material[i]);
    check(glGetError()==GL_NO_ERROR,"direct history GL errors");
    gl_renderer_shutdown();SDL_DestroyWindow(win);
    for(unsigned i=0;i<64;++i)check(!s_disp_ring[i].gpu_version && s_disp_ring[i].valid,
        "complete history preserved before context destruction");
    for(unsigned i=2;i<64;++i)check(s_disp_ring[i].vram[20*1024+i]==(uint16_t)(0x4000u+i),
        "late history survives context restart");
    win=open_history_context(scale);if(!win)return 2;
    if(gl_renderer_render_thread_start(2)==1) {
        for(unsigned i=0;i<3;++i) {
            RTH_REC(RTH_VRAM_WRITE,0,30+(int)i,30,(int)(0x5000u+i));
            s_frame_count=66+i;debug_server_record_display_frame();
            gl_renderer_render_thread_frame_boundary();
        }
        gl_renderer_render_thread_sync("history fixture drain");
        for(unsigned i=0;i<3;++i) {
            check(s_disp_ring[2+i].valid,"worker capture ready after drain");
            check(disp_ring_materialize(&s_disp_ring[2+i]),"worker retained image export");
            check(s_disp_ring[2+i].vram[30*1024+30+i]==(uint16_t)(0x5000u+i),
                "capture follows its ordered native upload");
        }
    } else printf("render thread unavailable: explicit worker case skipped\n");
    check(glGetError()==GL_NO_ERROR,"history GL state and errors");
    gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();
    free(s_disp_ring_px);
    printf("checks=%d failures=%d\n",checks,failures);
    return failures?1:0;
}
