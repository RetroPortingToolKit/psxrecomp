/* beetle_main.cpp — psx-beetle entry point.
 * Standalone harness around Beetle PSX libretro core. SDL window for
 * Beetle's framebuffer, keyboard input, TCP debug server on port 4380. */

#include "psx_sdl.h"
#include "frame_pacing.h"
#include "parity_trace.h"
#include "device_trace.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
int  beetle_init(const char* bios_path);
int  beetle_init_with_disc(const char* bios_path, const char* disc_path);
int  beetle_wtrace_arm(uint32_t lo, uint32_t hi);
int  beetle_rtrace_arm(uint32_t lo, uint32_t hi);
void beetle_shutdown(void);
void beetle_run_frame(uint16_t pad1_buttons);
int  beetle_get_framebuffer(uint32_t **out_pixels, unsigned *out_w, unsigned *out_h);

void beetle_debug_server_init(int port);
void beetle_debug_server_poll(void);
void beetle_debug_server_shutdown(void);
int  beetle_debug_server_get_input_override(void);
}

#define PAD_SELECT   (1 << 0)
#define PAD_L3       (1 << 1)
#define PAD_R3       (1 << 2)
#define PAD_START    (1 << 3)
#define PAD_UP       (1 << 4)
#define PAD_RIGHT    (1 << 5)
#define PAD_DOWN     (1 << 6)
#define PAD_LEFT     (1 << 7)
#define PAD_L2       (1 << 8)
#define PAD_R2       (1 << 9)
#define PAD_L1       (1 << 10)
#define PAD_R1       (1 << 11)
#define PAD_TRIANGLE (1 << 12)
#define PAD_CIRCLE   (1 << 13)
#define PAD_CROSS    (1 << 14)
#define PAD_SQUARE   (1 << 15)

