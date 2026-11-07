#include "xr_origin_diagnostics.h"
/* Opt-in Win32 OpenXR lifecycle. No guest state, console diagnostics or timing
 * phases: located poses and projection submission belong to one host frame.
 *
 * THREADING: every entry point runs on the emulation/main thread that owns the
 * GL context. Plugin hooks (psx_mod_openxr_*), the GL present path
 * (gl_swap_with_osd) and TCP handlers (executed at the debug_server_poll safe
 * point; the debug I/O thread only queues requests) are all that thread, and
 * no other thread creates a GL context or touches XR. Hence no locking. */
#include "psx_openxr.h"
#include "vr_pose_math.h"
#include "openxr_color.h"
#include "frame_trace.h"
#include "gpu_gl_renderer.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
static PSXOpenXRStats s_stats;
static PSXModOpenXRSurface s_surfaces[2];
static uint8_t *s_surface_pixels[2];
static size_t s_surface_capacity[2];
static unsigned s_surface_mask;

static uint64_t s_pair_id,s_pair_cycle;
static uint32_t s_source;
static uint64_t s_native_frame;
static double s_quad_distance,s_quad_width,s_quad_height;
static PSXModOpenXRInput s_input;
static PSXModOpenXRHands s_hands;
static uint64_t s_hands_sequence, s_hands_sample_ms;
static void hands_clear(void) {
    memset(&s_hands,0,sizeof s_hands);s_hands.struct_size=sizeof s_hands;
    s_hands.sequence=++s_hands_sequence;s_hands.age_ms=UINT32_MAX;
    s_hands_sample_ms=0;
}
static int s_input_override;
static PSXModOpenXRInput s_injected;
static int s_hands_override;
static PSXModOpenXRHands s_injected_hands;
static int valid_pose_numbers(const double q[4],const double p[3]) {
    double n=0;
    for(int i=0;i<4;i++){if(!isfinite(q[i]))return 0;n+=q[i]*q[i];}
    for(int i=0;i<3;i++)if(!isfinite(p[i]) || fabs(p[i])>100)return 0;
    return n>=.5 && n<=1.5;
}
/* Test-only synthetic head motion: translation in room-space metres and an optional pitch, added to
 * both eyes after the origin is established so it reads as the head moving in the world. The
 * translation checks that the world shifts rigidly with the head. The pitch exists because the
 * defects that only appear when looking down cannot be reached by translation, and because the only
 * other look path is unwired. Armed by the first override call; default off, so shipped behaviour
 * is unchanged. */
static int s_pose_bob_armed;
static double s_pose_bob_m[3];
static double s_pose_bob_pitch_deg;
void psx_openxr_pose_bob(double q[2][4],double p[2][3]) {
    if(!s_pose_bob_armed)return;
    for(int i=0;i<2;i++)for(int j=0;j<3;j++)p[i][j]+=s_pose_bob_m[j];
    if(s_pose_bob_pitch_deg!=0.0) {
        double h=s_pose_bob_pitch_deg*(3.141592653589793/180.0)*0.5,sq=sin(h),cq=cos(h);
        for(int i=0;i<2;i++) {
            double x=q[i][0],y=q[i][1],z=q[i][2],w=q[i][3];
            /* Left multiply by a rotation about the XR X axis: a look up or down. */
            q[i][0]=sq*w+cq*x;q[i][1]=cq*y-sq*z;q[i][2]=sq*y+cq*z;q[i][3]=cq*w-sq*x;
        }
    }
}
int psx_openxr_pose_override(double x_m,double y_m,double z_m) {
    if(!isfinite(x_m)||!isfinite(y_m)||!isfinite(z_m))return 0;
    if(fabs(x_m)>10.0||fabs(y_m)>10.0||fabs(z_m)>10.0)return 0;
    s_pose_bob_m[0]=x_m;s_pose_bob_m[1]=y_m;s_pose_bob_m[2]=z_m;s_pose_bob_armed=1;
    return 1;
}
int psx_openxr_pose_pitch_override(double pitch_deg) {
    if(!isfinite(pitch_deg) || fabs(pitch_deg)>90.0)return 0;
    s_pose_bob_pitch_deg=pitch_deg;s_pose_bob_armed=1;
    return 1;
}
void psx_openxr_pose_clear(void) {
    s_pose_bob_m[0]=s_pose_bob_m[1]=s_pose_bob_m[2]=0.0;s_pose_bob_pitch_deg=0.0;s_pose_bob_armed=0;
}
int psx_openxr_hands_override(const PSXModOpenXRHands *hands) {
    if(!hands){s_hands_override=0;hands_clear();return 1;}
    if(hands->struct_size!=sizeof *hands || hands->focused>1 || hands->origin_valid>1)return 0;
    for(int i=0;i<4;i++)if(!isfinite(hands->origin_orientation_xyzw[i]))return 0;
    for(int i=0;i<3;i++)if(!isfinite(hands->origin_position_m[i]) || fabs(hands->origin_position_m[i])>100)return 0;
    if(hands->origin_valid && !valid_pose_numbers(hands->origin_orientation_xyzw,hands->origin_position_m))return 0;
    for(int e=0;e<2;e++)for(int k=0;k<2;k++) {
        const PSXModTrackedPose *v=&hands->pose[e][k];
        if(v->active>1 || (v->flags & ~15u))return 0;
        double q[4],p[3];for(int i=0;i<4;i++)q[i]=v->orientation_xyzw[i];
        for(int i=0;i<3;i++)p[i]=v->position_m[i];
        for(int i=0;i<4;i++)if(!isfinite(q[i]))return 0;
        for(int i=0;i<3;i++)if(!isfinite(p[i]) || fabs(p[i])>100)return 0;
        if((v->flags & 3u) && !valid_pose_numbers(q,p))return 0;
    }
    s_injected_hands=*hands;s_injected_hands.synthetic=1;
    s_injected_hands.sequence=++s_hands_sequence;
    for(int e=0;e<2;e++)for(int k=0;k<2;k++)
        if(!hands->focused || !hands->origin_valid || !hands->pose[e][k].active)
            memset(&s_injected_hands.pose[e][k],0,sizeof(PSXModTrackedPose));
    s_hands_override=1;return 1;
}
int psx_openxr_input_override(const PSXModOpenXRInput *input) {
    if (!input) { s_input_override=0; memset(&s_injected,0,sizeof s_injected); return 1; }
    if (input->struct_size!=sizeof *input || input->focused>1 ||
        input->active[0]>1 || input->active[1]>1) return 0;
    for(int e=0;e<2;e++) for(int a=0;a<2;a++)
        if(!isfinite(input->stick[e][a]) || fabsf(input->stick[e][a])>1) return 0;
    for(int e=0;e<2;e++) {
        if(input->trigger_active[e]>1 || input->squeeze_active[e]>1 ||
           (input->buttons[e] & ~PSX_MOD_XR_CLICKS) ||
           (input->buttons_active[e] & ~PSX_MOD_XR_CLICKS) ||
           !isfinite(input->trigger[e]) || input->trigger[e]<0 || input->trigger[e]>1 ||
           !isfinite(input->squeeze[e]) || input->squeeze[e]<0 || input->squeeze[e]>1)return 0;
    }
    s_injected=*input; s_input_override=1; return 1;
}
void psx_openxr_input_snapshot(PSXModOpenXRInput *out) { if(out)*out=s_input; }
void psx_openxr_pair_metadata(uint64_t id,uint64_t cycle){s_pair_id=id;s_pair_cycle=cycle;s_source=1;s_native_frame=0;}
void psx_openxr_native_metadata(uint64_t frame){s_pair_id=s_pair_cycle=0;s_source=2;s_native_frame=frame;}
int psx_openxr_surface_valid(const PSXModOpenXRSurface *v) {
    if(!v || v->struct_size!=sizeof *v || v->id>=2 || !v->rgba ||
       !v->width_px || !v->height_px || v->width_px>1024 || v->height_px>1024)return 0;
    if(v->flags&~PSX_MOD_OPENXR_SURFACE_FACE_HEAD)return 0;
    double norm=0;
    for(int i=0;i<3;i++)if(!isfinite(v->position_m[i]) || fabs(v->position_m[i])>100)return 0;
    for(int i=0;i<4;i++){if(!isfinite(v->orientation_xyzw[i]))return 0;norm+=v->orientation_xyzw[i]*v->orientation_xyzw[i];}
    if(fabs(norm-1)>.01)return 0;
    for(int i=0;i<2;i++)if(!isfinite(v->size_m[i]) || v->size_m[i]<.001f || v->size_m[i]>2)return 0;
    return 1;
}
#if defined(PSX_OPENXR)
#include "host_time.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <GL/gl.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

