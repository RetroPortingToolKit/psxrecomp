/* gl_dxgi_present.c - present the GL renderer's frames through a DXGI
 * flip-model swapchain (Windows).
 *
 * Windowed and borderless GL swaps go through DWM composition. With swap
 * interval 0 (NVIDIA's vsync'd SwapBuffers stalls the render queue, #723) the
 * present thread's wall-clock pacing drifts against the refresh and DWM merges
 * two presents into one refresh: a 60 fps stream showed 35-60 distinct frames
 * on screen. A FLIP_DISCARD swapchain presented with Present(1, 0) puts every
 * frame in its own refresh, and only the present thread waits for it.
 *
 * GL draws into the swapchain's back buffer through WGL_NV_DX_interop2: the
 * present thread blits its finished slot into a renderbuffer registered on
 * the D3D11 back buffer, unlocks it and presents. Anything unavailable (the
 * extension, the device, the swapchain) leaves the SDL_GL_SwapWindow path in
 * place. Experimental: PSX_GL_DXGI_PRESENT=1 turns it on. */
#ifdef _WIN32
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h>   /* this TU defines the D3D11/DXGI IIDs it uses */
#include <d3d11.h>
#include <dxgi1_3.h>
#include "psx_sdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_dxgi_present.h"

typedef unsigned int GLuint_;
typedef unsigned int GLenum_;
#define GL_RENDERBUFFER_ 0x8D41
#define GL_FRAMEBUFFER_ 0x8D40
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define WGL_ACCESS_WRITE_DISCARD_NV_ 0x0002

typedef HANDLE (WINAPI *PFN_wglDXOpenDeviceNV)(void *);
typedef BOOL (WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE);
typedef HANDLE (WINAPI *PFN_wglDXRegisterObjectNV)(HANDLE, void *, GLuint_, GLenum_, GLenum_);
typedef BOOL (WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE, HANDLE);
typedef BOOL (WINAPI *PFN_wglDXLockObjectsNV)(HANDLE, int, HANDLE *);
typedef const char *(WINAPI *PFN_wglGetExtensionsStringARB)(HDC);
typedef void (APIENTRY *PFN_glGenX)(int, GLuint_ *);
typedef void (APIENTRY *PFN_glDelX)(int, const GLuint_ *);
typedef void (APIENTRY *PFN_glBindX)(GLenum_, GLuint_);
typedef void (APIENTRY *PFN_glFbRb)(GLenum_, GLenum_, GLenum_, GLuint_);

typedef struct Dxp {
    int active;
    HWND hwnd;
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain2 *sc;
    HANDLE wait, hdev, hbuf;
    ID3D11Texture2D *bb;
    GLuint_ rb, fbo;
    UINT w, h;
    PFN_wglDXOpenDeviceNV open;
    PFN_wglDXCloseDeviceNV close;
    PFN_wglDXRegisterObjectNV reg;
    PFN_wglDXUnregisterObjectNV unreg;
    PFN_wglDXLockObjectsNV lock, unlock;
    PFN_glGenX gen_rb, gen_fb;
    PFN_glDelX del_rb, del_fb;
    PFN_glBindX bind_rb, bind_fb;
    PFN_glFbRb fb_rb;
} Dxp;
static Dxp D;

static void fail(const char *why) {
    fprintf(stdout, "psxrecomp: DXGI present unavailable (%s); swapping through SDL\n", why);
}

static void unregister_buffer() {
    if (D.hbuf) { D.unreg(D.hdev, D.hbuf); D.hbuf = NULL; }
    if (D.fbo) { D.del_fb(1, &D.fbo); D.fbo = 0; }
    if (D.rb) { D.del_rb(1, &D.rb); D.rb = 0; }
    if (D.bb) { ID3D11Texture2D_Release(D.bb); D.bb = NULL; }
}

static int register_buffer() {
    if (FAILED(IDXGISwapChain2_GetBuffer(D.sc, 0, &IID_ID3D11Texture2D, (void **)&D.bb))) return 0;
    D.gen_rb(1, &D.rb);
    D.hbuf = D.reg(D.hdev, D.bb, D.rb, GL_RENDERBUFFER_, WGL_ACCESS_WRITE_DISCARD_NV_);
    if (!D.hbuf) return 0;
    D.gen_fb(1, &D.fbo);
    D.bind_fb(GL_FRAMEBUFFER_, D.fbo);
    D.fb_rb(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_RENDERBUFFER_, D.rb);
    D.bind_fb(GL_FRAMEBUFFER_, 0);
    return 1;
}

