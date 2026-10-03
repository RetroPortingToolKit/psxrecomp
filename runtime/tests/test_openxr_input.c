#include "psx_openxr.h"
#include <stdio.h>
#include <math.h>
/* Evaluate side effects and failures even when NDEBUG is defined. */
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)
int main(void) {
    /* UI requests require a live tracked frame. Off/unavailable requests
     * must leave submission state neutral, including invalid dimensions. */
    CHECK(!psx_openxr_quad(2,2,1.5));
    CHECK(!psx_openxr_quad(NAN,2,1.5));
    CHECK(!psx_openxr_quad(.1,2,1.5));
    CHECK(!psx_openxr_quad(2,0,1.5));
    CHECK(!psx_openxr_quad(0,0,0));
    PSXOpenXRStats stats;psx_openxr_stats(&stats);
    CHECK(!stats.submitted_layer && !stats.quad_submitted);
    CHECK(!stats.submitted_source && !stats.native_submitted && !stats.submitted_native_frame);
    PSXModOpenXRInput p={0},s={0};
    p.struct_size=sizeof p;
    CHECK(psx_openxr_input(&p) && !p.focused && p.stick[0][0]==0 && !p.synthetic);
    s.struct_size=sizeof s;s.focused=1;s.active[0]=s.active[1]=1;
    s.stick[0][0]=-.75f;s.stick[0][1]=1;s.stick[1][0]=.6f;
    s.trigger_active[1]=1;s.trigger[1]=.75f;
    s.squeeze_active[0]=1;s.squeeze[0]=.6f;
    s.buttons_active[0]=PSX_MOD_XR_PRIMARY|PSX_MOD_XR_MENU;
    s.buttons[0]=PSX_MOD_XR_PRIMARY|PSX_MOD_XR_SECONDARY|PSX_MOD_XR_MENU;
    CHECK(psx_openxr_input_override(&s));
    CHECK(psx_openxr_input(&p) && p.synthetic && p.stick[0][0]==-.75f && p.stick[1][0]==.6f);
    CHECK(p.trigger[1]==.75f && p.squeeze[0]==.6f);
    CHECK(p.buttons[0]==(PSX_MOD_XR_PRIMARY|PSX_MOD_XR_MENU));
    uint64_t seq=p.sequence;psx_openxr_input_snapshot(&p);CHECK(p.sequence==seq);
    s.focused=0;CHECK(psx_openxr_input_override(&s));
    CHECK(psx_openxr_input(&p) && p.synthetic && !p.focused && p.stick[0][1]==0 && p.stick[1][0]==0);
    CHECK(!p.trigger[1] && !p.squeeze[0] && !p.buttons[0]);
    s.focused=1;s.active[0]=0;CHECK(psx_openxr_input_override(&s));
    CHECK(psx_openxr_input(&p) && p.stick[0][0]==0 && p.stick[1][0]==.6f);
    CHECK(p.squeeze[0]==.6f && p.buttons[0]); /* independent of stick activity */
    s.trigger_active[1]=0;s.squeeze_active[0]=0;s.buttons_active[0]=0;
    CHECK(psx_openxr_input_override(&s));
    CHECK(psx_openxr_input(&p) && !p.trigger[1] && !p.squeeze[0] && !p.buttons[0]);
    s.trigger[0]=NAN;CHECK(!psx_openxr_input_override(&s));s.trigger[0]=0;
    s.squeeze[0]=1.1f;CHECK(!psx_openxr_input_override(&s));s.squeeze[0]=0;
    s.buttons_active[0]=16;CHECK(!psx_openxr_input_override(&s));s.buttons_active[0]=0;
    s.stick[0][0]=NAN;CHECK(!psx_openxr_input_override(&s));
    s.stick[0][0]=1.1f;CHECK(!psx_openxr_input_override(&s));
    CHECK(psx_openxr_input_override(0));
    CHECK(psx_openxr_input(&p) && !p.synthetic && p.stick[1][0]==0);
    s.stick[0][0]=1;CHECK(psx_openxr_input_override(&s));
    psx_openxr_shutdown();CHECK(psx_openxr_input(&p) && !p.synthetic && !p.focused);
    PSXModOpenXRHands h={0},got={0};got.struct_size=sizeof got;
    CHECK(psx_openxr_hands(&got) && !got.focused && got.age_ms==UINT32_MAX);
    h.struct_size=sizeof h;h.focused=1;h.origin_valid=1;h.origin_orientation_xyzw[3]=1;
    h.pose[1][1].active=1;h.pose[1][1].flags=15;
    h.pose[1][1].orientation_xyzw[3]=1;h.pose[1][1].position_m[0]=.3f;
    CHECK(psx_openxr_hands_override(&h));
    CHECK(psx_openxr_hands(&got) && got.synthetic && got.age_ms==0 && got.pose[1][1].position_m[0]==.3f);
    uint64_t hands_seq=got.sequence;
    CHECK(psx_openxr_hands(&got) && got.sequence==hands_seq); /* Read only. */
    h.pose[1][1].orientation_xyzw[3]=0;CHECK(!psx_openxr_hands_override(&h));
    CHECK(psx_openxr_hands(&got) && got.sequence==hands_seq); /* Reject before mutation. */
    h.pose[1][1].orientation_xyzw[3]=1;h.pose[1][1].flags=16;CHECK(!psx_openxr_hands_override(&h));
    h.pose[1][1].flags=1;CHECK(psx_openxr_hands_override(&h)); /* Partial flags retained. */
    CHECK(psx_openxr_hands(&got) && got.pose[1][1].flags==1);
    h.pose[1][1].position_m[0]=NAN;CHECK(!psx_openxr_hands_override(&h));h.pose[1][1].position_m[0]=.3f;
    h.focused=0;CHECK(psx_openxr_hands_override(&h));
    CHECK(psx_openxr_hands(&got) && !got.focused && !got.pose[1][1].flags && !got.pose[1][1].active);
    h.focused=1;h.pose[1][1].active=0;CHECK(psx_openxr_hands_override(&h));
    CHECK(psx_openxr_hands(&got) && !got.pose[1][1].flags);
    CHECK(psx_openxr_hands_override(NULL));
    CHECK(psx_openxr_hands(&got) && !got.synthetic && !got.pose[1][1].flags);
    got.struct_size=0;CHECK(!psx_openxr_hands(&got));got.struct_size=sizeof got;
    h.pose[1][1].active=1;CHECK(psx_openxr_hands_override(&h));
    psx_openxr_shutdown();CHECK(psx_openxr_hands(&got) && !got.synthetic && !got.origin_valid);
    return 0;
}
