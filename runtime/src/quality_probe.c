/* quality_probe.c -- fill PsxHostInfo for graphics preset detection
 * (quality_tier.h, docs/QUALITY_PRESETS.md).
 *
 * psx_quality_probe_host() uses OS facts only (no window, no GL): it runs on
 * every launch to fingerprint the machine. psx_quality_probe_gl_renderer()
 * opens a hidden 1x1 OpenGL window to read GL_RENDERER; it runs only when a
 * detection is due and the OS did not name the GPU. */
#include "quality_tier.h"
#include "psx_sdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static void copy_trim(char *dst, size_t cap, const char *src) {
    while (*src == ' ' || *src == '\t') src++;
    size_t n = strlen(src);
    while (n && (src[n - 1] == '\n' || src[n - 1] == '\r' || src[n - 1] == ' ')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

#if defined(__linux__)
static int read_line(const char *path, char *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[256];
    int ok = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (ok) copy_trim(out, cap, buf);
    return ok && out[0];
}
#endif

void psx_quality_probe_host(PsxHostInfo *h) {
    memset(h, 0, sizeof *h);
    h->logical_cores = SDL_GetCPUCount();
    const int ram = SDL_GetSystemRAM();
    h->ram_mb = ram > 0 ? (uint64_t)ram : 0;
#if defined(__APPLE__)
    size_t len = sizeof h->cpu;
    if (sysctlbyname("machdep.cpu.brand_string", h->cpu, &len, NULL, 0) != 0) h->cpu[0] = 0;
    /* Apple silicon's GPU is named after the chip, which is what GL_RENDERER
     * reports too ("Apple M1"); Intel Macs leave the GPU to the GL probe. */
    if (strncmp(h->cpu, "Apple M", 7) == 0) copy_trim(h->gpu, sizeof h->gpu, h->cpu);
#elif defined(_WIN32)
    {
        HKEY k;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                          "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                          0, KEY_READ, &k) == ERROR_SUCCESS) {
            char buf[128]; DWORD sz = sizeof buf, type = 0;
            if (RegQueryValueExA(k, "ProcessorNameString", NULL, &type, (LPBYTE)buf, &sz) ==
                    ERROR_SUCCESS && type == REG_SZ) {
                buf[sizeof buf - 1] = 0;
                copy_trim(h->cpu, sizeof h->cpu, buf);
            }
            RegCloseKey(k);
        }
        DISPLAY_DEVICEA dd;
        memset(&dd, 0, sizeof dd);
        dd.cb = sizeof dd;
        for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
            if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
                copy_trim(h->gpu, sizeof h->gpu, dd.DeviceString);
                break;
            }
            memset(&dd, 0, sizeof dd);
            dd.cb = sizeof dd;
        }
    }
#elif defined(__linux__)
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof line, f)) {
                if (strncmp(line, "model name", 10) == 0) {
                    const char *colon = strchr(line, ':');
                    if (colon) copy_trim(h->cpu, sizeof h->cpu, colon + 1);
                    break;
                }
            }
            fclose(f);
        }
        char vendor[64] = "", product[64] = "";
        read_line("/sys/class/dmi/id/board_vendor", vendor, sizeof vendor);
        read_line("/sys/class/dmi/id/product_name", product, sizeof product);
        if ((strcmp(vendor, "Valve") == 0 &&
             (strcmp(product, "Jupiter") == 0 || strcmp(product, "Galileo") == 0)))
            h->steam_deck = 1;
        /* The GPU's PCI id fingerprints the GPU without opening GL. */
        char v[16] = "", d[16] = "";
        if (read_line("/sys/class/drm/card0/device/vendor", v, sizeof v) &&
            read_line("/sys/class/drm/card0/device/device", d, sizeof d))
            snprintf(h->gpu_id, sizeof h->gpu_id, "%s:%s", v, d);
        else if (read_line("/sys/class/drm/card1/device/vendor", v, sizeof v) &&
                 read_line("/sys/class/drm/card1/device/device", d, sizeof d))
            snprintf(h->gpu_id, sizeof h->gpu_id, "%s:%s", v, d);
    }
#endif
    {
        const char *e = getenv("SteamDeck");
        if (e && e[0] == '1') h->steam_deck = 1;
    }
}

#ifndef GL_RENDERER
#define GL_RENDERER 0x1F01
#endif

int psx_quality_probe_gl_renderer(char *out, size_t cap) {
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_Window *w = SDL_CreateWindow("psxrecomp gpu probe", 0, 0, 1, 1,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!w) return 0;
    SDL_GLContext ctx = SDL_GL_CreateContext(w);
    int ok = 0;
    if (ctx) {
        typedef const unsigned char *(*GetStringFn)(unsigned int);
        GetStringFn get = (GetStringFn)SDL_GL_GetProcAddress("glGetString");
        const unsigned char *r = get ? get(GL_RENDERER) : NULL;
        if (r && r[0]) { copy_trim(out, cap, (const char *)r); ok = 1; }
        SDL_GL_MakeCurrent(w, NULL);
        SDL_GL_DeleteContext(ctx);
    }
    SDL_DestroyWindow(w);
    return ok;
}