static uint16_t pad_from_keyboard(void) {
    const Uint8* keys = SDL_GetKeyboardState(NULL);
    uint16_t b = 0xFFFF;
    if (keys[SDL_SCANCODE_UP])      b &= ~PAD_UP;
    if (keys[SDL_SCANCODE_DOWN])    b &= ~PAD_DOWN;
    if (keys[SDL_SCANCODE_LEFT])    b &= ~PAD_LEFT;
    if (keys[SDL_SCANCODE_RIGHT])   b &= ~PAD_RIGHT;
    if (keys[SDL_SCANCODE_RETURN])  b &= ~PAD_START;
    if (keys[SDL_SCANCODE_RSHIFT])  b &= ~PAD_SELECT;
    if (keys[SDL_SCANCODE_X])       b &= ~PAD_CROSS;
    if (keys[SDL_SCANCODE_Z])       b &= ~PAD_SQUARE;
    if (keys[SDL_SCANCODE_S])       b &= ~PAD_CIRCLE;
    if (keys[SDL_SCANCODE_A])       b &= ~PAD_TRIANGLE;
    if (keys[SDL_SCANCODE_Q])       b &= ~PAD_L1;
    if (keys[SDL_SCANCODE_W])       b &= ~PAD_R1;
    if (keys[SDL_SCANCODE_E])       b &= ~PAD_L2;
    if (keys[SDL_SCANCODE_R])       b &= ~PAD_R2;
    if (keys[SDL_SCANCODE_T])       b &= ~PAD_L3;   /* stick clicks: same keys */
    if (keys[SDL_SCANCODE_Y])       b &= ~PAD_R3;   /* as psx-runtime defaults */
    return b;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    const char* bios_path = "bios/SCPH1001.BIN";
    const char* disc_path = NULL;
    /* --wtrace-boot: arm wtrace on the CD command/param/ack regs + I_STAT/I_MASK
     * from frame 0, so the BIOS boot-EXE-load CD/IRQ activity is captured before
     * the oracle races past it into the game. Excludes the CD index reg 0x1F801800
     * on purpose: the BIOS status-poll writes it ~750x/frame and would flood the
     * ring, evicting the meaningful cmd/ack/IRQ-enable writes. */
    int wtrace_boot = 0;
    /* Default debug port 4382: 4380 collides with psxref and other recomp
     * projects' oracles (e.g. cdirecomp's CdiRuntime), and two LISTENers on
     * the same port silently route connections to the wrong process. Give
     * psx-beetle its own port; override with --port N. */
    int dbg_port = 4382;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--disc") && i + 1 < argc) {
            disc_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            dbg_port = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--wtrace-boot")) {
            wtrace_boot = 1;
        } else if (argv[i][0] != '-') {
            bios_path = argv[i];
        }
    }

    std::fprintf(stdout, "psx-beetle: loading BIOS from %s\n", bios_path);
    if (disc_path) {
        std::fprintf(stdout, "psx-beetle: loading disc from %s\n", disc_path);
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow(
        "psx-beetle — Beetle PSX",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        640, 480,
        SDL_WINDOW_SHOWN);
    if (!win) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    /* OpenGL renderer — see main.cpp note. Software renderer (GDI) hangs
     * the SDL main thread under heavy emulation load after the FMV-speed
     * fix raised cycle throughput. OpenGL avoids the GDI path. */
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) {
        std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Texture* tex = SDL_CreateTexture(
        ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 512);
    if (!tex) {
        std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return 1;
    }

    if (beetle_init_with_disc(bios_path, disc_path) != 0) {
        std::fprintf(stderr, "beetle_init failed\n");
        return 1;
    }

    if (wtrace_boot) {
        beetle_wtrace_arm(0x1F801801u, 0x1F801804u);  /* CD cmd/param/ack (not index 1800) */
        beetle_wtrace_arm(0x1F801070u, 0x1F801078u);  /* I_STAT / I_MASK */
        /* MMIO-READ trace from frame 0: capture what the IRQ handler READS from
         * the CD IRQ-flag reg (0x1F801803 idx1) and I_STAT during the boot-EXE
         * SeekL INT3 — the decisive verifier-vs-lowering signal. Include the
         * full CD reg window (0x1800-1803) for reads since the IRQ-flag read is
         * at index reg 0x1800/0x1803. */
        beetle_rtrace_arm(0x1F801800u, 0x1F801804u);  /* CD regs (incl. index/IRQ-flag) */
        beetle_rtrace_arm(0x1F801070u, 0x1F801078u);  /* I_STAT / I_MASK */
        std::fprintf(stdout, "psx-beetle: --wtrace-boot armed (wtrace CD cmd/ack + I_STAT; rtrace CD regs + I_STAT from frame 0)\n");
    }

#ifndef PSX_NO_DEBUG_TOOLS
    beetle_debug_server_init(dbg_port);
    std::fprintf(stdout, "psx-beetle: debug server on port %d\n", dbg_port);
#endif

    /* General control-flow parity trace (oracle producer). Armed from boot via
     * PSX_PARITY_TRACE=1 so the cutscene-transition window is covered. Default
     * trigger is the MMX6 setter func_8001344C (the behavior native is MISSING):
     * Beetle freezes its ring when thread1 reaches the setter, capturing the
     * correct lead-up to diff against native's parked-at-func_8002000C ring.
     * TCB / trigger / watch are env-tunable (same vars as psx-runtime). */
    if (const char* pt = std::getenv("PSX_PARITY_TRACE"); pt && pt[0] && pt[0] != '0') {
        auto envhex = [](const char* k, uint32_t dflt) -> uint32_t {
            const char* v = std::getenv(k);
            return (v && v[0]) ? (uint32_t)std::strtoul(v, nullptr, 0) : dflt;
        };
        uint32_t tcb     = envhex("PSX_PARITY_TCB",     0xA000E35Cu);
        uint32_t trigger = envhex("PSX_PARITY_TRIGGER", 0x8001344Cu); /* setter */
        uint32_t watch[PARITY_WATCH_MAX] = {
            0x801FEB78u, 0x8006D9ACu, 0x800CD3F8u,
            0x800CD3FCu, 0x800CD404u, 0x800200F4u,
        };
        parity_trace_config(tcb, trigger, 0x88u, 0x00u, watch, PARITY_WATCH_MAX);
        parity_trace_arm(1);
        std::fprintf(stdout, "psx-beetle: parity trace ARMED tcb=0x%08X trigger=0x%08X\n",
                     tcb, trigger);
    }

    /* Device-event cycle ring (oracle producer). Same gate as native: armed from
     * boot under PSX_PARITY_TRACE or PSX_DEVTRACE so every mednafen IRQ_Assert
     * rising edge is recorded with its guest cycle for the device-timing diff. */
    {
        const char* pt = std::getenv("PSX_PARITY_TRACE");
        const char* dt = std::getenv("PSX_DEVTRACE");
        if ((pt && pt[0] && pt[0] != '0') || (dt && dt[0] && dt[0] != '0')) {
            device_trace_arm(1);
            std::fprintf(stdout, "psx-beetle: device-event trace ARMED\n");
        }
    }

    static uint32_t pixels[640 * 512];

    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
#ifndef PSX_NO_DEBUG_TOOLS
                beetle_debug_server_shutdown();
#endif
                beetle_shutdown();
                SDL_DestroyTexture(tex);
                SDL_DestroyRenderer(ren);
                SDL_DestroyWindow(win);
                SDL_Quit();
                return 0;
            }
        }

