/* Original source-owned GL readback-coherence regression. No retail payload. */
#include "gpu_gl_renderer.c"
#include "gpu_hd_texture_stubs.inc"
#include "mod_texture_banks.c"
#include "gl_batch_policy_lle.c"
#include "gl_batch_policy_hle.c"
/* Both textured-batch tiers in one binary: the fixture selects per scene. */
static int test_batch_hle;
GlBatchClass gl_batch_classify(int semi,int mask_check,int bank_batch){
 return test_batch_hle?gl_batch_policy_hle(semi,mask_check,bank_batch):gl_batch_policy_lle(semi,mask_check,bank_batch);}
const char *gl_batch_policy_name(void){return test_batch_hle?"HLE":"LLE";}
#include "gpu_timeline.c"
uint64_t psx_cycle_count=0;
static int test_full_composite;
int gpu_ws_background_requires_full_composite(void){return test_full_composite;}
uint32_t psx_mod_gpu_dma_memory_alloc(uint32_t n,uint32_t a){(void)n;(void)a;return 0;}
uint32_t psx_mod_read_word(uint32_t a){(void)a;return 0;}
static uint16_t image[1024*512], oracle[1024*512];
int g_psx_vram_dirty_tracking=0;
uint32_t g_psx_vblank_cycles=564480u;
uint64_t s_frame_count=0;
void gpu_vram_dirty_mark_row_impl(uint32_t y){}
void gpu_vram_dirty_mark_rect(int x,int y,int w,int h){}
void gpu_vram_dirty_mark_all(void){}
int psx_netplay_active(void){return 0;}
/* The renderer's facade hooks (gpu_render.c) for the render thread, which
 * these fixtures never start. */
GrBackend gr_backend(void){return GR_BACKEND_OPENGL;}
void gr_refresh_backend(void){}
static int test_depth24;
int gpu_display_is_depth24(void){return test_depth24;}
void gpu_get_display_info(GpuDisplayInfo *out){memset(out,0,sizeof(*out));out->display_x=32;out->display_y=32;out->width=320;out->height=16;}
int psx_ws_prim_in_backdrop(void){return 0;}
int gpu_ws_nw_flat_backdrop_enabled(void){return 0;}
int g_ws_tex_edge_pct=0;
int psx_ws_prim_is_tagged(void){return 0;}
void gpu_depth24_upload_span_reset(void){}
/* Netplay unsplit view and forward-pass opt-in: off in these fixtures. */
int gpu_ws_netplay_local_viewport_width(void){return 0;}
int render_pass_netplay_enabled(void){return 0;}
void frame_interpolation_schedule_reset(FrameInterpolationSchedule *p){memset(p,0,sizeof(*p));}
void frame_flip_tracker_reset(FrameFlipTracker *p){memset(p,0,sizeof(*p));p->period=1;}
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
/* HLE textured batching must reproduce the LLE pixels. One scene, drawn by
 * each tier from the same VRAM: overlapping painter-ordered runs of opaque
 * prims and every blend mode, 16-bit and 4-bit CLUT pages, raw and modulated
 * texels with STP=0/STP=1/transparent words, every mask-set/mask-check and
 * filter combination, and prims that sample a page an earlier queued prim
 * wrote. Each pass first resets every VRAM word the scene reads; every
 * hr-surface pixel of the scene and the pages is compared. */
