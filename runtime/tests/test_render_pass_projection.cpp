#include "psx_thread_local.h"
#include "cpu_state.h"
#include "render_pass_projection.h"
#include <cmath>
#include <cstdio>

extern "C" {
PSX_THREAD_LOCAL int g_psx_render_pass_active=0;
int (*g_psx_projection_command)(CPUState*,uint32_t)=nullptr;
uint16_t psx_mod_read_half(uint32_t) { return 0; }
uint32_t psx_mod_read_word(uint32_t) { return 0; }
void psx_mod_write_half(uint32_t,uint16_t) {}
void psx_mod_write_word(uint32_t,uint32_t) {}
}
static int failures=0;
#define CHECK(v) do { if(!(v)) { std::printf("FAIL line %d: %s\n",__LINE__,#v); ++failures; } } while(0)
static CPUState pose(int x,int vertex=7,double yaw=0) {
    CPUState c{}; c.gpr[31]=0x80012340;
    int16_t r[9]={(int16_t)std::lround(4096*std::cos(yaw)),0,(int16_t)std::lround(4096*std::sin(yaw)),
        0,4096,0,(int16_t)std::lround(-4096*std::sin(yaw)),0,(int16_t)std::lround(4096*std::cos(yaw))};
    for(int i=0;i<9;++i) c.gte_ctrl[i/2]|=uint32_t(uint16_t(r[i]))<<(16*(i%2));
    c.gte_ctrl[5]=x; c.gte_ctrl[7]=1000;
    c.gte_data[0]=vertex; c.gte_data[1]=10;
    return c;
}
static void capture(PSXProjectionHistory* h,CPUState* list,int count,uint32_t ticks=2) {
    g_psx_render_pass_active=0;
    psx_projection_capture_begin(h);
    for(int i=0;i<count;++i) CHECK(!psx_projection_command(list+i,1));
    psx_projection_capture_end(h,ticks,512);
}
static void test_matching() {
    auto* h=psx_projection_create(16);
    CPUState first[]={pose(0),pose(1000),pose(0,9)};
    capture(h,first,3);
    PSXProjectionStats st{}; psx_projection_stats(h,&st);
    CHECK(st.captured==3 && st.matched==0);
    // Same geometry on two objects, reversed order; one disappears and a new
    // vertex enters. A shifted ordinal must not reuse the disappeared object.
    CPUState current[]={pose(1100),pose(100),pose(50,11)};
    capture(h,current,3); psx_projection_stats(h,&st);
    CHECK(st.matched==2 && st.changed==2 && st.unmatched==1);
    g_psx_render_pass_active=1; psx_projection_replay_begin(h,32768);
    CPUState c=current[1]; CHECK(psx_projection_command(&c,1)); CHECK(c.gte_ctrl[5]==50);
    c=current[0]; CHECK(psx_projection_command(&c,1)); CHECK(c.gte_ctrl[5]==1050);
    c=current[2]; CHECK(!psx_projection_command(&c,1)); CHECK(c.gte_ctrl[5]==50);
    c=current[0]; CHECK(!psx_projection_command(&c,1)); // each occurrence once
    psx_projection_replay_end(h); CHECK(!g_psx_projection_command);
    psx_projection_destroy(h);
}
static void test_cuts_and_rotation() {
    auto* h=psx_projection_create(16);
    CPUState a[]={pose(0,1),pose(0,2),pose(0,3)};
    CPUState b[]={pose(2048,1),pose(0,2,2.0),pose(100,3,.6)};
    capture(h,a,3); capture(h,b,3);
    PSXProjectionStats st{}; psx_projection_stats(h,&st);
    CHECK(st.discontinuities==2 && st.matched==1);
    g_psx_render_pass_active=1; psx_projection_replay_begin(h,32768);
    CPUState c=b[0]; CHECK(!psx_projection_command(&c,1)); CHECK(c.gte_ctrl[5]==2048);
    c=b[1]; CHECK(!psx_projection_command(&c,1));
    c=b[2]; CHECK(psx_projection_command(&c,1)); CHECK(c.gte_ctrl[5]==50);
    CHECK(std::abs(std::atan2(int16_t(c.gte_ctrl[1]),int16_t(c.gte_ctrl[0]))-.3)<.002);
    // If an abort unwinds the callback, a later architectural GTE command is
    // never interpolated using the abandoned pass's active history.
    g_psx_render_pass_active=0; c=b[2]; CHECK(!psx_projection_command(&c,1));
    CHECK(!g_psx_projection_command);
    psx_projection_destroy(h);
}
static void test_overflow_and_reset() {
    auto* h=psx_projection_create(1);
    CPUState a[]={pose(0),pose(10)};
    capture(h,a,2);
    PSXProjectionStats st{}; psx_projection_stats(h,&st); CHECK(st.overflow==1);
    capture(h,a,1); psx_projection_stats(h,&st); CHECK(!st.matched);
    capture(h,a,1); psx_projection_stats(h,&st); CHECK(st.matched==1);
    // Savestate loads occur between frames, with no active capture/replay.
    psx_projection_reset_session();
    capture(h,a,1); psx_projection_stats(h,&st); CHECK(!st.matched);
    psx_projection_capture_begin(h); psx_projection_reset_session();
    CHECK(!g_psx_projection_command);
    psx_projection_stats(h,&st); CHECK(!st.captured);
    psx_projection_destroy(h);
}
int main() {
    test_matching(); test_cuts_and_rotation(); test_overflow_and_reset();
    std::printf("projection history failures=%d\n",failures); return failures?1:0;
}