#ifndef PSX_NO_DEBUG_TOOLS
        beetle_debug_server_poll();
        int override = beetle_debug_server_get_input_override();
#else
        int override = -1;
#endif
        uint16_t pad = (override >= 0) ? (uint16_t)override : pad_from_keyboard();

        beetle_run_frame(pad);

        uint32_t* fb = nullptr;
        unsigned w = 0, h = 0;
        if (beetle_get_framebuffer(&fb, &w, &h) && fb && w && h) {
            if (w > 640) w = 640;
            if (h > 512) h = 512;
            for (unsigned y = 0; y < h; y++) {
                for (unsigned x = 0; x < w; x++) {
                    pixels[y * w + x] = fb[y * w + x] | 0xFF000000u;
                }
            }
            /* Upload and present ONLY the valid w×h region. The texture is
             * allocated at the max PSX size (640×512), but the live frame
             * dimensions vary per-frame (256/320/368/512/640 wide, 240/480
             * tall). Passing NULL rects made SDL treat the source as the full
             * 640×512 texture while `pixels` is tightly packed at stride w:
             * each 640-texel texture row was filled from a w-stride buffer
             * (→ horizontal doubling / shear) and rows past the h*w packed
             * data drew uninitialized memory (→ garbage streaks below). A
             * per-frame src rect fixes both — the upload's pitch (w*4) now
             * matches the packed stride, and only the valid pixels are scaled
             * to the window. */
            SDL_Rect src = { 0, 0, (int)w, (int)h };
            SDL_UpdateTexture(tex, &src, pixels, (int)(w * sizeof(uint32_t)));
            SDL_Rect dst = { 0, 0, 640, 480 };
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, &src, &dst);
            SDL_RenderPresent(ren);
        }

        /* Wall-clock pacing to PSX-native 59.94 Hz for apples-to-apples
         * comparison with psx-runtime. Press TAB on the Beetle window to
         * sustain unlocked rate (turbo). */
        const Uint8* keys = SDL_GetKeyboardState(NULL);
        if (keys && keys[SDL_SCANCODE_TAB]) continue;
        constexpr double FRAME_MS = 1000.0 / 59.94;
        static FramePacer pacer = { 0 };
        frame_pacer_wait(&pacer, FRAME_MS);
    }
}