enum { TB_X=96, TB_Y=288, TB_W=192, TB_H=160 };
static uint16_t tb_page16[128*64], tb_page4[64*64], tb_clut[16];
static void tier_batch_scene(int filter,int mask_set,int mask_check){
 glb_vram_transfer_in(512,0,128,64,tb_page16);
 glb_vram_transfer_in(640,0,64,64,tb_page4);
 glb_vram_transfer_in(768,0,16,1,tb_clut);
 glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
 glb_fill_rect(TB_X,TB_Y,TB_W,TB_H,0x2108);
 glb_fill_rect(512,64,128,64,0x1084);
 /* A mask-set strip so mask-check scenes have protected pixels to keep. */
 glb_set_mask_bits(1,0);glb_draw_flat_rect(TB_X+40,TB_Y+30,90,20,0x3def);
 s_tex_filter=filter;glb_set_mask_bits(mask_set,mask_check);
 for(int i=0;i<60;++i){
  const int semi=(i%5)-1, x=TB_X+(i*29)%150, y=TB_Y+(i*17)%120;
  glb_set_semi_transparency(semi>=0,semi>=0?semi:0);
  const int four=(i/6)&1, raw=(i/12)&1;
  const uint16_t tp=four?(uint16_t)0x00A:(uint16_t)0x108;
  const uint32_t c=raw?0x808080:(i&1?0x60a0c0:0xc08040);
  glb_draw_shaded_textured_triangle(x,y,(i*7)&31,(i*5)&31,c,
      x+40,y+3,((i*7)&31)+40,(i*5)&31,0x808080,x+5,y+38,(i*7)&31,((i*5)&31)+30,c,
      four?768:0,0,tp,raw);
 }
 /* Feedback: draw into the 16-bit page, then sample what was just drawn. */
 for(int i=0;i<6;++i){
  glb_set_semi_transparency(i&1,i%4);
  glb_draw_shaded_textured_triangle(514+i*8,66,0,0,0x808080,
      540+i*8,66,26,0,0x808080,514+i*8,100,0,34,0x808080,0,0,0x108,1);
  glb_draw_shaded_textured_triangle(TB_X+10+i*25,TB_Y+120,2+i*8,66,0x808080,
      TB_X+40+i*25,TB_Y+122,30+i*8,66,0x808080,TB_X+12+i*25,TB_Y+156,2+i*8,98,0x808080,0,0,0x108,1);
 }
 flush_flat_batch();flush_tex_batch();
 glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);s_tex_filter=0;
}
static void hr_read(int x,int y,int w,int h,uint32_t *out){
 flush_flat_batch();flush_tex_batch();hiw_flush_queue();
 const int S=s_hr_scale;
 p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,s_hr_fbo);
 glPixelStorei(GL_PACK_ALIGNMENT,4);
 glReadPixels(x*S,y*S,w*S,h*S,GL_RGBA,GL_UNSIGNED_BYTE,out);
 p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,0);
}
static void verify_tier_batching(void){
 for(int i=0;i<128*64;++i){
  const int x=i&127,y=i>>7;
  uint16_t v=(uint16_t)(((x*3)&31)|(((y*5)&31)<<5)|(((x^y)&31)<<10));
  if(((x>>2)^(y>>2))&1)v|=0x8000;   /* STP=1 blocks */
  if((x+y)%9==0)v=0;               /* transparent */
  tb_page16[i]=v;
  if(i<64*64)tb_page4[i]=(uint16_t)(i*0x1357u);
 }
 for(int i=0;i<16;++i)tb_clut[i]=(uint16_t)(i==0?0:((i*0x0843u)&0x7fff)|(i&4?0x8000:0));
 const int S=s_hr_scale;
 const size_t scene=(size_t)TB_W*TB_H*S*S, page=(size_t)128*128*S*S;
 uint32_t *ref=malloc((scene+page)*4),*got=malloc((scene+page)*4);
 if(!ref||!got){check(0,"tier batching alloc");free(ref);free(got);return;}
 for(int filter=0;filter<2;++filter)for(int mset=0;mset<2;++mset)for(int mcheck=0;mcheck<2;++mcheck){
  int batches[2];
  for(test_batch_hle=0;test_batch_hle<2;++test_batch_hle){
   const int before=s_cw_batches;
   tier_batch_scene(filter,mset,mcheck);
   batches[test_batch_hle]=s_cw_batches-before;
   uint32_t *dst=test_batch_hle?got:ref;
   hr_read(TB_X,TB_Y,TB_W,TB_H,dst);hr_read(512,0,128,128,dst+scene);
  }
  test_batch_hle=0;
  size_t n=0;
  for(size_t i=0;i<scene+page;++i){
   if(ref[i]==got[i])continue;
   if(n++<8){
    const int pg=i>=scene;const size_t j=pg?i-scene:i;const int w=(pg?128:TB_W)*S;
    fprintf(stderr,"  diff at native (%d,%d) sub (%d,%d): LLE %08x HLE %08x\n",
        (pg?512:TB_X)+(int)(j%w)/S,(pg?0:TB_Y)+(int)(j/w)/S,(int)(j%w)%S,(int)(j/w)%S,ref[i],got[i]);
   }
  }
  char label[96];
  snprintf(label,sizeof label,"HLE batching pixel equivalence filter=%d mask_set=%d mask_check=%d",filter,mset,mcheck);
  if(n)fprintf(stderr,"%s: %zu hr pixels differ\n",label,n);
  check(n==0,label);
  snprintf(label,sizeof label,"HLE batch reduction filter=%d mask_set=%d mask_check=%d (%d vs %d)",filter,mset,mcheck,batches[1],batches[0]);
  check(mcheck?batches[1]<=batches[0]:batches[1]*2<batches[0],label);
  printf("tier batches filter=%d mask_set=%d mask_check=%d: LLE %d HLE %d\n",filter,mset,mcheck,batches[0],batches[1]);
 }
 free(ref);free(got);
 verify("tier batching leaves CPU VRAM coherent");
}
static void verify_stereo_transactions(void) {
 uint16_t before[16*16], after[16*16];
 const int32_t views[2][3]={{-24,0,0},{24,0,0}};
 GLRenderStereoDiag diag;
 GLRenderStereoCapture record;
 glb_set_mask_bits(0,0); glb_set_semi_transparency(0,0);
 check(gl_renderer_fbo_peek(40,40,16,16,before),"stereo canonical snapshot");
 check(gl_renderer_stereo_unavailable()==PSX_MOD_RENDER_PASS_READY,"stereo GL ready");
 check(!gl_renderer_stereo_begin(-1,40,16,16,0),"stereo rejects invalid rectangle");
 s_hiw=1;
 check(gl_renderer_stereo_unavailable()==PSX_MOD_RENDER_PASS_BACKEND,"stereo refuses windowed tiles");
 check(!gl_renderer_stereo_begin(40,40,16,16,0),"stereo window refusal before GPU work");
 s_hiw=0;
 test_depth24=1;
 check(!gl_renderer_stereo_begin(40,40,16,16,0),"stereo refuses depth24");
 test_depth24=0;
 gl_renderer_stereo_stage_reset();
 check(gl_renderer_stereo_begin(40,40,16,16,0),"left eye transaction begins");
 glb_draw_flat_rect(40,40,16,16,0x001f);
 check(gl_renderer_stereo_end(0,1),"left eye captured and restored");
 check(!gl_renderer_stereo_publish(41,91000,views),"partial pair cannot publish");
 gl_renderer_stereo_diag(&diag);
 check(!diag.valid && diag.staged_mask==1,"partial left stays private");
 check(gl_renderer_stereo_begin(40,40,16,16,1),"right eye shares canonical backup");
 glb_draw_flat_rect(40,40,16,16,0x7c00);
 check(gl_renderer_stereo_end(1,1),"right eye captured and restored");
 check(gl_renderer_stereo_publish(41,91000,views),"complete pair publishes atomically");
 check(gl_renderer_fbo_peek(40,40,16,16,after),"stereo canonical readback");
 check(memcmp(before,after,sizeof before)==0,"eye draws restore canonical VRAM pixels");
 size_t bytes=(size_t)16*16*s_hr_scale*s_hr_scale*3;
 uint8_t *left=malloc(bytes),*right=malloc(bytes);
 if(!left || !right){check(0,"stereo readback allocation");free(left);free(right);return;}
 StereoPair *pair=&s_stereo_pair[s_stereo_current];
 glPixelStorei(GL_PACK_ALIGNMENT,1);
 glBindTexture(GL_TEXTURE_2D,pair->tex[0]);glGetTexImage(GL_TEXTURE_2D,0,GL_RGB,GL_UNSIGNED_BYTE,left);
 glBindTexture(GL_TEXTURE_2D,pair->tex[1]);glGetTexImage(GL_TEXTURE_2D,0,GL_RGB,GL_UNSIGNED_BYTE,right);
 glBindTexture(GL_TEXTURE_2D,0);
 check(left[0]>200 && left[2]==0,"left texture contains left draw");
 check(right[2]>200 && right[0]==0,"right texture contains right draw");
 check(memcmp(left,right,bytes)!=0,"both eye textures are independently captured");
 gl_renderer_stereo_stage_reset();
 check(gl_renderer_stereo_begin(40,40,16,16,0),"next left starts");
 glb_draw_flat_rect(40,40,16,16,0x03e0);
 check(gl_renderer_stereo_end(0,1),"next left stages");
 check(gl_renderer_stereo_begin(40,40,16,16,1),"next right starts");
 check(gl_renderer_stereo_end(1,0),"failed right restores without capture");
 check(!gl_renderer_stereo_publish(42,100000,views),"failed pair does not publish");
 gl_renderer_stereo_diag(&diag);
 check(diag.valid && diag.pair_id==41,"failed pair retains complete published pair");
 pair=&s_stereo_pair[s_stereo_current];
 glBindTexture(GL_TEXTURE_2D,pair->tex[0]);glGetTexImage(GL_TEXTURE_2D,0,GL_RGB,GL_UNSIGNED_BYTE,right);
 check(memcmp(left,right,bytes)==0,"failed staging preserves published texture pixels");
 glBindTexture(GL_TEXTURE_2D,0);
 free(left);free(right);
 gl_renderer_stereo_dump_arm(".",1);
 check(gl_renderer_stereo_capture_records(&record,1)==0,"capture request starts empty");
 gl_renderer_stereo_stage_reset();
 for(uint32_t eye=0;eye<2;eye++){
  check(gl_renderer_stereo_begin(40,40,16,16,eye!=0),"capture eye begins");
  glb_draw_flat_rect(40,40,16,16,eye?0x7c00:0x001f);
  check(gl_renderer_stereo_end(eye,1),"capture eye completes");
 }
 check(gl_renderer_stereo_publish(43,110000,views),"capture publishes complete pair");
 check(gl_renderer_stereo_capture_records(&record,1)==1,"capture metadata retained for TCP");
 check(record.pair_id==43 && record.guest_cycle==110000 && record.view_offset[1][0]==24,
       "capture metadata belongs to exact exported pair");
 check(record.width==16u*s_hr_scale && record.height==16u*s_hr_scale,"capture dimensions match textures");
 FILE *file=fopen("p000043_left.png","rb");check(file!=NULL,"left PNG evidence exported");if(file)fclose(file);
 file=fopen("p000043_right.png","rb");check(file!=NULL,"right PNG evidence exported");if(file)fclose(file);
 file=fopen("p000043_sbs.png","rb");check(file!=NULL,"SBS PNG evidence exported");if(file)fclose(file);
 file=fopen("p000043.json","rb");check(file==NULL,"runtime does not write diagnostic JSON");if(file)fclose(file);
 gl_renderer_stereo_reset();
 check(gl_renderer_stereo_capture_records(&record,1)==0,"session reset clears capture metadata");
 check(glGetError()==GL_NO_ERROR,"stereo transaction GL error");
}