int dxp_init(void *hwnd, int w, int h) {
    /* Opt-in while experimental: the GL window surface and the swapchain on
     * the same HWND left the screen frozen in the first test. */
    const char *env = getenv("PSX_GL_DXGI_PRESENT");
    if (!env || env[0] != '1') return 0;
    if (D.active) return 1;
    PFN_wglGetExtensionsStringARB ext = (PFN_wglGetExtensionsStringARB)(void *)wglGetProcAddress("wglGetExtensionsStringARB");
    const char *exts = ext ? ext(wglGetCurrentDC()) : NULL;
    if (!exts || !strstr(exts, "WGL_NV_DX_interop2")) { fail("no WGL_NV_DX_interop2"); return 0; }
    D.open = (PFN_wglDXOpenDeviceNV)(void *)wglGetProcAddress("wglDXOpenDeviceNV");
    D.close = (PFN_wglDXCloseDeviceNV)(void *)wglGetProcAddress("wglDXCloseDeviceNV");
    D.reg = (PFN_wglDXRegisterObjectNV)(void *)wglGetProcAddress("wglDXRegisterObjectNV");
    D.unreg = (PFN_wglDXUnregisterObjectNV)(void *)wglGetProcAddress("wglDXUnregisterObjectNV");
    D.lock = (PFN_wglDXLockObjectsNV)(void *)wglGetProcAddress("wglDXLockObjectsNV");
    D.unlock = (PFN_wglDXLockObjectsNV)(void *)wglGetProcAddress("wglDXUnlockObjectsNV");
    D.gen_rb = (PFN_glGenX)SDL_GL_GetProcAddress("glGenRenderbuffers");
    D.gen_fb = (PFN_glGenX)SDL_GL_GetProcAddress("glGenFramebuffers");
    D.del_rb = (PFN_glDelX)SDL_GL_GetProcAddress("glDeleteRenderbuffers");
    D.del_fb = (PFN_glDelX)SDL_GL_GetProcAddress("glDeleteFramebuffers");
    D.bind_rb = (PFN_glBindX)SDL_GL_GetProcAddress("glBindRenderbuffer");
    D.bind_fb = (PFN_glBindX)SDL_GL_GetProcAddress("glBindFramebuffer");
    D.fb_rb = (PFN_glFbRb)SDL_GL_GetProcAddress("glFramebufferRenderbuffer");
    if (!D.open || !D.close || !D.reg || !D.unreg || !D.lock || !D.unlock || !D.gen_rb ||
        !D.gen_fb || !D.del_rb || !D.del_fb || !D.bind_rb || !D.bind_fb || !D.fb_rb) {
        fail("interop entry points"); return 0;
    }
    /* The GL context's adapter: the high-performance GPU (what SDL's GL
     * context runs on with a single discrete GPU). */
    IDXGIFactory2 *factory = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&factory))) { fail("factory"); return 0; }
    IDXGIAdapter1 *adapter = NULL;
    if (FAILED(IDXGIFactory2_EnumAdapters1(factory, 0, &adapter))) adapter = NULL;
    const D3D_FEATURE_LEVEL levels[1] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice((IDXGIAdapter *)adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                   NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1,
                                   D3D11_SDK_VERSION, &D.dev, NULL, &D.ctx);
    if (adapter) IDXGIAdapter1_Release(adapter);
    if (FAILED(hr)) { IDXGIFactory2_Release(factory); fail("D3D11 device"); return 0; }
    DXGI_SWAP_CHAIN_DESC1 d; memset(&d, 0, sizeof d);
    d.Width = (UINT)(w > 0 ? w : 1); d.Height = (UINT)(h > 0 ? h : 1);
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    d.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    IDXGISwapChain1 *sc1 = NULL;
    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)D.dev, (HWND)hwnd, &d, NULL, NULL, &sc1);
    if (SUCCEEDED(hr)) IDXGIFactory2_MakeWindowAssociation(factory, (HWND)hwnd, DXGI_MWA_NO_ALT_ENTER);
    IDXGIFactory2_Release(factory);
    if (FAILED(hr)) { char b[64]; snprintf(b, sizeof b, "swapchain 0x%08lx", hr); fail(b); dxp_shutdown(); return 0; }
    hr = IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain2, (void **)&D.sc);
    IDXGISwapChain1_Release(sc1);
    if (FAILED(hr)) { fail("IDXGISwapChain2"); dxp_shutdown(); return 0; }
    IDXGISwapChain2_SetMaximumFrameLatency(D.sc, 1);
    D.wait = IDXGISwapChain2_GetFrameLatencyWaitableObject(D.sc);
    D.hdev = D.open(D.dev);
    if (!D.hdev) { fail("wglDXOpenDeviceNV"); dxp_shutdown(); return 0; }
    if (!register_buffer()) { fail("wglDXRegisterObjectNV"); dxp_shutdown(); return 0; }
    D.hwnd = (HWND)hwnd; D.w = d.Width; D.h = d.Height; D.active = 1;
    fprintf(stdout, "psxrecomp: DXGI flip presentation on (%ux%u, vblank-aligned)\n", D.w, D.h);
    return 1;
}

int dxp_active(void) { return D.active; }

unsigned dxp_begin(int w, int h) {
    if (!D.active || w <= 0 || h <= 0) return 0;
    if (D.wait) WaitForSingleObjectEx(D.wait, 100, TRUE);
    if ((UINT)w != D.w || (UINT)h != D.h) {
        unregister_buffer();
        ID3D11DeviceContext_ClearState(D.ctx);
        ID3D11DeviceContext_Flush(D.ctx);
        if (FAILED(IDXGISwapChain2_ResizeBuffers(D.sc, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN,
                                                 DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) ||
            !register_buffer()) {
            fail("resize"); dxp_shutdown(); return 0;
        }
        D.w = (UINT)w; D.h = (UINT)h;
    }
    if (!D.lock(D.hdev, 1, &D.hbuf)) return 0;
    return D.fbo;
}

int dxp_present(void) {
    if (!D.active) return 0;
    D.unlock(D.hdev, 1, &D.hbuf);
    const HRESULT hr = IDXGISwapChain2_Present(D.sc, 1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        fail("device removed"); dxp_shutdown(); return 0;
    }
    return SUCCEEDED(hr);
}

void dxp_shutdown(void) {
    if (D.hdev) unregister_buffer();
    if (D.hdev) { D.close(D.hdev); D.hdev = NULL; }
    if (D.wait) { CloseHandle(D.wait); D.wait = NULL; }
    if (D.sc) { IDXGISwapChain2_Release(D.sc); D.sc = NULL; }
    if (D.ctx) { ID3D11DeviceContext_Release(D.ctx); D.ctx = NULL; }
    if (D.dev) { ID3D11Device_Release(D.dev); D.dev = NULL; }
    D.active = 0;
}
#endif
