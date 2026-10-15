/* Source-owned grayscale/color ramp, actual submission callbacks and hidden GL.
 * No headset or retail payload required. Compile with function-section GC. */
#include "openxr_color_fixture_renderer.h"
#include <assert.h>
static PSXOpenXRStats fixture_xr;
void psx_openxr_stats(PSXOpenXRStats *out) { *out=fixture_xr; }
static PSXModOpenXRSurface fixture_surface;
static int fixture_surface_active;
int psx_openxr_surface_get(uint32_t id,PSXModOpenXRSurface *out) {
    if (id || !fixture_surface_active) return 0;
    *out=fixture_surface;return 1;
}
static double decode(double x) {return x<=.04045?x/12.92:pow((x+.055)/1.055,2.4);}
static void check_pixels(const unsigned char *got,const unsigned char *src,int flip,
                         double gamma,int linear,int alpha) {
    for(int y=0;y<2;y++) for(int x=0;x<256;x++) for(int c=0;c<(alpha?4:3);c++) {
        double v=src[((flip?1-y:y)*256+x)*4+c]/255.;
        if(c<3) {v=pow(v,1/gamma);if(linear)v=decode(v);}
        int expected=(int)floor(v*255+.5);
        assert(abs((int)got[(y*256+x)*4+c]-expected)<=1);
    }
}
int main(void) {
    int64_t formats[]={0x1234,PSX_XR_RGBA8,PSX_XR_SRGB8_ALPHA8};
    assert(psx_xr_color_format(formats,3)==PSX_XR_SRGB8_ALPHA8);
    assert(psx_xr_color_format(formats,2)==PSX_XR_RGBA8);
    assert(!psx_xr_color_format(formats,1));assert(!psx_xr_color_format(NULL,0));
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE,8);
    SDL_Window *win=SDL_CreateWindow("XR color fixture",0,0,256,64,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
    assert(win);s_ctx=SDL_GL_CreateContext(win);assert(s_ctx);assert(load_modern_gl());
    p_glGenVertexArrays(1,&s_present_vao);
    unsigned char src[256*2*4],got[sizeof src],native[sizeof src];
    for(int y=0;y<2;y++) for(int x=0;x<256;x++) {
        int i=(y*256+x)*4;
        src[i]=y?255-x:x;src[i+1]=y?x:255-x;src[i+2]=x/2;src[i+3]=77;
    }
    StereoPair *pair=&s_stereo_pair[0];s_stereo_current=0;
    pair->tex[0]=make_tex(GL_RGBA8,256,2,GL_RGBA,GL_UNSIGNED_BYTE);
    glBindTexture(GL_TEXTURE_2D,pair->tex[0]);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,256,2,GL_RGBA,GL_UNSIGNED_BYTE,src);
    assert(make_fbo(&pair->fbo[0],pair->tex[0],0));pair->tw[0]=256;pair->th[0]=2;
    for(int linear=0;linear<2;linear++) for(int gamma=0;gamma<2;gamma++) for(int enabled=0;enabled<2;enabled++) {
        fixture_xr.swapchain_format=linear?PSX_XR_RGBA8:PSX_XR_SRGB8_ALPHA8;
        GLuint target=make_tex((GLint)fixture_xr.swapchain_format,256,2,GL_RGBA,GL_UNSIGNED_BYTE),fbo=0;
        assert(make_fbo(&fbo,target,0));
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);glViewport(3,4,17,19);
        glEnable(GL_SCISSOR_TEST);glScissor(0,0,1,1);
        if(enabled) glEnable(0x8DB9);else glDisable(0x8DB9);
        s_present_gamma=gamma?1.7f:1.f;
        glEnable(GL_BLEND);glEnable(GL_DEPTH_TEST);glEnable(GL_STENCIL_TEST);glEnable(GL_CULL_FACE);
        glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_TRUE);
        assert(openxr_copy_eye(0,target,256,2));assert(glGetError()==GL_NO_ERROR);
        GLint viewport[4],read_fbo,draw_fbo;GLboolean mask[4];
        glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(0x8CAA,&read_fbo);glGetIntegerv(0x8CA6,&draw_fbo);
        glGetBooleanv(GL_COLOR_WRITEMASK,mask);
        assert(viewport[0]==3&&viewport[1]==4&&viewport[2]==17&&viewport[3]==19);
        assert(!read_fbo&&!draw_fbo&&!mask[0]&&mask[1]&&!mask[2]&&mask[3]);
        assert(glIsEnabled(GL_SCISSOR_TEST)&&glIsEnabled(0x8DB9)==enabled);
        assert(glIsEnabled(GL_BLEND)&&glIsEnabled(GL_DEPTH_TEST)&&glIsEnabled(GL_STENCIL_TEST)&&glIsEnabled(GL_CULL_FACE));
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);glReadBuffer(PSXGL_COLOR_ATTACHMENT0);
        glReadPixels(0,0,256,2,GL_RGBA,GL_UNSIGNED_BYTE,got);
        check_pixels(got,src,1,s_present_gamma,linear,1);
        /* Native already contains desktop gamma: do not apply it a second time. */
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);glDisable(0x8DB9);
        assert(openxr_color_draw(pair->tex[0],256,2,0,s_present_gamma,0));
        glReadBuffer(GL_BACK);glReadPixels(0,0,256,2,GL_RGBA,GL_UNSIGNED_BYTE,native);
        s_native_surface_rect[0]=s_native_surface_rect[1]=0;
        s_native_surface_rect[2]=256;s_native_surface_rect[3]=2;
        if(enabled)glEnable(0x8DB9);
        assert(openxr_copy_native(0,target,256,2));assert(glIsEnabled(0x8DB9)==enabled);
        p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);glReadBuffer(PSXGL_COLOR_ATTACHMENT0);
        glReadPixels(0,0,256,2,GL_RGBA,GL_UNSIGNED_BYTE,got);
        check_pixels(got,native,0,1,linear,0);assert(glGetError()==GL_NO_ERROR);
        p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);p_glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&target);
    }
    /* Execute the actual surface shader, rather than only its projection math.
     * A centered translucent red panel covers a source-owned blue scene. */
    unsigned char red[4]={255,0,0,128};
    for(int i=0;i<256*2;i++){src[i*4]=0;src[i*4+1]=0;src[i*4+2]=255;src[i*4+3]=255;}
    p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);glDisable(0x8DB9);
    glBindTexture(GL_TEXTURE_2D,pair->tex[0]);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,256,2,GL_RGBA,GL_UNSIGNED_BYTE,src);
    fixture_surface.struct_size=sizeof fixture_surface;fixture_surface.width_px=fixture_surface.height_px=1;
    fixture_surface.rgba=red;fixture_surface.position_m[2]=-1;fixture_surface.orientation_xyzw[3]=1;
    fixture_surface.size_m[0]=fixture_surface.size_m[1]=2;
    fixture_surface.flags=PSX_MOD_OPENXR_SURFACE_FACE_HEAD;
    for(int eye=0;eye<2;eye++){
        fixture_xr.pose[eye][6]=1;
        fixture_xr.fov[eye][0]=fixture_xr.fov[eye][3]=-.785398163f;
        fixture_xr.fov[eye][1]=fixture_xr.fov[eye][2]=.785398163f;
    }
    fixture_xr.swapchain_format=PSX_XR_SRGB8_ALPHA8;s_present_gamma=1;fixture_surface_active=1;
    GLuint target=make_tex(GL_RGBA8,256,2,GL_RGBA,GL_UNSIGNED_BYTE),fbo=0;
    assert(make_fbo(&fbo,target,0));p_glBindFramebuffer(PSXGL_FRAMEBUFFER,0);
    assert(openxr_copy_eye(0,target,256,2));assert(glGetError()==GL_NO_ERROR);
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);glReadPixels(0,0,256,2,GL_RGBA,GL_UNSIGNED_BYTE,got);
    for(int i=0;i<256*2;i++){
        assert(abs(got[i*4]-128)<=1 && got[i*4+1]==0 && abs(got[i*4+2]-127)<=1);
    }
    fixture_surface_active=0;
    /* A fresh published pair without a surface returns to the scene alone. */
    glBindTexture(GL_TEXTURE_2D,pair->tex[0]);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,256,2,GL_RGBA,GL_UNSIGNED_BYTE,src);
    assert(openxr_copy_eye(0,target,256,2));
    p_glBindFramebuffer(PSXGL_READ_FRAMEBUFFER,fbo);glReadPixels(0,0,256,2,GL_RGBA,GL_UNSIGNED_BYTE,got);
    check_pixels(got,src,1,1,0,1);
    assert(glGetError()==GL_NO_ERROR);
    SDL_GL_DeleteContext(s_ctx);SDL_DestroyWindow(win);SDL_Quit();return 0;
}
