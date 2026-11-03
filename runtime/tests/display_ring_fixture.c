/* Authored synthetic fixture. Executes current production functions inserted by
 * test_display_ring_capture.py; PNG encoding and the GL/RPC boundary are stubs. */
#include "gpu.h"
#include "color_lut.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define PSX_BSS
#define GTL_DISP_CAPTURE 0
static uint64_t s_frame_count;
static uint16_t live[1024 * 512];
static GpuDisplayInfo current, expected;
static unsigned peek_calls, sync_calls, errors, checks;
static int fbo_enabled, requested_frame, encoded, failures;
static uint16_t expected_word;
static char response[1024];
static ColorLut *s_screen_lut;
static void screen_lut_ensure(void) { /* Config selection is a boundary stub. */ }
static void check(int condition) { ++checks; if (!condition) ++failures; }
void gpu_get_display_info(GpuDisplayInfo *p) { *p = current; }
const uint16_t *gpu_get_vram(void) { return live; }
static void gpu_timeline_note(int event, int a, uint32_t b) { (void)event;(void)a;(void)b; }
static int gl_renderer_fbo_peek_deferred(int x,int y,int w,int h,uint16_t *p) {
    (void)x;(void)y; check(w == 1024 && h == 512); ++peek_calls;
    if (!fbo_enabled) return 0;
    for (unsigned i=0;i<1024u*512u;++i) p[i]=0x7fff;
    return 1;
}
static void gl_renderer_render_thread_sync(const char *s) { check(!strcmp(s,"display_ring")); ++sync_calls; }
static int json_get_int(const char *j,const char *key,int fallback) { (void)j;(void)fallback;check(!strcmp(key,"frame"));return requested_frame; }
static int json_get_str(const char *j,const char *key,char *p,size_t n) { (void)j;check(!strcmp(key,"path"));snprintf(p,n,"ring-fixture-output.bin");return 1; }
static void send_err(int id,const char *message) { (void)id;snprintf(response,sizeof(response),"%s",message);++errors; }
static void send_fmt(const char *format,...) { va_list a;va_start(a,format);vsnprintf(response,sizeof(response),format,a);va_end(a); }
static uint8_t authored_byte(unsigned y,unsigned byte) { return (uint8_t)(y*17u+byte*13u+7u); }
static int png_write_rgb(FILE *fp,const uint8_t *rgb,uint32_t w,uint32_t h) {
    (void)fp; ++encoded; check(w==expected.width && h==expected.height);
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
        const uint8_t *p=rgb+((size_t)y*w+x)*3;
        if(expected.depth24){
            unsigned row=(expected.display_y+y)%512u, byte=expected.display_x*2u+x*3u;
            for(unsigned c=0;c<3;++c)check(p[c]==(byte+2u<2048u?authored_byte(row,byte+c):0));
        }else{
            for(unsigned c=0;c<3;++c){unsigned v=(expected_word>>(c*5u))&31u;check(p[c]==(uint8_t)((v<<3)|(v>>2)));}
        }
    }
    return 1;
}
/* PRODUCTION_DECODER */
/* PRODUCTION_RING */
int main(void) {
    for(unsigned color=0;color<32768;++color) {
        uint8_t r,g,b;gpu_rgb555_to_rgb888((uint16_t)color,&r,&g,&b);
        unsigned rv=color&31u,gv=(color>>5)&31u,bv=(color>>10)&31u;
        check(r==((rv<<3)|(rv>>2)));check(g==((gv<<3)|(gv>>2)));check(b==((bv<<3)|(bv>>2)));
    }
    requested_frame=1;handle_display_ring_get(1,"");check(errors==1);
    memset(&current,0,sizeof(current));current.width=640;current.height=480;
    current.display_x=1000;current.display_y=100;
    for(unsigned i=0;i<1024u*512u;++i)live[i]=0x3a65;
    s_frame_count=1;disp_ring_capture();expected=current;expected_word=0x3a65;
    memset(live,0,sizeof(live));current.width=8;current.height=2;
    handle_display_ring_get(1,"");check(encoded==1 && errors==1);check(strstr(response,"\"height\":480")!=NULL);
    /* Depth24 must select CPU bytes even when an FBO is available. Origin and
     * the last rows exercise vertical wrap and horizontal out-of-row blanking. */
    current.depth24=1;current.width=640;current.height=480;current.display_x=100;current.display_y=500;
    for(unsigned y=0;y<512;++y)for(unsigned x=0;x<1024;++x)
        live[y*1024u+x]=(uint16_t)(authored_byte(y,x*2u)|(authored_byte(y,x*2u+1u)<<8));
    expected=current;fbo_enabled=1;unsigned before=peek_calls;s_frame_count=2;disp_ring_capture();check(peek_calls==before);
    memset(live,0,sizeof(live));memset(&current,0,sizeof(current));requested_frame=2;
    handle_display_ring_get(1,"");check(encoded==2 && errors==1);check(strstr(response,"\"depth24\":1")!=NULL);
    /* 15-bit entries select available FBO truth and remain immutable. */
    current.width=320;current.height=240;s_frame_count=3;disp_ring_capture();expected=current;expected_word=0x7fff;
    fbo_enabled=0;requested_frame=3;handle_display_ring_get(1,"");check(encoded==3 && errors==1);
    /* Reuse of a slot must refuse an old frame, including after disabled capture. */
    s_frame_count=65;disp_ring_capture();requested_frame=1;handle_display_ring_get(1,"");check(errors==2);
    current.disabled=1;s_frame_count=66;disp_ring_capture();requested_frame=2;handle_display_ring_get(1,"");check(errors==3);
    check(sync_calls==6);free(s_disp_ring_px);
    printf("display ring: %u checks, %d failures; full historical480i/depth24, CPU/FBO ownership, stale-frame refusal\n",checks,failures);
    return failures?1:0;
}
