/* Deterministic clock/RAM fixtures exercise the real capture implementation. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "debug_counter_route.h"
static uint64_t ticks,cycles,s_frame_count,work;
static uint32_t draw,state;
static int error;
static char response[32768];
static uint64_t SDL_GetPerformanceCounter(void) { return ticks; }
static uint64_t SDL_GetPerformanceFrequency(void) { return 1000; }
static uint64_t psx_get_cycle_count(void) { return cycles; }
static uint32_t psx_read_word(uint32_t addr) { return (addr&0x1fffff)==4 ? draw : state; }
static int gr_scale(void) { return 4; }
static void psx_rewind_perf(double out[4]) { for(int i=0;i<4;++i)out[i]=(double)work; }
static void overlay_loader_get_counters(uint32_t*a,uint32_t*b,uint32_t*c,
    uint64_t*d,uint64_t*e,uint64_t*f,void*g,void*h,void*i,void*j,uint32_t*k) {
    (void)g;(void)h;(void)i;(void)j;*a=*b=*c=*k=(uint32_t)work;*d=*e=*f=work;
}
static void overlay_loader_get_load_timing(uint64_t*a,void*b,void*c) { (void)b;(void)c;*a=work; }
static void gl_renderer_batch_diag(uint64_t out[8]) { for(int i=0;i<8;++i) out[i]=work; }
static void gl_renderer_submission_diag(double out[3]) { for(int i=0;i<3;++i) out[i]=(double)work; }
static int psx_audio_out_stats(double*a,double*b,uint64_t*c,uint64_t*d,double*e,int*f,int*g) {
    *a=*b=*e=0;*c=*d=work;*f=0;*g=44100;return 1;
}
static const char* value(const char* json,const char* key) {
    char needle[64];snprintf(needle,sizeof(needle),"\"%s\":",key);
    const char* p=strstr(json,needle);return p?p+strlen(needle):NULL;
}
static char* json_get_str(const char*j,const char*k,char*out,size_t size) {
    const char*p=value(j,k);if(!p || *p!='"')return NULL;
    size_t n=strcspn(++p,"\"");if(n>=size)n=size-1;memcpy(out,p,n);out[n]=0;return out;
}
static int json_get_int(const char*j,const char*k,int fallback) { const char*p=value(j,k);return p?atoi(p):fallback; }
static void send_err(int id,const char*p) { (void)id;(void)p;error=1; }
static void send_ok(int id) { (void)id; }
static void send_line(const char*p) { snprintf(response,sizeof(response),"%s",p); }
static void send_fmt(const char*fmt,...) { va_list a;va_start(a,fmt);vsnprintf(response,sizeof(response),fmt,a);va_end(a); }
#include "../src/debug_perf_capture.c.inc"
#define CHECK(c) do { if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);return 1;} } while(0)
static void tick(uint32_t next) { ++s_frame_count;ticks+=17;cycles+=100;draw=next;++work;perf_capture_tick(); }
static void command(const char*p) { error=0;handle_perf_capture(1,p); }
int main(void) {
    uint32_t address;
    CHECK(!perf_parse_address("invalid",&address) && !perf_parse_address("180000004",&address));
    CHECK(!perf_parse_address("",&address) && perf_parse_address("80000004",&address));
    CHECK(perf_ram_address(0x80000004) && perf_ram_address(0xa01ffffc));
    CHECK(!perf_ram_address(0x80200000) && !perf_ram_address(0x1f800004) && !perf_ram_address(3));
    command("{\"op\":\"start\",\"draw_addr\":\"80000004\",\"draws\":2,\"warmup\":1}");
    CHECK(!error);tick(10);CHECK(s_perf_count==0);tick(10);tick(10);tick(11);tick(12);
    CHECK(!s_perf_active && s_perf_count==4 && !strcmp(s_perf_end,"complete"));
    CHECK(s_perf_capture[3].ticks-s_perf_capture[0].ticks==51 && s_perf_work_end[0]-s_perf_work_start[0]==3);
    CHECK(s_perf_audio_end[0]-s_perf_audio_start[0]==3);
    command("{\"op\":\"read\",\"offset\":3,\"count\":1}");CHECK(!error && strstr(response,"samples"));
    command("{\"op\":\"read\",\"offset\":-1}");CHECK(error);
    command("{\"op\":\"start\",\"draw_addr\":\"80000004\",\"draws\":2,\"warmup\":0}");
    command("{}");CHECK(strstr(response,"\"gl_cpu_ms\":[0.000,0.000]"));
    tick(UINT32_MAX);tick(0);tick(1);CHECK(!s_perf_active && !strcmp(s_perf_end,"complete"));
    command("{\"op\":\"start\",\"draw_addr\":\"80000004\",\"draws\":2,\"warmup\":0}");
    tick(100);tick(1);CHECK(!s_perf_active && !strcmp(s_perf_end,"invalid_counter_or_start"));
    command("{\"op\":\"start\",\"draw_addr\":\"80000004\",\"draws\":2,\"warmup\":0}");
    tick(10);++work;command("{}");CHECK(s_perf_gl_end[0]-s_perf_gl_start[0]==1);
    ++work;command("{\"op\":\"stop\"}");CHECK(!s_perf_active && s_perf_batch_end[0]-s_perf_batch_start[0]==2);
    command("{\"op\":\"start\",\"draw_addr\":\"80000004\",\"draws\":2,\"warmup\":0}");
    for(unsigned i=0;i<PERF_CAPTURE_CAP+5;++i)tick(10);
    CHECK(s_perf_count==PERF_CAPTURE_CAP && !strcmp(s_perf_end,"capacity"));
    state=256;command("{\"op\":\"start\",\"draw_addr\":\"80000004\",\"state_addr\":\"80000008\",\"state\":256,\"warmup\":0}");
    tick(10);CHECK(!s_perf_active && !strcmp(s_perf_end,"invalid_counter_or_start"));
    const DebugInputRouteStep steps[]={{3,0xffef},{2,0xbfff},{4,0xffff}};
    uint32_t index=0,remaining=3,last=10;
    CHECK(debug_counter_route_advance(steps,3,&index,&remaining,&last,10) && remaining==3);
    CHECK(debug_counter_route_advance(steps,3,&index,&remaining,&last,14) && index==1 && remaining==1);
    CHECK(debug_counter_route_advance(steps,3,&index,&remaining,&last,15) && index==2 && remaining==4);
    CHECK(!debug_counter_route_advance(steps,3,&index,&remaining,&last,19));
    index=0;remaining=3;last=UINT32_MAX;
    CHECK(debug_counter_route_advance(steps,3,&index,&remaining,&last,0) && remaining==2);
    CHECK(!debug_counter_route_advance(steps,3,&index,&remaining,&last,UINT32_MAX));
    free(s_perf_capture);puts("PASS: frame capture boundaries, limits, reset, wrap, stop, audio and counter routes");return 0;
}