/* Frame-trace stage helper: perf ticks -> microseconds (CPU submission/wait
 * wall time only, never GPU execution time). */
static uint64_t xr_trace_us(uint64_t ticks) {
    static double inv = 0.0;
    if (inv == 0.0) {
        double f = (double)gl_renderer_perf_frequency();
        inv = f > 0.0 ? 1000000.0 / f : 1.0;
    }
    return (uint64_t)((double)ticks * inv);
}
static XrInstance s_instance;
static XrSystemId s_system;
static XrSession s_session;
static XrSpace s_space;
static XrSpace s_view_space;
static XrActionSet s_actions;
static XrAction s_stick;
static XrAction s_trigger,s_squeeze,s_click[4];
static XrAction s_pose_action[2];
static XrSpace s_pose_space[2][2];
static XrPath s_hand[2];
static XrTime s_origin_reset_time;
static unsigned s_origin_inversion_streak;
static XrSwapchain s_chain[2];
static XrSwapchainImageOpenGLKHR *s_images[2];
static uint32_t s_count[2];
static int s_w[2],s_h[2],s_origin_valid;
static XrView s_views[2];
static PSXModRenderView s_render[2];
static double s_oq[4],s_op[3];
static XrTime s_time;
/* Cadence telemetry for the frame trace: the runtime's own predicted display
 * time delta between submissions (what the compositor actually saw), and when
 * the pose for the current frame was sampled (xrLocateViews return). */
static XrTime s_prev_display_time;
static int s_prev_display_valid;
static uint32_t s_display_delta_us;
static uint64_t s_pose_sample_ticks;
/* Loss handling. A lost session/instance (or an unusable swapchain) is only
 * flagged where it is detected; service_loss() tears the stack down at the end
 * of the public call so no caller loses handles mid-sequence. */