static void check_wide_green(uint32_t pixel,const char *label) {
 /* Accurate blending stores each 5-bit channel as k*8, so full green is
  * 248 in the raw wide surface. The legacy basis expands it to 255. */
 const uint32_t expected=gl_renderer_accurate_blending()?0xff00f800u:0xff00ff00u;
 if(pixel!=expected)fprintf(stderr,"%s: got %08x expected %08x\n",label,pixel,expected);
 check(pixel==expected,label);
}
static void verify_oversize_wide_geometry(int scale) {
 /* Captured hallway triangles exceed the PS1 height limit. Proven geometry
  * must use identical canonical/wide passes, including the center-copy edge. */
 glb_set_draw_area(0,0,511,239);glb_set_precise_triangle(0,0,0,0,0,0,0);
 glb_wide_configure(848,168);glb_wide_set_target(0);
 glb_wide_set_view(0,0,0,0);s_wide_fast=1;
 glb_wide_clear(0,0,240,0);
 glb_draw_flat_rect(-168,0,848,240,0x7c00);
 gl_renderer_note_wide_triangle_recovery(1);
 glb_draw_flat_triangle(-300,-200,300,100,-100,500,0x001f);
 check(glb_vram_read(100,100)==0x001f,"recovered flat updates canonical pixels coherently");
 /* A line queued immediately after a recovered flat triangle must use
  * the ordinary batch at both native and supersampled resolutions. */
 gl_renderer_note_wide_triangle_recovery(1);
 glb_draw_flat_triangle(-300,-200,300,100,-100,500,0x001f);
 glb_draw_line(203,40,219,40,0x03e0);
 check(glb_vram_read(210,40)==0x03e0,"ordinary line after oversize flat");
 glb_vram_write(512,0,0x03e0);
 gl_renderer_note_wide_triangle_recovery(1);
 glb_draw_shaded_textured_triangle(561,184,0,0,0x808080,
   1023,-724,0,0,0x808080,1023,334,0,0,0x808080,0,0,0x108,1);
 gl_renderer_note_wide_triangle_recovery(1);
 glb_draw_shaded_textured_triangle(-300,-200,0,0,0x808080,
   300,100,0,0,0x808080,-100,500,0,0,0x808080,0,0,0x108,1);
 check(glb_vram_read(100,100)==0x03e0,"recovered textured updates canonical pixels coherently");
 gl_renderer_note_wide_triangle_recovery(1);
 glb_draw_shaded_textured_triangle(400,-200,0,0,0x808080,
   900,100,0,0,0x808080,400,500,0,0,0x808080,0,0,0x108,1);
 gl_renderer_note_wide_triangle_recovery(1);
 glb_draw_shaded_textured_triangle(561,184,0,0,0x808080,
   1023,-724,0,0,0x808080,1023,334,0,0,0x808080,0,0,0x108,1);
 glb_draw_shaded_textured_triangle(40,100,0,0,0x808080,
   70,100,0,0,0x808080,40,130,0,0,0x808080,0,0,0x108,1);
 check(glb_vram_read(45,105)==0x03e0,"ordinary textured after oversize textured");
 verify("recovered geometry keeps CPU/GPU readback coherent");
 uint32_t *pixels=calloc((size_t)848*240*scale*scale,sizeof(uint32_t));
 check(pixels!=NULL,"oversize wide pixels allocation");
 if(pixels) {
  check(glb_render_wide_display(pixels,848*scale*4,0,0,240)>0,"oversize wide readback");
  check_wide_green(pixels[(100*scale)*(848*scale)+68*scale],"left oversize margin drawn in painter order");
  check_wide_green(pixels[(100*scale)*(848*scale)+818*scale],"captured right hallway wall drawn");
  check_wide_green(pixels[(100*scale)*(848*scale)+268*scale],"canonical center retains recovered faces");
  check_wide_green(pixels[(100*scale)*(848*scale)+678*scale],"recovered wall before center-copy boundary");
  check_wide_green(pixels[(100*scale)*(848*scale)+680*scale],"same recovered wall after center-copy boundary");
  free(pixels);
 }
 glb_wide_disable_target();
 gl_renderer_note_wide_triangle_recovery(1);
 glb_draw_flat_triangle(-300,-200,300,100,-100,500,0x001f);
 check(glb_vram_read(100,100)==0x001f,"admitted backend geometry remains coherent without mirror");
 glb_wide_set_target(0);
}
static void verify_camera_plane_clip(int scale) {
 glb_set_draw_area(0,0,511,239);glb_wide_configure(848,168);
 glb_wide_set_target(0);glb_wide_set_view(0,0,0,0);
 glb_wide_clear(0,0,240,0);glb_draw_flat_rect(-168,0,848,240,0x7c00);
 glb_vram_write(512,0,0x03e0);
 /* One corner is behind the camera, with two at z=2 in front. The
  * visible trapezoid contains (100,100), and extends into the left margin. */
 const PSXProjectedVertex crossing[3]={
   {-1600,0,-1,0,0,1,1,1},{600,0,2,0,0,1,1,1},{600,480,2,0,0,1,1,1}};
 check(gl_renderer_projective_supported(),"projective GPU path ready");
 gl_renderer_draw_projected_triangle(crossing,0x108,0,0,1,-1,1);
 check(glb_vram_read(100,100)==0x03e0,"crossing wall retained in front of camera");
 check(glb_vram_read(400,100)==0x7c00,"outside crossing wall remains untouched");
 const PSXProjectedVertex hidden[3]={
   {-400,0,-2,0,0,1,1,1},{600,0,-1,0,0,1,1,1},{600,480,-2,0,0,1,1,1}};
 gl_renderer_draw_projected_triangle(hidden,0x108,0,0,1,-1,1);
 check(glb_vram_read(400,100)==0x7c00,"entirely behind-camera triangle rejected");
 glb_draw_flat_rect(120,100,4,4,0x001f);
 check(glb_vram_read(121,101)==0x001f,"ordinary draw after clipped wall keeps painter order");
 check(!s_pc_valid && !s_pq_valid && !s_projected_uv_valid,"clipped overrides consumed");
 verify("camera-plane clipping preserves GPU and CPU authority");
 uint32_t *pixels=calloc((size_t)848*240*scale*scale,sizeof(uint32_t));
 check(pixels!=NULL,"camera-plane wide allocation");
 if(pixels) {
  check(glb_render_wide_display(pixels,848*scale*4,0,0,240)>0,"camera-plane wide readback");
  check_wide_green(pixels[100*scale*(848*scale)+68*scale],"crossing wall retained in wide margin");
  check_wide_green(pixels[100*scale*(848*scale)+268*scale],"same crossing wall across center copy");
  free(pixels);
 }
 /* Perspective UVs must survive intersections without integer rounding.
  * Analytic barycentric values at the two samples are (15.789,5.702)
  * and (17.5,6.771), independent of the clipped polygon's triangulation. */
 for(int v=0;v<32;++v) for(int u=0;u<32;++u)
  glb_vram_write(512+u,v,(uint16_t)((u+1)|((v+1)<<5)));
 PSXProjectedVertex mapped[3]={crossing[0],crossing[1],crossing[2]};
 mapped[1].u=mapped[2].u=20;mapped[2].v=20;
 gl_renderer_draw_projected_triangle(mapped,0x108,0,0,1,-1,1);
 check(glb_vram_read(100,100)==(16|(6<<5)),"fractional clipped UV sample one");
 check(glb_vram_read(200,100)==(18|(7<<5)),"fractional clipped UV sample two");
}
/* Native-wide, vertically double-buffered (both bands in ONE wide surface):
 * a full-width flat rect takes the full-screen-overlay wide pass. It spans
 * above its draw area; canonical VRAM clips it at the band top, and the wide
 * surface must clip it there too instead of painting the other band. */
