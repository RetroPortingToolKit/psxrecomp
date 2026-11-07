#include "psx_openxr.h"
#include <stdio.h>
#include <math.h>
#include "xr_surface_math.h"
#include "xr_origin_diagnostics.h"
extern void psx_openxr_pose_bob(double q[2][4], double p[2][3]);
extern int psx_openxr_pose_override(double, double, double);
extern int psx_openxr_pose_pitch_override(double);
extern void psx_openxr_pose_clear(void);
/* Evaluate side effects and failures even when NDEBUG is defined. */
#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)
int main(void) {
    const double upright[4]={0,0,0,1}, upside[4]={0,0,1,0}, yaw180[4]={0,1,0,0};
    unsigned inversion=0;double up_dot=0;
    for(int i=0;i<9;i++)
        inversion=xr_origin_observe_inversion(inversion,1,upright,upside,&up_dot);
    CHECK(inversion==9 && up_dot < -0.5);
    inversion=xr_origin_observe_inversion(inversion,1,upright,upside,&up_dot);
    CHECK(inversion==10 && up_dot < -0.5);
    /* A valid sustained roll stays ambiguous but never requests a reset. */
    inversion=xr_origin_observe_inversion(inversion,1,upright,yaw180,&up_dot);
    CHECK(inversion==0 && fabs(up_dot-1.0)<1e-12);
    inversion=xr_origin_observe_inversion(7,0,upright,upside,&up_dot);
    CHECK(inversion==0 && up_dot==1.0);
    inversion=xr_origin_observe_inversion(7,1,upside,upside,&up_dot);
    CHECK(inversion==0 && fabs(up_dot-1.0)<1e-12);
    CHECK(!xr_origin_reference_reset_due(0,100,100));
    CHECK(!xr_origin_reference_reset_due(1,101,100));
    CHECK(xr_origin_reference_reset_due(1,100,100));
    /* Synthetic controls are absolute, bounded and inert after clearing. */
    double bob_q[2][4]={{0,0,0,1},{0,0,0,1}};
    double bob_p[2][3]={{-.03,0,0},{.03,0,0}};
    psx_openxr_pose_clear();
    CHECK(!psx_openxr_pose_override(NAN,0,0));
    CHECK(!psx_openxr_pose_override(11,0,0));
    CHECK(!psx_openxr_pose_pitch_override(INFINITY));
    CHECK(!psx_openxr_pose_pitch_override(91));
    psx_openxr_pose_bob(bob_q,bob_p);
    CHECK(bob_p[0][0]==-.03 && bob_q[0][3]==1);
    CHECK(psx_openxr_pose_override(.1,.2,-.3));
    CHECK(psx_openxr_pose_pitch_override(30));
    psx_openxr_pose_bob(bob_q,bob_p);
    CHECK(fabs(bob_p[0][0]-.07)<1e-12 && fabs(bob_p[1][0]-.13)<1e-12);
    CHECK(bob_p[0][1]==.2 && bob_p[1][2]==-.3);
    CHECK(fabs(bob_q[0][0]-sin(3.141592653589793/12))<1e-12);
    CHECK(bob_q[0][0]==bob_q[1][0] && bob_q[0][3]==bob_q[1][3]);
    psx_openxr_pose_clear();
    psx_openxr_pose_bob(bob_q,bob_p);
    CHECK(fabs(bob_p[0][0]-.07)<1e-12);
    /* Shared head center gives coherent binocular geometry. Billboarding must
     * ignore attachment roll, respond to head motion, and reject near-plane
     * crossings. Common translation of eye/head/surface preserves projection. */
    PSXModOpenXRSurface plane={0};plane.orientation_xyzw[3]=1;
    plane.position_m[2]=-1;plane.size_m[0]=plane.size_m[1]=.2f;
    float eye[7]={-.03f,0,0,0,0,0,1},head[3]={0,0,0};
    const float fov[4]={-.785398f,.785398f,.785398f,-.785398f};
    float left[36],right[36],other[36];
    CHECK(xr_surface_vertices_head(&plane,eye,fov,head,left));
    eye[0]=.03f;CHECK(xr_surface_vertices_head(&plane,eye,fov,head,right));
    CHECK(left[0]>right[0]);
    plane.flags=PSX_MOD_OPENXR_SURFACE_FACE_HEAD;
    CHECK(xr_surface_vertices_head(&plane,eye,fov,head,left));
    plane.orientation_xyzw[0]=1;plane.orientation_xyzw[3]=0;
    CHECK(xr_surface_vertices_head(&plane,eye,fov,head,right));
    for(int i=0;i<36;i++)CHECK(fabsf(left[i]-right[i])<.00001f);
    head[0]=.5f;CHECK(xr_surface_vertices_head(&plane,eye,fov,head,other));
    CHECK(fabsf(left[0]-other[0])+fabsf(left[3]-other[3])>.001f);
    head[0]=0;plane.position_m[0]+=2;plane.position_m[1]+=3;
    eye[0]+=2;eye[1]+=3;head[0]+=2;head[1]+=3;
    CHECK(xr_surface_vertices_head(&plane,eye,fov,head,other));
    for(int i=0;i<36;i++)CHECK(fabsf(left[i]-other[i])<.00001f);
    plane.position_m[2]=.1f;CHECK(!xr_surface_vertices_head(&plane,eye,fov,head,other));
    plane.position_m[0]=head[0];plane.position_m[1]=head[1];plane.position_m[2]=head[2];
    CHECK(!xr_surface_vertices_head(&plane,eye,fov,head,other));
    uint8_t pixel[4]={255,0,0,255};
    PSXModOpenXRSurface surface={0};surface.struct_size=sizeof surface;
    surface.width_px=surface.height_px=1;surface.rgba=pixel;
    surface.size_m[0]=surface.size_m[1]=.1f;surface.orientation_xyzw[3]=1;
    CHECK(psx_openxr_surface_valid(&surface));CHECK(!psx_openxr_surface(&surface));
    surface.flags=PSX_MOD_OPENXR_SURFACE_FACE_HEAD;CHECK(psx_openxr_surface_valid(&surface));
    surface.flags=2;CHECK(!psx_openxr_surface_valid(&surface));surface.flags=0;
    surface.id=2;CHECK(!psx_openxr_surface_valid(&surface));surface.id=0;
    surface.position_m[0]=NAN;CHECK(!psx_openxr_surface_valid(&surface));surface.position_m[0]=0;
    surface.orientation_xyzw[3]=0;CHECK(!psx_openxr_surface_valid(&surface));surface.orientation_xyzw[3]=1;
    surface.width_px=1025;CHECK(!psx_openxr_surface_valid(&surface));surface.width_px=1;
    surface.rgba=NULL;CHECK(!psx_openxr_surface_valid(&surface));
    /* Interpolation gate: only a compiled, enabled, initialized, RUNNING session
     * suspends presentation interpolation; every other state is a no-op. */
    CHECK(psx_openxr_interp_gate(1,1,1,1));
    for(unsigned m=0;m<15;m++)
        CHECK(!psx_openxr_interp_gate(m&1,(m>>1)&1,(m>>2)&1,(m>>3)&1));
    CHECK(!psx_openxr_session_active()); /* nothing enabled in this process */
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
    h.age_ms=151;CHECK(psx_openxr_hands_override(&h));
    CHECK(psx_openxr_hands(&got) && got.synthetic && got.age_ms==151);
    h.age_ms=0;CHECK(psx_openxr_hands_override(&h));
    CHECK(psx_openxr_hands(&got) && got.age_ms==0);
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
