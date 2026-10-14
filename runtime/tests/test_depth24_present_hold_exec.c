#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#define GPU_EXEC_REAL_UI_GROUP
#include "../src/gpu.c"
#include "gpu_exec_stubs.inc"

static void arm(void) {
    gpu_reset_state(0);
    display_depth = 1;
    gp1_v_display_range((260u<<10)|20u);
    gp1_v_display_range((244u<<10)|20u);
    assert(s_d24_present_hold==3);
}
int main(void) {
    for (int host_present=0; host_present<2; ++host_present) {
        arm();
        for (int vb=0;vb<3;++vb) {
            depth24_note_upload(0,480);
            assert(s_d24_upload_x1==0);
            gpu_vblank_tick();
            assert(s_d24_present_hold==2-vb);
            if (host_present) {
                assert(gpu_depth24_present_hold_tick()==1);
                assert(gpu_depth24_present_hold_tick()==0);
            }
        }
        depth24_note_upload(0,480);
        assert(s_d24_upload_x1==480);
        gpu_vblank_tick();
        assert(gpu_depth24_present_hold_tick()==0);
    }
    arm(); gpu_vblank_tick();
    unsigned char saved[4096];
    uint32_t n=gpu_snapshot_bytes();
    assert(n<=sizeof saved);
    gpu_snapshot_write(saved);
    gpu_vblank_tick(); gpu_vblank_tick();
    assert(s_d24_present_hold==0);
    assert(gpu_snapshot_read(saved,n));
    assert(s_d24_present_hold==2);
    gpu_vblank_tick(); assert(s_d24_present_hold==1);
    gpu_depth24_on_savestate_loaded();
    assert(s_d24_present_hold==0 && gpu_depth24_present_hold_tick()==0);
    puts("depth24 hold: retarget, headless/present cadence, upload span and snapshot restore PASS");
    return 0;
}