static void verify_wide_overlay_band(void) {
 const int S=s_out_scale,W=426*S,H=512*S;
 uint32_t *wide=(uint32_t*)malloc((size_t)W*H*sizeof(uint32_t));
 int ow=0,oh=0;
 check(wide!=NULL,"wide dump alloc");if(!wide)return;
 glb_set_draw_area(0,0,1023,511);
 glb_wide_configure(426,53);glb_wide_set_target(0);
 glb_wide_clear(0,0,512,0x0421);
 glb_set_draw_area(0,240,319,479);
 glb_draw_flat_rect(0,158,320,322,0x001f);flush_flat_batch();
 check(glb_wide_dump_full(wide,W*H,&ow,&oh,0)>0&&ow==W&&oh==H,"wide surface dump");
 const uint32_t other=wide[(200*S)*W+5*S],edge=wide[(239*S+S-1)*W+420*S],drawn=wide[(240*S)*W+5*S];
 check(((other>>16)&0xff)<0x20,"overlay rect leaves the other band's margin");
 check(((edge>>16)&0xff)<0x20,"overlay rect stops at the draw-area top row");
 check(((drawn>>16)&0xff)>=0xf0,"overlay rect covers its own band's margin");
 free(wide);
 glb_set_draw_area(0,0,1023,511);glb_wide_disable_target();glb_wide_configure(0,0);
}
/* A fog strip changes the blend key while earlier world triangles are still
 * queued. They must reach the reveal before the strip, and the strip must
 * blend exactly once in both the centre and the reveal. No retail payload. */
