/* gl_dxgi_present.h - present GL frames through a DXGI flip-model swapchain
 * (Windows; gl_dxgi_present.cpp). All calls run on the present thread with
 * its GL context current. */
#ifndef PSX_GL_DXGI_PRESENT_H
#define PSX_GL_DXGI_PRESENT_H
#ifdef __cplusplus
extern "C" {
#endif

/* Set up for the window (an HWND); 1 when active. */
int dxp_init(void *hwnd, int w, int h);
int dxp_active(void);
/* Wait for the swapchain's frame latency, resize to w x h if needed, lock the
 * back buffer: the GL framebuffer to draw this frame into, 0 on failure. Its
 * origin is the top-left (D3D): draw Y-flipped. */
unsigned dxp_begin(int w, int h);
/* Unlock and present with vsync (one frame per refresh). 1 on success. */
int dxp_present(void);
void dxp_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
