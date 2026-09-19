/* Original source-owned GL readback-coherence regression. No retail payload. */
#include "gpu_gl_renderer.c"
#include "mod_texture_banks.c"
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a){(void)n;(void)a;return 0;}
uint32_t psx_mod_read_word(uint32_t a){(void)a;return 0;}
static uint16_t image[1024*512], oracle[1024*512];
int g_psx_vram_dirty_tracking=0;
uint64_t s_frame_count=0;
void gpu_vram_dirty_mark_row_impl(uint32_t y){}
void gpu_vram_dirty_mark_rect(int x,int y,int w,int h){}
void gpu_vram_dirty_mark_all(void){}
int psx_netplay_active(void){return 0;}
static int test_depth24;
int gpu_display_is_depth24(void){return test_depth24;}
void gpu_get_display_info(GpuDisplayInfo *out){memset(out,0,sizeof(*out));out->display_x=32;out->display_y=32;out->width=320;out->height=16;}
int psx_ws_prim_in_backdrop(void){return 0;}
int gpu_ws_nw_flat_backdrop_enabled(void){return 0;}
int g_ws_tex_edge_pct=0;
int psx_ws_prim_is_tagged(void){return 0;}
void gpu_depth24_upload_span_reset(void){}
void frame_interpolation_schedule_reset(FrameInterpolationSchedule *p){memset(p,0,sizeof(*p));}
static int checks,failures;
static void check(int ok,const char *label){checks++;if(!ok){fprintf(stderr,"FAIL %s\n",label);failures++;}}
static void verify(const char *label){
 gl_renderer_sync_cpu();
 check(gl_renderer_fbo_peek(0,0,1024,512,oracle),"oracle read");
 int n=0;for(int i=0;i<1024*512;i++)n+=image[i]!=oracle[i];
 if(n)fprintf(stderr,"%s: %d native words differ\n",label,n);
 check(n==0,label);check(glGetError()==GL_NO_ERROR,"GL error");
}
static void verify_bank_batching(void) {
 static uint16_t bank[256*128], baseline[96*96], result[96*96];
 for(int i=256;i<256*128;++i)bank[i]=0x3210;
 bank[0]=0;bank[1]=0x001f;bank[2]=0x83e0;bank[3]=0xfc00;
 check(psx_mod_define_texture_bank(8,256,128,bank),"batch fixture bank");
 for(int filter=0;filter<2;++filter) for(int mask=0;mask<2;++mask) {
  int counts[2];
  for(int enabled=0;enabled<2;++enabled) {
   gl_renderer_select_texture_bank(0);
   glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
   glb_draw_flat_rect(400,300,96,96,0x1234);flush_flat_batch();
   psx_mod_set_texture_bank_batching(enabled);
   s_tex_filter=filter;glb_set_mask_bits(0,mask);
   gl_renderer_select_texture_bank(8);
   const int before=s_cw_batches;
   for(int i=0;i<36;++i) {
    const int x=404+(i%6)*5,y=304+(i%4)*7;
    glb_set_semi_transparency(1,(i/6)%4);
    glb_draw_shaded_textured_triangle(x,y,0,2,0x808080,
        x+44,y+2,63,2,0x507090,x+3,y+48,0,65,0x907050,0,0,0,0);
   }
   flush_tex_batch();counts[enabled]=s_cw_batches-before;
   gl_renderer_select_texture_bank(0);
   gl_renderer_sync_cpu();
   check(gl_renderer_fbo_peek(400,300,96,96,enabled?result:baseline),"batch pixel read");
  }
  check(memcmp(baseline,result,sizeof baseline)==0,"ordered semi batching pixel equivalence");
  check(mask?counts[1]==counts[0]:counts[1]<counts[0],"batch reduction only on supported path");
 }
 psx_mod_set_texture_bank_batching(0);s_tex_filter=0;
 glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
}
int main(int argc,char **argv){
 int scale=argc>1?atoi(argv[1]):1;
 if(SDL_Init(SDL_INIT_VIDEO)!=0)return 2;
 SDL_Window *win=SDL_CreateWindow("Readback coherence hidden test",0,0,128,128,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!win)return 2;
 for(int i=0;i<1024*512;i++)image[i]=(uint16_t)((i*17)&0x7fff);
 glb_init(image);glb_set_scale(scale);gl_renderer_set_swap_interval(0);
 if(!gl_renderer_init_context(win))return 2;
 printf("driver=%s renderer=%s scale=%d\n",glGetString(GL_VERSION),glGetString(GL_RENDERER),scale);
 glb_set_draw_area(0,0,1023,511);glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);glb_set_color_modulation(128,128,128,1);
 verify("initial upload");
 glb_draw_flat_rect(1020,511,1,1,0x7fff);
 check(glb_vram_read(1020,511)==0x7fff,"test pixel value");
 GlCohEvent event;int found=0;
 for(uint64_t i=gl_renderer_coh_total();i>0&&i+32>gl_renderer_coh_total();){i--;if(gl_renderer_coh_get(i,&event)&&event.kind==GL_COH_ENSURE){found=1;break;}}
 check(found,"readback event");check(found&&(event.x1-event.x0+1)*(event.y1-event.y0+1)<=4,"single-pixel bounded transfer");
 verify("single pixel + unchanged background");
 for(int row=0;row<8;row++){
  glb_draw_flat_rect(13,17+row*9,7,3,0x1234+row);
  glb_vram_write(23,20+row*9,0x7654);verify("odd coordinates width and row stride");
 }
 glb_draw_flat_rect(40,40,16,16,0x4321);glb_vram_write(900,400,0x7117);glb_draw_flat_rect(600,410,13,7,0x2222);verify("disjoint upload inside readback union");
 glb_draw_flat_rect(512,0,16,16,0x1234);
 glb_draw_textured_rect(90,90,16,16,0,0,0,0,0x108);verify("texture pack does not clear CPU debt");
 glb_fill_rect(1016,508,24,8,0x3210);verify("wrapping fill");
 glb_copy_rect(40,40,42,41,12,12);verify("overlapping copy");
 for(int mode=0;mode<4;mode++){
  glb_set_mask_bits(1,0);glb_draw_flat_rect(111,151,7,3,0x4567);
  glb_set_mask_bits(0,1);glb_draw_flat_rect(109,150,12,6,0x2222);
  glb_set_mask_bits(0,0);glb_set_semi_transparency(1,mode);glb_draw_flat_rect(108,149,14,8,0x1123);glb_set_semi_transparency(0,0);verify("mask and blend");
 }
 glb_set_precise_triangle(1,31*65536+49152,201*65536+49152,63*65536+49152,201*65536+49152,31*65536+49152,219*65536+49152);
 glb_draw_flat_triangle(31,201,63,201,31,219,0x7abc);verify("precision bound margin");
 glb_set_draw_area(11,11,19,19);glb_draw_flat_rect(0,0,32,32,0x5aaa);verify("clipped primitive");
 glb_set_draw_area(0,0,1023,511);glb_draw_flat_rect(320,320,8,8,0x4444);
 for(int i=0;i<1024*512;i++)image[i]=(uint16_t)((i*23)&0x7fff);
 gl_renderer_restage_vram_after_savestate();verify("state restage with pending draw");
 /* Existing depth24 policy clears the skipped movie band on return to15-bit.
  * That GPU write must become visible without waiting for another primitive. */
 static uint16_t movie[480*16], texture[4]={0x3210,0x3210,0x3210,0x3210};
 for(int i=0;i<480*16;i++)movie[i]=0x1234;
 test_depth24=1;depth24_upload_policy();
 glb_vram_transfer_in(32,32,480,16,movie);
 glb_vram_transfer_in(33,33,2,2,texture);
 test_depth24=0;depth24_upload_policy();
 check(glb_vram_read(40,40)==0,"depth24 cleared band immediate CPU read");
 check(glb_vram_read(33,33)==0x3210,"newer overlapping texture survives clear");
 verify("depth24 leave coherence without subsequent primitive");
 /* Retained banks use their own texels AND CLUT, and bank/VRAM transitions
  * must split batches without changing painter order. No retail assets. */
 static uint16_t bank[256*128];
 bank[0]=0x001f; bank[1]=0x03e0; bank[2]=0x7c00;
 bank[16]=0x1111; /* 4-bit indices, CLUT at (0,0) */
 check(psx_mod_define_texture_bank(7,256,128,bank),"define retained bank");
 check(gl_renderer_select_texture_bank(7),"select retained bank");
 glb_draw_shaded_textured_triangle(100,250,64,0,0x808080,132,250,64,0,0x808080,100,282,64,0,0x808080,0,0,0,1);
 check(gl_renderer_select_texture_bank(0),"select original VRAM");
 glb_vram_write(512,0,0x7c00);
 glb_draw_shaded_textured_triangle(116,250,0,0,0x808080,148,250,0,0,0x808080,116,282,0,0,0x808080,0,0,0x108,1);
 check(glb_vram_read(102,252)==0x03e0,"retained 4-bit CLUT independent of guest VRAM");
 check(glb_vram_read(118,252)==0x7c00,"following stock texture wins overlap");
 check(gl_renderer_select_texture_bank(7),"reselect retained bank");
 glb_draw_shaded_textured_triangle(300,250,0,0,0x808080,332,250,0,0,0x808080,300,282,0,0,0x808080,0,0,0x100,1);
 check(gl_renderer_select_texture_bank(0),"reset bank after direct texture");
 check(glb_vram_read(302,252)==0x001f,"retained 16-bit texel");
 verify("retained banks and original VRAM ordered together");
 verify_bank_batching();
 printf("checks=%d failures=%d\n",checks,failures);
 gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