#define XR_RETRY_MS 3000u
static int s_reset_pending,s_exit_pending,s_recovering;
static const char *s_reset_stage;
static uint64_t s_retry_ms;
static int check(XrResult r,const char *stage) {
    s_stats.result=r;
    if(XR_FAILED(r)) {
        s_stats.stage=s_stats.last_failure=stage;s_stats.last_failure_result=r;s_stats.failures++;
        if(r==XR_ERROR_SESSION_LOST || r==XR_ERROR_INSTANCE_LOST) {
            s_reset_pending=1;s_reset_stage=r==XR_ERROR_SESSION_LOST?"session_lost":"instance_lost";
        }
        return 0;
    }
    return 1;
}
static int input_initialize(void) {
    XrActionSetCreateInfo set={XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(set.actionSetName,"gameplay"); strcpy(set.localizedActionSetName,"Gameplay");
    if(!check(xrCreateActionSet(s_instance,&set,&s_actions),"action_set"))return 0;
    if(!check(xrStringToPath(s_instance,"/user/hand/left",&s_hand[0]),"left_hand_path") ||
       !check(xrStringToPath(s_instance,"/user/hand/right",&s_hand[1]),"right_hand_path"))return 0;
    XrActionCreateInfo action={XR_TYPE_ACTION_CREATE_INFO};
    strcpy(action.actionName,"thumbstick"); strcpy(action.localizedActionName,"Thumbstick");
    action.actionType=XR_ACTION_TYPE_VECTOR2F_INPUT;
    action.countSubactionPaths=2; action.subactionPaths=s_hand;
    if(!check(xrCreateAction(s_actions,&action,&s_stick),"stick_action"))return 0;
    action.actionType=XR_ACTION_TYPE_FLOAT_INPUT;
    strcpy(action.actionName,"trigger");strcpy(action.localizedActionName,"Trigger");
    if(!check(xrCreateAction(s_actions,&action,&s_trigger),"trigger_action"))return 0;
    strcpy(action.actionName,"squeeze");strcpy(action.localizedActionName,"Grip squeeze");
    if(!check(xrCreateAction(s_actions,&action,&s_squeeze),"squeeze_action"))return 0;
    const char *names[4]={"primary","secondary","menu","stick_click"};
    const char *labels[4]={"Primary button","Secondary button","Menu","Stick click"};
    action.actionType=XR_ACTION_TYPE_BOOLEAN_INPUT;
    for(int i=0;i<4;i++) {
        strcpy(action.actionName,names[i]);strcpy(action.localizedActionName,labels[i]);
        if(!check(xrCreateAction(s_actions,&action,&s_click[i]),names[i]))return 0;
    }
    action.actionType=XR_ACTION_TYPE_POSE_INPUT;
    for(int k=0;k<2;k++) {
        strcpy(action.actionName,k?"aim_pose":"grip_pose");
        strcpy(action.localizedActionName,k?"Aim pose":"Grip pose");
        if(!check(xrCreateAction(s_actions,&action,&s_pose_action[k]),"pose_action"))return 0;
        for(int e=0;e<2;e++) {
            XrActionSpaceCreateInfo ci={XR_TYPE_ACTION_SPACE_CREATE_INFO};
            ci.action=s_pose_action[k];ci.subactionPath=s_hand[e];ci.poseInActionSpace.orientation.w=1;
            if(!check(xrCreateActionSpace(s_session,&ci,&s_pose_space[e][k]),"pose_space"))return 0;
        }
    }
    XrPath profile;
    if(!check(xrStringToPath(s_instance,"/interaction_profiles/oculus/touch_controller",&profile),"touch_profile"))return 0;
    const char *paths[2][7]={
        {"/user/hand/left/input/thumbstick","/user/hand/left/input/trigger/value",
         "/user/hand/left/input/squeeze/value","/user/hand/left/input/x/click",
         "/user/hand/left/input/y/click","/user/hand/left/input/menu/click",
         "/user/hand/left/input/thumbstick/click"},
        {"/user/hand/right/input/thumbstick","/user/hand/right/input/trigger/value",
         "/user/hand/right/input/squeeze/value","/user/hand/right/input/a/click",
         "/user/hand/right/input/b/click",NULL,"/user/hand/right/input/thumbstick/click"}};
    XrAction actions[7]={s_stick,s_trigger,s_squeeze,s_click[0],s_click[1],s_click[2],s_click[3]};
    XrActionSuggestedBinding bindings[17];uint32_t n=0;
    for(int e=0;e<2;e++)for(int a=0;a<7;a++)if(paths[e][a]) {
        bindings[n].action=actions[a];
        if(!check(xrStringToPath(s_instance,paths[e][a],&bindings[n].binding),"touch_input_path"))return 0;
        n++;
    }
    const char *pose_paths[2][2]={{"/user/hand/left/input/grip/pose","/user/hand/left/input/aim/pose"},
                               {"/user/hand/right/input/grip/pose","/user/hand/right/input/aim/pose"}};
    for(int e=0;e<2;e++)for(int k=0;k<2;k++) {
        bindings[n].action=s_pose_action[k];
        if(!check(xrStringToPath(s_instance,pose_paths[e][k],&bindings[n].binding),"pose_binding"))return 0;
        n++;
    }
    XrInteractionProfileSuggestedBinding suggest={XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile=profile; suggest.countSuggestedBindings=n; suggest.suggestedBindings=bindings;
    if(!check(xrSuggestInteractionProfileBindings(s_instance,&suggest),"touch_bindings"))return 0;
    XrSessionActionSetsAttachInfo attach={XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets=1; attach.actionSets=&s_actions;
    return check(xrAttachSessionActionSets(s_session,&attach),"attach_actions");
}
static int initialize(void) {
    uint32_t count=0;
    XrExtensionProperties *ext=NULL;
    if(!check(xrEnumerateInstanceExtensionProperties(NULL,0,&count,NULL),"extensions"))return 0;
    ext=calloc(count,sizeof *ext); if(!ext){s_stats.stage="extensions_memory";return 0;}
    for(uint32_t i=0;i<count;i++)ext[i].type=XR_TYPE_EXTENSION_PROPERTIES;
    int supported=0;
    if(check(xrEnumerateInstanceExtensionProperties(NULL,count,&count,ext),"extensions"))
        for(uint32_t i=0;i<count;i++)
            if(!strcmp(ext[i].extensionName,XR_KHR_OPENGL_ENABLE_EXTENSION_NAME))supported=1;
    free(ext);
    if(!supported){s_stats.stage="opengl_extension_missing";return 0;}
    const char *extensions[]={XR_KHR_OPENGL_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci={XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName,"PSXRecomp stereo");
    strcpy(ci.applicationInfo.engineName,"psxrecomp");
    ci.applicationInfo.apiVersion=XR_API_VERSION_1_0;
    ci.enabledExtensionCount=1;ci.enabledExtensionNames=extensions;
    if(!check(xrCreateInstance(&ci,&s_instance),"instance"))return 0;
    XrInstanceProperties ip={XR_TYPE_INSTANCE_PROPERTIES};
    if(check(xrGetInstanceProperties(s_instance,&ip),"instance_properties"))
        memcpy(s_stats.runtime,ip.runtimeName,sizeof s_stats.runtime);
    s_stats.runtime[sizeof s_stats.runtime-1]=0;
    XrSystemGetInfo si={XR_TYPE_SYSTEM_GET_INFO};si.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if(!check(xrGetSystem(s_instance,&si,&s_system),"system"))return 0;
    PFN_xrGetOpenGLGraphicsRequirementsKHR requirements=NULL;
    if(!check(xrGetInstanceProcAddr(s_instance,"xrGetOpenGLGraphicsRequirementsKHR",
                                  (PFN_xrVoidFunction*)&requirements),"graphics_requirements_proc"))return 0;
    XrGraphicsRequirementsOpenGLKHR req={XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if(!requirements || !check(requirements(s_instance,s_system,&req),"graphics_requirements"))return 0;
    GLint major=0,minor=0;glGetIntegerv(0x821B,&major);glGetIntegerv(0x821C,&minor);
    XrVersion version=XR_MAKE_VERSION(major,minor,0);
    s_stats.gl_version=version;s_stats.min_gl_version=req.minApiVersionSupported;
    s_stats.max_gl_version=req.maxApiVersionSupported;
    if(version<req.minApiVersionSupported){s_stats.stage="opengl_version";s_stats.failures++;return 0;}
    XrGraphicsBindingOpenGLWin32KHR binding={XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    binding.hDC=wglGetCurrentDC();binding.hGLRC=wglGetCurrentContext();
    if(!binding.hDC || !binding.hGLRC){s_stats.stage="opengl_context";return 0;}
    XrSessionCreateInfo sci={XR_TYPE_SESSION_CREATE_INFO};sci.next=&binding;sci.systemId=s_system;
    if(!check(xrCreateSession(s_instance,&sci,&s_session),"session"))return 0;
    if(++s_stats.session_generation==0)s_stats.session_generation=1;
    if(!input_initialize())return 0;
    XrReferenceSpaceCreateInfo space={XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL;space.poseInReferenceSpace.orientation.w=1;
    if(!check(xrCreateReferenceSpace(s_session,&space,&s_space),"local_space"))return 0;
    XrViewConfigurationView config[2]={{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if(!check(xrEnumerateViewConfigurationViews(s_instance,s_system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
              2,&count,config),"view_config") || count!=2)return 0;
    uint32_t formats_n=0;
    if(!check(xrEnumerateSwapchainFormats(s_session,0,&formats_n,NULL),"formats"))return 0;
    int64_t *formats=calloc(formats_n,sizeof *formats);if(!formats){s_stats.stage="formats_memory";return 0;}
    if(!check(xrEnumerateSwapchainFormats(s_session,formats_n,&formats_n,formats),"formats")) {
        free(formats);return 0;
    }
    s_stats.swapchain_format=psx_xr_color_format(formats,formats_n);
    free(formats);
    if(!s_stats.swapchain_format){s_stats.stage="color_format_missing";return 0;}
    for(int eye=0;eye<2;eye++) {
        s_w[eye]=config[eye].recommendedImageRectWidth;s_h[eye]=config[eye].recommendedImageRectHeight;
        XrSwapchainCreateInfo sc={XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;sc.format=s_stats.swapchain_format;
        sc.sampleCount=1;sc.width=s_w[eye];sc.height=s_h[eye];sc.faceCount=1;sc.arraySize=1;sc.mipCount=1;
        if(!check(xrCreateSwapchain(s_session,&sc,&s_chain[eye]),"swapchain"))return 0;
        if(!check(xrEnumerateSwapchainImages(s_chain[eye],0,&s_count[eye],NULL),"images"))return 0;
        s_images[eye]=calloc(s_count[eye],sizeof *s_images[eye]);
        if(!s_images[eye]){s_stats.stage="images_memory";return 0;}
        for(uint32_t i=0;i<s_count[eye];i++)s_images[eye][i].type=XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        if(!check(xrEnumerateSwapchainImages(s_chain[eye],s_count[eye],&s_count[eye],
                    (XrSwapchainImageBaseHeader*)s_images[eye]),"images"))return 0;
    }
    s_stats.initialized=1;s_stats.stage="waiting_session";return 1;
}
static int events(void) {
    for(;;) {
        XrEventDataBuffer event={XR_TYPE_EVENT_DATA_BUFFER};
        XrResult r=xrPollEvent(s_instance,&event);
        if(r==XR_EVENT_UNAVAILABLE)break;
        if(!check(r,"events"))return 0;
        if(event.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *e=(void*)&event;
            s_stats.state=e->state;
            if(e->state==XR_SESSION_STATE_READY && !s_stats.running) {
                XrSessionBeginInfo begin={XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if(!check(xrBeginSession(s_session,&begin),"begin_session"))return 0;
                s_stats.running=1;
            } else if(e->state==XR_SESSION_STATE_STOPPING) {
                if(!check(xrEndSession(s_session),"end_session"))return 0;
                s_stats.running=0;
            } else if(e->state==XR_SESSION_STATE_LOSS_PENDING) {
                s_reset_pending=1;s_reset_stage="session_loss";return 0;
            } else if(e->state==XR_SESSION_STATE_EXITING) {
                s_exit_pending=1;return 0;
            }
        } else if(event.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            /* Recenter from the next valid located pair. */
            const XrEventDataReferenceSpaceChangePending *e=(const void*)&event;
            if(e->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL) s_origin_reset_time=e->changeTime;
        } else if(event.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            s_reset_pending=1;s_reset_stage="instance_loss";return 0;
        }
    }
    return 1;
}
/* Sync the gameplay action set. Needed by both the input poll and hand
 * location (a pose action is only active after a sync), so a plugin that only
 * calls psx_mod_openxr_begin still gets hands. True only for XR_SUCCESS: the
 * session is focused and the actions are current. */
static int sync_actions(void) {
    XrActiveActionSet active={s_actions,XR_NULL_PATH};
    XrActionsSyncInfo sync={XR_TYPE_ACTIONS_SYNC_INFO};sync.countActiveActionSets=1;sync.activeActionSets=&active;
    XrResult r=xrSyncActions(s_session,&sync);
    return check(r,"sync_actions") && r==XR_SUCCESS;
}
/* Focus for hands comes from the session state events and a successful action
 * sync in this call, never from whether the plugin polled psx_mod_openxr_input. */
static void hands_locate(int focused) {
    PSXModOpenXRHands current=s_hands;
    current.predicted_time=(uint64_t)s_time;
    current.focused=focused && s_stats.state==XR_SESSION_STATE_FOCUSED;
    current.origin_valid=s_origin_valid;
    memcpy(current.origin_position_m,s_op,sizeof s_op);
    memcpy(current.origin_orientation_xyzw,s_oq,sizeof s_oq);
    if(current.focused && current.origin_valid)for(int e=0;e<2;e++)for(int k=0;k<2;k++) {
        XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};
        get.action=s_pose_action[k];get.subactionPath=s_hand[e];
        XrActionStatePose state={XR_TYPE_ACTION_STATE_POSE};
        if(!check(xrGetActionStatePose(s_session,&get,&state),"pose_state"))continue;
        PSXModTrackedPose *v=&current.pose[e][k];v->active=state.isActive;
        if(!state.isActive)continue;
        XrSpaceLocation loc={XR_TYPE_SPACE_LOCATION};
        XrResult r=xrLocateSpace(s_pose_space[e][k],s_space,s_time,&loc);
        if(!check(r,"locate_hand") || r!=XR_SUCCESS)continue;
        v->flags=(uint32_t)loc.locationFlags & 15u;
        if((v->flags & 3u)!=3u)continue; /* Values remain zero for incomplete poses. */
        double norm=0;float q[4]={loc.pose.orientation.x,loc.pose.orientation.y,
                               loc.pose.orientation.z,loc.pose.orientation.w};
        float p[3]={loc.pose.position.x,loc.pose.position.y,loc.pose.position.z};
        int valid=1;
        for(int i=0;i<4;i++){if(!isfinite(q[i]))valid=0;norm+=(double)q[i]*q[i];}
        for(int i=0;i<3;i++)if(!isfinite(p[i]) || fabsf(p[i])>100)valid=0;
        if(!valid || norm<.5 || norm>1.5){v->flags=0;continue;}
        memcpy(v->orientation_xyzw,q,sizeof q);memcpy(v->position_m,p,sizeof p);
    }
    s_hands=current;s_hands_sample_ms=psx_host_mono_ms();
}
/* Destroy every XR object (children before parents). Safe on a partially
 * initialised stack and on a lost session: destroy calls need no live session. */
static void xr_destroy(void) {
    /* A new session must not inherit the previous one's display times: the
     * first delta after a re-init would be meaningless. */
    s_prev_display_valid=0;s_prev_display_time=0;
    for(int eye=0;eye<2;eye++) {
        for(int k=0;k<2;k++) {
            if(s_pose_space[eye][k])xrDestroySpace(s_pose_space[eye][k]);
            s_pose_space[eye][k]=XR_NULL_HANDLE;
        }
        if(s_chain[eye])xrDestroySwapchain(s_chain[eye]);
        s_chain[eye]=XR_NULL_HANDLE;free(s_images[eye]);s_images[eye]=NULL;s_count[eye]=0;
    }
    if(s_space)xrDestroySpace(s_space);s_space=XR_NULL_HANDLE;
    if(s_view_space)xrDestroySpace(s_view_space);s_view_space=XR_NULL_HANDLE;
    if(s_session)xrDestroySession(s_session);s_session=XR_NULL_HANDLE;
    if(s_actions)xrDestroyActionSet(s_actions);s_actions=XR_NULL_HANDLE;s_stick=XR_NULL_HANDLE;
    s_trigger=s_squeeze=XR_NULL_HANDLE;memset(s_click,0,sizeof s_click);
    memset(s_pose_action,0,sizeof s_pose_action);
    if(s_instance)xrDestroyInstance(s_instance);s_instance=XR_NULL_HANDLE;
    s_origin_valid=0;s_origin_reset_time=0;s_origin_inversion_streak=0;
    s_stats.origin_inversion_streak=s_stats.origin_ambiguous=0;s_stats.origin_up_dot=1.0f;
    s_stats.state=0;
    s_stats.initialized=s_stats.running=s_stats.tracking=s_stats.frame_open=0;
}
/* Act on a flagged loss. A lost session/instance or an unusable swapchain tears
 * everything down so the caller falls back to the flat present, then keeps XR
 * enabled and re-initialises after XR_RETRY_MS. EXITING (the user closed the
 * headset app) tears down and leaves XR disabled until an explicit enable.
 * Returns 1 if it tore the stack down. Call only at the end of a public entry. */
static int service_loss(void) {
    if(!s_reset_pending && !s_exit_pending)return 0;
    const int exiting=s_exit_pending && !s_reset_pending;
    xr_destroy(); /* a lost session cannot end its frame; xr_destroy clears frame_open */
    memset(&s_input,0,sizeof s_input);s_input.struct_size=sizeof s_input;
    hands_clear();
    s_quad_distance=s_quad_width=s_quad_height=0;
    if(exiting) {
        s_stats.enabled=0;s_stats.stage="session_exit";s_recovering=0;s_retry_ms=0;
    } else {
        s_stats.losses++;s_stats.stage=s_reset_stage?s_reset_stage:"lost";
        s_recovering=1;s_retry_ms=psx_host_mono_ms()+XR_RETRY_MS;
    }
    s_reset_pending=s_exit_pending=0;s_reset_stage=NULL;
    return 1;
}
#endif
int psx_openxr_hands(PSXModOpenXRHands *out) {
    if(!out || out->struct_size!=sizeof *out)return 0;
    if(s_hands_override){*out=s_injected_hands;return 1;}
    *out=s_hands;out->struct_size=sizeof *out;out->age_ms=UINT32_MAX;
#if defined(PSX_OPENXR)
    if(s_hands_sample_ms) {
        uint64_t now=psx_host_mono_ms();
        uint64_t age=now>=s_hands_sample_ms?now-s_hands_sample_ms:UINT32_MAX;
        out->age_ms=age<UINT32_MAX?(uint32_t)age:UINT32_MAX;
    }
#endif
    return 1;
}
int psx_openxr_interp_gate(uint32_t compiled,uint32_t enabled,uint32_t initialized,uint32_t running) {
    return compiled && enabled && initialized && running;
}
int psx_openxr_session_active(void) {
    return psx_openxr_interp_gate(s_stats.compiled,s_stats.enabled,s_stats.initialized,s_stats.running);
}
void psx_openxr_stats(PSXOpenXRStats *out) {
    if(out){*out=s_stats;
#if defined(PSX_OPENXR)
        out->compiled=1;
#endif
    }
}
void psx_openxr_recenter(void) {
    hands_clear();
#if defined(PSX_OPENXR)
    s_origin_valid=0;s_origin_inversion_streak=0;
    s_stats.origin_inversion_streak=s_stats.origin_ambiguous=0;
#endif
}
void psx_openxr_shutdown(void) {
#if defined(PSX_OPENXR)
    if(s_stats.frame_open)psx_openxr_end(0,NULL);
    xr_destroy();
    s_reset_pending=s_exit_pending=s_recovering=0;s_reset_stage=NULL;s_retry_ms=0;
#endif
    for(int i=0;i<2;i++){free(s_surface_pixels[i]);s_surface_pixels[i]=NULL;s_surface_capacity[i]=0;}
    s_surface_mask=0;
    s_stats.enabled=0;
    s_quad_distance=s_quad_width=s_quad_height=0;
    memset(&s_input,0,sizeof s_input);
    hands_clear();
    s_input_override=0;
    s_hands_override=0;memset(&s_injected_hands,0,sizeof s_injected_hands);
}
int psx_openxr_enable(int enabled) {
    psx_openxr_shutdown();memset(&s_stats,0,sizeof s_stats);
#if defined(PSX_OPENXR)
    s_stats.compiled=1;s_stats.enabled=enabled!=0;s_stats.stage=enabled?"requested":"off";return 1;
#else
    s_stats.stage="not_compiled";return !enabled;
#endif
}
int psx_openxr_view(uint32_t eye,PSXModRenderView *out) {
#if defined(PSX_OPENXR)
    if(s_stats.frame_open && s_stats.tracking && out && eye<2){*out=s_render[eye];return 1;}
#endif
    return 0;
}
int psx_openxr_input(PSXModOpenXRInput *out) {
    if(!out || out->struct_size!=sizeof *out)return 0;
    uint64_t seq=s_input.sequence+1;
    memset(&s_input,0,sizeof s_input);s_input.struct_size=sizeof s_input;s_input.sequence=seq;
    if(s_input_override) {
        s_input=s_injected;s_input.sequence=seq;s_input.synthetic=1;
        for(int e=0;e<2;e++) if(!s_input.focused || !s_input.active[e])
            s_input.stick[e][0]=s_input.stick[e][1]=0;
        for(int e=0;e<2;e++) {
            if(!s_input.focused || !s_input.trigger_active[e])s_input.trigger[e]=0;
            if(!s_input.focused || !s_input.squeeze_active[e])s_input.squeeze[e]=0;
            s_input.buttons[e] &= s_input.focused ? s_input.buttons_active[e] : 0;
        }
        if(!s_input.focused)hands_clear();
        *out=s_input;return 1;
    }
#if defined(PSX_OPENXR)
    if(s_stats.enabled && s_stats.initialized && !s_stats.frame_open && events() && s_stats.running) {
        if(sync_actions() && s_stats.state==XR_SESSION_STATE_FOCUSED) {
            PSXModOpenXRInput current=s_input;current.focused=1;int valid=1;
            for(int e=0;e<2;e++) {
                XrActionStateGetInfo get={XR_TYPE_ACTION_STATE_GET_INFO};get.action=s_stick;get.subactionPath=s_hand[e];
                XrActionStateVector2f state={XR_TYPE_ACTION_STATE_VECTOR2F};
                if(!check(xrGetActionStateVector2f(s_session,&get,&state),"stick_state")) { valid=0; break; }
                current.active[e]=state.isActive;
                if(state.isActive) {
                    current.stick[e][0]=state.currentState.x;current.stick[e][1]=state.currentState.y;
                    for(int a=0;a<2;a++) if(!isfinite(current.stick[e][a]) || fabsf(current.stick[e][a])>1) valid=0;
                }
                XrAction analog[2]={s_trigger,s_squeeze};
                for(int a=0;a<2;a++) {
                    get.action=analog[a];
                    XrActionStateFloat value={XR_TYPE_ACTION_STATE_FLOAT};
                    if(!check(xrGetActionStateFloat(s_session,&get,&value),"analog_action_state")) { valid=0;break; }
                    float v=value.isActive?value.currentState:0;
                    if(!isfinite(v) || v<0 || v>1) { valid=0;break; }
                    if(a==0){current.trigger_active[e]=value.isActive;current.trigger[e]=v;}
                    else {current.squeeze_active[e]=value.isActive;current.squeeze[e]=v;}
                }
                for(int a=0;a<4;a++) {
                    get.action=s_click[a];
                    XrActionStateBoolean value={XR_TYPE_ACTION_STATE_BOOLEAN};
                    if(!check(xrGetActionStateBoolean(s_session,&get,&value),"click_action_state")) { valid=0;break; }
                    if(value.isActive) {
                        current.buttons_active[e] |= 1u<<a;
                        if(value.currentState)current.buttons[e] |= 1u<<a;
                    }
                }
                if(!valid)break;
            }
            if(valid)s_input=current;
        }
    }
    /* events()/sync may have flagged a loss: tear down, return neutral input. */
    (void)service_loss();
#endif
    if(!s_input.focused)hands_clear();
    *out=s_input;return 1; /* Always a fresh neutral sample when unavailable. */
}
int psx_openxr_begin(int width,int height,double units) {
#if defined(PSX_OPENXR)
    if(!s_stats.enabled || s_stats.frame_open)return 0;
    s_surface_mask=0;
    s_quad_distance=s_quad_width=s_quad_height=0;
    s_pair_id=s_pair_cycle=s_native_frame=0;s_source=0;
    hands_clear();
    s_stats.units_per_meter=units;
    if(!s_stats.initialized) {
        if(s_retry_ms) { /* backing off after a loss: flat until the retry time */
            if(psx_host_mono_ms()<s_retry_ms)return 0;
            s_retry_ms=0;
        }
        if(!initialize()) {
            if(s_recovering) {
                /* The runtime/headset is still gone after a loss. Stay enabled,
                 * release the partial stack and try again later; the failure
                 * stays inspectable through the stats. */
                xr_destroy();s_retry_ms=psx_host_mono_ms()+XR_RETRY_MS;return 0;
            }
            /* Retain the failed producer/result. Avoid repeated costly startup until
             * explicit re-enable; a disconnected headset is an inspectable state. */
            psx_openxr_shutdown();return 0;
        }
        s_recovering=0;
    }
    if(!events() || !s_stats.running){(void)service_loss();return 0;}
    /* Open this frame's trace record before the blocking waits. The record is
     * committed by psx_openxr_end(); the two failure returns below commit it
     * themselves so a lost session cannot leave a record open forever. */
    psx_frame_trace_open();
    /* Cleared so a frame that never reaches xrLocateViews (no render request,
     * lost tracking, bad view maths) cannot report a stale pose age. */
    s_pose_sample_ticks=0;
    uint64_t trace_t = gl_renderer_perf_ticks();
    XrFrameWaitInfo wait={XR_TYPE_FRAME_WAIT_INFO};XrFrameState fs={XR_TYPE_FRAME_STATE};
    if(!check(xrWaitFrame(s_session,&wait,&fs),"wait_frame")){
        psx_frame_trace_add(PSX_TRACE_XR_WAIT_FRAME, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
        /* Session-level submission failure: the stereo cadence context is gone,
         * so drop any stale pair-time estimate rather than carrying it into the
         * recovery attempt. */
        psx_mod_stereo_reset_budget();
        (void)service_loss();psx_frame_trace_commit();return 0;}
    psx_frame_trace_add(PSX_TRACE_XR_WAIT_FRAME, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
    s_stats.waits++;s_time=fs.predictedDisplayTime;s_stats.predicted_time=(uint64_t)s_time;
    /* The runtime's own clock between submissions. This is the cadence the
     * compositor saw, independent of our frame timing: delta / display period
     * is how many granted periods one presented frame consumed. */
    s_display_delta_us=0;
    if(s_prev_display_valid && s_time>s_prev_display_time) {
        uint64_t d=(uint64_t)(s_time-s_prev_display_time)/1000u;
        s_display_delta_us=d>0xFFFFFFFFull?0xFFFFFFFFu:(uint32_t)d;
    }
    s_prev_display_time=s_time;s_prev_display_valid=1;
    /* Record the runtime display period with its name for cadence analysis. */
    /* What the runtime wants us to hit is the only true deadline for the pair.
     * Hand it to stereo shedding (PSX VBlank is a guest redraw budget, not a
     * compositor deadline) and record it with the runtime name so a trace can be
     * read without guessing the headset's refresh. */
    psx_mod_stereo_set_budget_ms(fs.predictedDisplayPeriod>0?(double)fs.predictedDisplayPeriod/1e6:0.0);
    psx_frame_trace_session(s_stats.runtime,
                            fs.predictedDisplayPeriod>0?(uint32_t)(fs.predictedDisplayPeriod/1000):0u);
    trace_t = gl_renderer_perf_ticks();
    XrFrameBeginInfo begin={XR_TYPE_FRAME_BEGIN_INFO};
    if(!check(xrBeginFrame(s_session,&begin),"begin_frame")){
        psx_frame_trace_add(PSX_TRACE_XR_BEGIN_FRAME, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
        psx_mod_stereo_reset_budget();
        (void)service_loss();psx_frame_trace_commit();return 0;}
    psx_frame_trace_add(PSX_TRACE_XR_BEGIN_FRAME, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
    s_stats.frame_open=1;s_stats.tracking=0;
    if(!fs.shouldRender){psx_openxr_end(0,NULL);return 0;}
    XrViewLocateInfo loc={XR_TYPE_VIEW_LOCATE_INFO};loc.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    loc.displayTime=s_time;loc.space=s_space;
    XrViewState vs={XR_TYPE_VIEW_STATE};uint32_t count=0;
    memset(s_views,0,sizeof s_views);s_views[0].type=s_views[1].type=XR_TYPE_VIEW;
    uint64_t locate_t=gl_renderer_perf_ticks();
    int located=check(xrLocateViews(s_session,&loc,&vs,2,&count,s_views),"locate_views");
    psx_frame_trace_add(PSX_TRACE_XR_LOCATE, xr_trace_us(gl_renderer_perf_ticks()-locate_t));
    s_pose_sample_ticks=gl_renderer_perf_ticks();
    uint64_t prep_t=s_pose_sample_ticks;
    s_stats.view_flags=(uint64_t)vs.viewStateFlags;
    if(!located || count!=2 ||
       (vs.viewStateFlags&(XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT))!=
       (XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT)) {
        s_stats.stage="tracking_invalid";psx_openxr_end(0,NULL);return 0;
    }
    double q[2][4],p[2][3];
    for(int i=0;i<2;i++) {
        XrPosef *v=&s_views[i].pose;
        q[i][0]=v->orientation.x;q[i][1]=v->orientation.y;q[i][2]=v->orientation.z;q[i][3]=v->orientation.w;
        p[i][0]=v->position.x;p[i][1]=v->position.y;p[i][2]=v->position.z;
    }
    if(xr_origin_reference_reset_due(s_origin_reset_time!=0,s_origin_reset_time,s_time)) {
        s_origin_valid=0;s_origin_reset_time=0;s_origin_inversion_streak=0;
    }
    if(!s_origin_valid) {
        memcpy(s_oq,q[0],sizeof s_oq);
        for(int j=0;j<3;j++)s_op[j]=(p[0][j]+p[1][j])*.5;
        s_origin_valid=1;
        s_origin_inversion_streak=0;
    }
    /* A sustained roll can be a legitimate headset pose. Report the ambiguous
     * case without changing the reference; explicit recenter or the OpenXR
     * reference-space change event owns origin recovery. */
    double origin_up_dot=1.0;
    s_origin_inversion_streak=xr_origin_observe_inversion(
        s_origin_inversion_streak,s_origin_valid,s_oq,q[0],&origin_up_dot);
    s_stats.origin_up_dot=(float)origin_up_dot;
    s_stats.origin_inversion_streak=s_origin_inversion_streak;
    s_stats.origin_ambiguous=s_origin_inversion_streak>=10u;
    double separation=0;
    for(int j=0;j<3;j++)separation+=(p[1][j]-p[0][j])*(p[1][j]-p[0][j]);
    s_stats.ipd_m=(float)sqrt(separation);
#ifndef PSX_NO_DEBUG_TOOLS
    psx_openxr_pose_bob(q, p);   /* test-only synthetic head motion; no-op unless armed */
#endif
    for(int i=0;i<2;i++) {
        XrFovf *v=&s_views[i].fov;
        double f[4]={v->angleLeft,v->angleRight,v->angleUp,v->angleDown};
        if(!vr_pose_to_view(q[i],p[i],s_oq,s_op,f,units,width,height,&s_render[i])) {
            s_stats.stage="view_math";psx_openxr_end(0,NULL);return 0;
        }
    }
    for(int i=0;i<2;i++) {
        for(int j=0;j<3;j++)s_stats.pose[i][j]=(float)p[i][j];
        for(int j=0;j<4;j++)s_stats.pose[i][j+3]=(float)q[i][j];
        s_stats.fov[i][0]=s_views[i].fov.angleLeft;s_stats.fov[i][1]=s_views[i].fov.angleRight;
        s_stats.fov[i][2]=s_views[i].fov.angleUp;s_stats.fov[i][3]=s_views[i].fov.angleDown;
        s_stats.view[i]=s_render[i];
    }
    hands_locate(s_stats.state==XR_SESSION_STATE_FOCUSED && sync_actions());
    psx_frame_trace_add(PSX_TRACE_HOST_PREP, xr_trace_us(gl_renderer_perf_ticks()-prep_t));
    s_stats.tracking=1;s_stats.stage="located";return 1;
#endif
    return 0;
}
int psx_openxr_quad(double distance,double width,double height) {
    if(!isfinite(distance) || !isfinite(width) || !isfinite(height))return 0;
    if(distance!=0 && (distance<.25 || distance>20 || width<=0 || width>20 || height<=0 || height>20))return 0;
#if defined(PSX_OPENXR)
    if(!s_stats.frame_open || !s_stats.tracking)return 0;
    if(distance && !s_view_space) {
        XrReferenceSpaceCreateInfo space={XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.referenceSpaceType=XR_REFERENCE_SPACE_TYPE_VIEW;space.poseInReferenceSpace.orientation.w=1;
        if(!check(xrCreateReferenceSpace(s_session,&space,&s_view_space),"quad_view_space"))return 0;
    }
    s_quad_distance=distance;s_quad_width=distance?width:0;s_quad_height=distance?height:0;return 1;
#else
    return 0;
#endif
}
int psx_openxr_end(int keep,PSXOpenXRCopy copy) {
#if defined(PSX_OPENXR)
    if(!s_stats.frame_open)return 0;
    /* Defensive open: normally psx_openxr_begin() already opened the record. */
    psx_frame_trace_open();
    uint64_t trace_t;
    XrCompositionLayerProjectionView pv[2]={{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    int complete=keep && copy && s_stats.tracking;
    int quad=s_quad_distance>0;
    for(int eye=0;eye<(quad?1:2) && complete;eye++) {
        uint32_t index=0;
        trace_t = gl_renderer_perf_ticks();
        XrSwapchainImageAcquireInfo ai={XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if(!check(xrAcquireSwapchainImage(s_chain[eye],&ai,&index),"acquire")){complete=0;break;}
        psx_frame_trace_add(PSX_TRACE_XR_ACQUIRE, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
        XrSwapchainImageWaitInfo wi={XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};wi.timeout=1000000000;
        trace_t = gl_renderer_perf_ticks();
        XrResult wr=xrWaitSwapchainImage(s_chain[eye],&wi);
        /* A timeout leaves ownership acquired: wait again (bounded) before
         * releasing. Only XR_SUCCESS means the image is writable; the timeout
         * code is a success code and must not be mistaken for it. */
        if(wr==XR_TIMEOUT_EXPIRED){wi.timeout=2000000000;wr=xrWaitSwapchainImage(s_chain[eye],&wi);}
        psx_frame_trace_add(PSX_TRACE_XR_WAIT_IMAGE, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
        int writable=check(wr,"wait_image") && wr==XR_SUCCESS;
        if(writable) {
            trace_t = gl_renderer_perf_ticks();
            if(index>=s_count[eye] || !copy(eye,s_images[eye][index].image,s_w[eye],s_h[eye])) {
                s_stats.stage="copy_image";s_stats.failures++;complete=0;
            }
            psx_frame_trace_add(PSX_TRACE_XR_COPY, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
            trace_t = gl_renderer_perf_ticks();
            XrSwapchainImageReleaseInfo ri={XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if(!check(xrReleaseSwapchainImage(s_chain[eye],&ri),"release"))complete=0;
            psx_frame_trace_add(PSX_TRACE_XR_RELEASE, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
        } else {
            /* Acquired but never writable. Release it best-effort so the acquire
             * count cannot leak; a runtime that refuses (the wait never completed)
             * leaves this swapchain unusable for the next acquire, so rebuild the
             * whole stack rather than fail every later frame. */
            XrSwapchainImageReleaseInfo ri={XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            (void)xrReleaseSwapchainImage(s_chain[eye],&ri);
            if(wr==XR_TIMEOUT_EXPIRED){s_stats.stage=s_stats.last_failure="wait_image_timeout";s_stats.failures++;}
            if(!s_reset_pending){s_reset_pending=1;s_reset_stage="swapchain_wait_failed";}
            complete=0;
        }
        pv[eye].pose=s_views[eye].pose;pv[eye].fov=s_views[eye].fov;
        pv[eye].subImage.swapchain=s_chain[eye];pv[eye].subImage.imageRect.extent.width=s_w[eye];
        pv[eye].subImage.imageRect.extent.height=s_h[eye];
    }
    XrCompositionLayerProjection layer={XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space=s_space;layer.viewCount=2;layer.views=pv;
    XrCompositionLayerQuad surface={XR_TYPE_COMPOSITION_LAYER_QUAD};
    surface.space=s_view_space;surface.eyeVisibility=XR_EYE_VISIBILITY_BOTH;
    surface.subImage=pv[0].subImage;surface.pose.orientation.w=1;
    surface.pose.position.z=(float)-s_quad_distance;
    surface.size.width=(float)s_quad_width;surface.size.height=(float)s_quad_height;
    const XrCompositionLayerBaseHeader *layers[]={quad?(void*)&surface:(void*)&layer};
    XrFrameEndInfo end={XR_TYPE_FRAME_END_INFO};end.displayTime=s_time;
    end.environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount=complete?1:0;end.layers=complete?layers:NULL;
    trace_t = gl_renderer_perf_ticks();
    int ok=check(xrEndFrame(s_session,&end),"end_frame");s_stats.frame_open=0;
    psx_frame_trace_add(PSX_TRACE_XR_END_FRAME, xr_trace_us(gl_renderer_perf_ticks()-trace_t));
    if(s_pose_sample_ticks)
        psx_frame_trace_add(PSX_TRACE_POSE_AGE,
                            xr_trace_us(gl_renderer_perf_ticks()-s_pose_sample_ticks));
    psx_frame_trace_xr_cadence(s_display_delta_us, complete);
    if(ok && complete) {
        s_stats.submitted_session_generation=s_stats.session_generation;
        s_stats.submitted_surfaces=quad?0:((s_surface_mask&1)!=0)+((s_surface_mask&2)!=0);
        s_stats.submitted++;s_stats.submitted_pair_id=s_pair_id;
        s_stats.submitted_guest_cycle=s_pair_cycle;s_stats.submitted_predicted_time=(uint64_t)s_time;
        s_stats.submitted_layer=quad?2:1;
        s_stats.submitted_source=s_source;s_stats.submitted_native_frame=s_native_frame;
        if(s_source==2)s_stats.native_submitted++;
        if(quad){s_stats.quad_submitted++;s_stats.quad_distance_m=s_quad_distance;
            s_stats.quad_width_m=s_quad_width;s_stats.quad_height_m=s_quad_height;}
        /* A fresh stereo pair already labelled the record source 1; only
         * annotate native presentation when nothing else claimed it. */
        psx_frame_trace_source(2);
    } else {
        s_stats.empty++;s_stats.submitted_surfaces=0;
        psx_frame_trace_flag(PSX_FRAME_TRACE_XR_EMPTY);
        psx_frame_trace_flag(PSX_FRAME_TRACE_STALE_PAIR);
        psx_frame_trace_source(2);
    }
    if(ok && complete)s_stats.stage="submitted";
    s_quad_distance=s_quad_width=s_quad_height=0;
    psx_frame_trace_commit();
    (void)service_loss(); /* after the frame is closed: lost session / bad swapchain */
    return ok && complete;
#else
    return 0;
#endif
}

int psx_openxr_surface(const PSXModOpenXRSurface *v) {
    if(!psx_openxr_surface_valid(v))return 0;
    if(!s_stats.frame_open || !s_stats.tracking || s_quad_distance>0)return 0;
    size_t bytes=(size_t)v->width_px*v->height_px*4;
    if(s_surface_capacity[v->id]<bytes) {
        uint8_t *buffer=realloc(s_surface_pixels[v->id],bytes);
        if(!buffer){s_stats.surface_failures++;return 0;}
        s_surface_pixels[v->id]=buffer;s_surface_capacity[v->id]=bytes;
    }
    memcpy(s_surface_pixels[v->id],v->rgba,bytes);
    s_surfaces[v->id]=*v;s_surfaces[v->id].rgba=s_surface_pixels[v->id];
    s_surface_mask|=1u<<v->id;s_stats.surface_uploads++;return 1;
}
int psx_openxr_surface_get(uint32_t id,PSXModOpenXRSurface *out) {
    if(!out || id>=2 || !(s_surface_mask&(1u<<id)) || !s_stats.frame_open || s_quad_distance>0)return 0;
    *out=s_surfaces[id];return 1;
}