static void verify_wide_overlay_order(void) {
 const int S=s_out_scale,W=426*S,H=512*S;
 uint32_t *wide=(uint32_t*)malloc((size_t)W*H*sizeof(uint32_t));
 int ow=0,oh=0;
 check(wide!=NULL,"overlay order alloc");if(!wide)return;
 gl_renderer_select_texture_bank(0);
 for(int textured=0;textured<2;++textured) {
 glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
 glb_set_draw_area(0,0,1023,511);
 glb_draw_flat_rect(0,0,1024,512,0x0421);flush_flat_batch();
 glb_vram_write(512,0,0x001f);flush_cpu_upload();
 glb_wide_configure(426,53);glb_wide_set_target(0);
 glb_wide_clear(0,0,512,0x0421);
 glb_set_draw_area(0,240,319,479);
 if(textured) {
  glb_draw_shaded_textured_triangle(200,320,0,0,0x808080,400,320,0,0,0x808080,300,440,0,0,0x808080,0,0,0x108,1);
  check(s_tb_n>0,"textured predecessor is queued");
 }else glb_draw_gouraud_triangle(200,320,0x001f,400,320,0x001f,300,440,0x001f);
 glb_set_semi_transparency(1,0);
 glb_draw_flat_rect(0,340,320,1,0x7fff);
 flush_flat_batch();
 check(glb_wide_dump_full(wide,W*H,&ow,&oh,0)>0&&ow==W&&oh==H,"overlay order dump");
 const uint32_t edge=wide[(340*S)*W+403*S],centre=wide[(340*S)*W+333*S];
 check(((edge>>16)&0xff)>=0xf0,textured?"queued textured face survives fog in reveal":"queued world triangle survives fog in reveal");
 check(((edge>>8)&0xff)>=120&&((edge>>8)&0xff)<=128,"fog blends once in reveal");
 check(((centre>>16)&0xff)>=0xf0&&((centre>>8)&0xff)>=120&&((centre>>8)&0xff)<=128,"fog blends once in centre");
 check(glGetError()==GL_NO_ERROR,"overlay order GL error");
 gl_renderer_select_texture_bank(0);glb_set_semi_transparency(0,0);
 glb_set_draw_area(0,0,1023,511);glb_wide_disable_target();glb_wide_configure(0,0);
 }
 free(wide);
}
/* Render-pass capture of an anchored wide frame (frame interpolation). The
 * camera view translates world draws by its shift, so the mirror drew the whole
 * wide surface and the canonical frame is NOT its centre. A capture taken on the
 * fast centre path must equal one taken with the fast path off; copying the
 * unshifted canonical frame over the centre shows stale margins beside a
 * displaced centre, alternating with the game's own frames (flicker). */
static int capture_wide_pass(GLuint tex,int fast){
 PassGen g;memset(&g,0,sizeof g);
 g.source_path=GL_PRES_WIDE;g.x=0;g.y=0;g.w=320;g.h=240;
 flush_flat_batch();flush_tex_batch();
 s_wide_fast=fast;pass_capture_into(tex,&g);s_wide_fast=1;
 return glGetError()==GL_NO_ERROR;
}
static void verify_anchored_pass_capture(int scale,int shift){
 const int W=426*scale,H=240*scale;const size_t n=(size_t)W*H*4;
 uint8_t *fast=malloc(n),*full=malloc(n),*again=malloc(n);
 if(!fast||!full||!again){check(0,"anchored pass capture allocation");free(fast);free(full);free(again);return;}
 GLuint tex[2];glGenTextures(2,tex);
 for(int i=0;i<2;i++){
  glBindTexture(GL_TEXTURE_2D,tex[i]);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
 }
 /* Full mirror first: a capture that copied the canonical frame into the live
  * wide surface would also corrupt every later read of it. */
 check(capture_wide_pass(tex[1],0),"anchored pass capture, full mirror");
 check(capture_wide_pass(tex[0],1),"anchored pass capture, fast centre path");
 glPixelStorei(GL_PACK_ALIGNMENT,1);
 glBindTexture(GL_TEXTURE_2D,tex[0]);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,fast);
 glBindTexture(GL_TEXTURE_2D,tex[1]);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,full);
 size_t diff=0;for(size_t i=0;i<n;i++)diff+=fast[i]!=full[i];
 if(diff)fprintf(stderr,"anchored pass capture shift %d: %zu bytes differ\n",shift,diff);
 check(diff==0,"anchored pass capture keeps the mirrored centre");
 check(capture_wide_pass(tex[1],0),"anchored pass capture, full mirror again");
 glBindTexture(GL_TEXTURE_2D,tex[1]);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,again);
 glBindTexture(GL_TEXTURE_2D,0);glPixelStorei(GL_PACK_ALIGNMENT,4);
 diff=0;for(size_t i=0;i<n;i++)diff+=again[i]!=full[i];
 check(diff==0,"anchored pass capture leaves the live wide surface unchanged");
 /* The anchored world marker is in the capture where the view put it. */
 int at_view=0,at_canonical=0;
 for(int y=0;y<H;y++){
  const uint8_t *v=fast+((size_t)y*W+(size_t)(213+shift)*scale)*4;
  const uint8_t *c=fast+((size_t)y*W+(size_t)213*scale)*4;
  at_view+=v[1]>200&&v[0]<64&&v[2]<64;at_canonical+=c[1]>200&&c[0]<64&&c[2]<64;
 }
 check(at_view>0,"anchored marker captured at the view position");
 if(shift)check(at_canonical==0,"no canonical-position marker in the anchored capture");
 glDeleteTextures(2,tex);free(fast);free(full);free(again);
}
static void verify_presentation_capture(void) {
 const int w=7*s_out_scale,h=3*s_out_scale,stride=w+3;
 uint32_t *out=malloc((size_t)stride*h*4);
 s_cpu_auth_dual=1;
 glb_set_draw_area(0,0,1023,511);glb_set_mask_bits(0,0);glb_set_semi_transparency(0,0);
 glb_draw_flat_rect(53,61,7,1,0x001f);
 glb_draw_flat_rect(53,62,7,2,0x03e0);
 flush_flat_batch();flush_tex_batch();flush_cpu_upload();
 // Deliberately distinct CPU authority. No upload debt: capture must read FBO.
 image[61*1024+53]=0x7c00;
 memcpy(oracle,image,sizeof(image));
 for(int i=0;i<stride*h;i++)out[i]=0x12345678;
 GLuint pack=0;p_glGenBuffers(1,&pack);p_glBindBuffer(GL_PIXEL_PACK_BUFFER,pack);
 p_glBufferData(GL_PIXEL_PACK_BUFFER,1024,NULL,0x88E1);
 glPixelStorei(PSXGL_PACK_ROW_LENGTH,19);glPixelStorei(GL_PACK_ALIGNMENT,8);
 GLint before_fbo;glGetIntegerv(0x8CAA,&before_fbo);
 check(gl_renderer_capture_display_hires(out,stride*4,53,61,7,3)==w*h,"presentation capture scaled count");
 check((out[0]&0x00ffffff)>=0x00f00000 && !(out[0]&0xffff),"capture reads GL red, not CPU blue");
 check((out[(h-1)*stride]&0xff00)>=0xf000 && !(out[(h-1)*stride]&0xff00ff),"capture retains native row orientation");
 for(int y=0;y<h;y++)check(out[y*stride+w]==0x12345678,"capture respects pitch padding");
 check(!memcmp(image,oracle,sizeof(image)),"capture leaves canonical CPU VRAM unchanged");
 GLint value;glGetIntegerv(0x88ED,&value);check(value==(GLint)pack,"capture restores PBO");
 glGetIntegerv(0x8CAA,&value);check(value==before_fbo,"capture restores read FBO");
 glGetIntegerv(PSXGL_PACK_ROW_LENGTH,&value);check(value==19,"capture restores row length");
 glGetIntegerv(GL_PACK_ALIGNMENT,&value);check(value==8,"capture restores alignment");
 check(!gl_renderer_capture_display_hires(out,4,53,61,7,3),"capture rejects short pitch");
 check(glGetError()==GL_NO_ERROR,"capture GL error");
 p_glBindBuffer(GL_PIXEL_PACK_BUFFER,0);((void (APIENTRY *)(GLsizei,const GLuint *))SDL_GL_GetProcAddress("glDeleteBuffers"))(1,&pack);
 glPixelStorei(PSXGL_PACK_ROW_LENGTH,0);glPixelStorei(GL_PACK_ALIGNMENT,4);
 s_cpu_auth_dual=0;free(out);
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
 /* Adaptive backdrop reflection must retain every edge texel at both native
  * and supersampled scales; the ordinary rect stays forward-facing. */
 for(int x=0;x<16;++x) glb_vram_write(512+x,0,(uint16_t)(0x400+x+1));
 glb_draw_textured_rect(700,200,16,1,0,0,0,0,0x108);
 glb_draw_textured_rect_scaled(720,200,16,1,15,0,-1,1,0,0,0x108);
 for(int x=0;x<16;++x) {
  check(glb_vram_read(700+x,200)==0x401+x,"forward panorama texel");
  check(glb_vram_read(720+x,200)==0x410-x,"reflected panorama texel");
 }
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
 /* Consecutive RGB888 movies can stay in depth24. The second player's tile
  * clear must reach the CPU scanout, without reading stale FBO words over the
  * new packed movie. Also cover entry directly through an upload, not a test
  * call to the mode policy before it. */
 static uint16_t movie_first[480*16], movie_next[480*8];
 for(int i=0;i<480*16;i++)movie_first[i]=0x2345;
 for(int i=0;i<480*8;i++)movie_next[i]=0x4567;
 glb_draw_flat_rect(900,400,2,2,0x4321);
 glb_draw_flat_rect(32,32,480,16,0x7117);
 test_depth24=1;
 glb_vram_transfer_in(32,32,480,16,movie_first);
 check(image[400*1024+900]==0x4321,"depth24 entry retains pending GPU pixels");
 check(image[32*1024+32]==0x2345,"entry sync precedes first packed upload");
 glb_draw_flat_rect(32,32,480,16,0);
 glb_vram_transfer_in(32,36,480,8,movie_next);
 check(image[33*1024+40]==0,"consecutive movie top bar cleared");
 check(image[46*1024+40]==0,"consecutive movie bottom bar cleared");
 gl_renderer_sync_cpu();
 check(image[38*1024+40]==0x4567,"movie survives primitive readback debt");
 glb_copy_rect(32,36,40,37,4,2);
 check(image[37*1024+40]==0x4567,"depth24 copy reads packed CPU source");
 glb_fill_rect(48,33,16,1,0);
 check(glb_vram_read(48,33)==0,"depth24 fill and read stay coherent");
 test_depth24=0;depth24_upload_policy();
 verify("consecutive movie return to GPU authority");
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
 /* Run retained-bank tests with the normal draw area before the wide-view
  * regression narrows it to the gameplay rectangle. */
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
 /* A streamed scene can retain indices while CLUT uploads/fades continue.
  * Alternate both modes of the SAME bank with pending draws: palette source
  * must participate in the batch key, and stock packets must reset it. */
 glb_vram_write(1,0,0x7c00);
 check(gl_renderer_select_texture_bank_live_clut(7),"select bank with live CLUT");
 glb_draw_shaded_textured_triangle(400,250,64,0,0x808080,432,250,64,0,0x808080,400,282,64,0,0x808080,0,0,0,1);
 check(gl_renderer_select_texture_bank(7),"same bank with retained CLUT");
 glb_draw_shaded_textured_triangle(440,250,64,0,0x808080,472,250,64,0,0x808080,440,282,64,0,0x808080,0,0,0,1);
 check(glb_vram_read(402,252)==0x7c00,"live guest palette used");
 check(glb_vram_read(442,252)==0x03e0,"same-bank palette source is a batch key");
 glb_vram_write(1,0,0x001f);
 check(gl_renderer_select_texture_bank_live_clut(7),"live CLUT after update");
 glb_draw_shaded_textured_triangle(480,250,64,0,0x808080,512,250,64,0,0x808080,480,282,64,0,0x808080,0,0,0,1);
 check(gl_renderer_select_texture_bank(0),"reset live-CLUT bank");
 check(glb_vram_read(482,252)==0x001f,"palette update visible without replacing indices");
 verify("retained indices with animated guest CLUT");
 verify_bank_batching();
 verify_tier_batching();
 verify_stereo_transactions();
 verify_oversize_wide_geometry(scale);
 verify_camera_plane_clip(scale);
 /* World and UI use different origins in an anchored wide frame. Keep the
  * canonical-center optimization enabled to catch an erroneous blit over the
  * completed mirror, and change origins with a pending flat batch. */
 glb_set_draw_area(0,0,319,239);glb_set_precise_triangle(0,0,0,0,0,0,0);
 glb_wide_configure(426,53);glb_wide_set_target(0);
 s_wide_fast=1;
 uint32_t *wide_pixels=calloc((size_t)426*240*scale*scale,sizeof(uint32_t));
 if(!wide_pixels)return 2;
 for(int shift=-53;shift<=53;shift+=53){
  glb_wide_clear(0,0,240,0);
  glb_wide_set_view(1,shift,0,0);
  glb_draw_flat_rect(-106,0,532,240,0x7c00);
  glb_draw_flat_rect(160,40,3,3,0x03e0);
  glb_wide_set_view(1,0,0,0);
  glb_draw_flat_rect(160,20,3,3,0x001f);
  check(glb_render_wide_display(wide_pixels,426*scale*4,0,0,240)>0,"anchored wide readback");
  check(wide_pixels[(40*scale)*(426*scale)+(213+shift)*scale]==0xff00f800u,"anchored world marker");
  check(wide_pixels[(20*scale)*(426*scale)+213*scale]==0xfff80000u,"centered dialogue marker");
  check(wide_pixels[(60*scale)*(426*scale)]==0xff0000f8u,"anchored left edge");
  check(wide_pixels[(60*scale)*(426*scale)+425*scale]==0xff0000f8u,"anchored right edge");
  verify_anchored_pass_capture(scale,shift);
 }
 glb_wide_set_view(0,0,0,0);
 check(wide_dx()==53,"disabled view preserves original origin");
 free(wide_pixels);
 for(test_full_composite=0;test_full_composite<2;test_full_composite++){verify_wide_overlay_band();verify_wide_overlay_order();}
 test_full_composite=0;
 verify_presentation_capture();
 printf("checks=%d failures=%d\n",checks,failures);
 gl_renderer_shutdown();SDL_DestroyWindow(win);SDL_Quit();return failures?1:0;
}
