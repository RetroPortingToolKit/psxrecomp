#!/usr/bin/env python3
"""The OpenGL swap shares the SDL_Renderer present's vsync self-heal.

After three swaps that block for more than 250 ms with vsync armed, the
present path turns driver vsync off for the session. The SDL_Renderer present
in main.cpp does this; every OpenGL swap goes through gl_window_swap, which
must do the same and keep vsync off afterwards.
"""

from pathlib import Path
import argparse
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
GL = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(encoding="utf-8")


def body(start_marker: str) -> str:
    start = GL.index(start_marker)
    return GL[start:GL.index("\n}\n", start)]



def check_publication(cc: str) -> None:
    start = GL.index("static _Atomic uint32_t s_present_slow_count;")
    state = GL[start:GL.index("\nvoid gl_renderer_set_swap_interval", start)]
    heal = body("static void gl_swap_self_heal(") + "\n}\n"
    prelude = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include "video_telemetry.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
typedef uint64_t Uint64;
static Uint64 now;
static int s_swap_interval = 1, set_calls, set_result;
Uint64 SDL_GetPerformanceCounter(void) { return now; }
Uint64 SDL_GetPerformanceFrequency(void) { return 1000; }
int SDL_GL_SetSwapInterval(int interval) {
    assert(interval == 0); ++set_calls; return set_result;
}
"""
    checks = r"""
static void record_slow(void) {
    for (unsigned i = 0; i < 10000; ++i) psx_present_note_slow();
    psx_present_disable_vsync();
}
#ifdef _WIN32
static DWORD WINAPI worker(void *unused) { (void)unused; record_slow(); return 0; }
#else
static void *worker(void *unused) { (void)unused; record_slow(); return NULL; }
#endif
int main(void) {
    now = 250; gl_swap_self_heal(0);
    assert(psx_present_slow_count() == 0 && set_calls == 0);
    now = 251; gl_swap_self_heal(0); gl_swap_self_heal(0);
    assert(psx_present_slow_count() == 2 && !psx_present_vsync_disabled());
    set_result = -1; gl_swap_self_heal(0);
    assert(set_calls == 1 && !psx_present_vsync_disabled());
    set_result = 0; gl_swap_self_heal(0);
    assert(set_calls == 2 && psx_present_vsync_disabled());
    gl_swap_self_heal(0);
    assert(psx_present_slow_count() == 4 && set_calls == 2);
    /* Only the fixture resets session state between independent cases. */
    atomic_store(&s_present_slow_count, 0);
    atomic_store(&s_present_vsync_disabled, 0);
#ifdef _WIN32
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    assert(thread);
#else
    pthread_t thread;
    assert(pthread_create(&thread, NULL, worker, NULL) == 0);
#endif
    uint32_t previous = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        uint32_t count = psx_present_slow_count();
        assert(count >= previous && count <= 10000);
        previous = count;
        (void)psx_present_vsync_disabled();
    }
#ifdef _WIN32
    assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    assert(pthread_join(thread, NULL) == 0);
#endif
    assert(psx_present_slow_count() == 10000 && psx_present_vsync_disabled());
    puts("vsync self-heal: threshold, failure, sticky state and concurrent publication PASS");
}
"""
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        source, exe = folder / "vsync.c", folder / "vsync.exe"
        source.write_bytes((prelude + state + heal + checks).encode())
        if Path(cc).name.lower() in ("cl", "cl.exe", "clang-cl", "clang-cl.exe"):
            command = [cc, "/nologo", "/std:c11", "/I" + str(ROOT / "runtime/include"),
                       str(source), "/Fe:" + str(exe)]
        else:
            command = [cc, "-std=c11", "-I", str(ROOT / "runtime/include"),
                       str(source), "-o", str(exe)]
            if sys.platform != "win32":
                command.append("-pthread")
        subprocess.run(command, cwd=folder, check=True)
        subprocess.run([str(exe)], check=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True)
    args = parser.parse_args()
    failures = []
    swap = body("static void gl_window_swap(void) {")
    if "SDL_GL_SwapWindow(s_win)" not in swap:
        failures.append("gl_window_swap no longer swaps")
    if GL.count("SDL_GL_SwapWindow(") != swap.count("SDL_GL_SwapWindow("):
        failures.append("an OpenGL swap bypasses gl_window_swap")
    if "SDL_GetPerformanceCounter()" not in swap.split("SDL_GL_SwapWindow(")[0]:
        failures.append("gl_window_swap does not time the swap")
    if "gl_swap_self_heal(" not in swap:
        failures.append("gl_window_swap does not apply the self-heal")
    else:
        heal = body("static void gl_swap_self_heal(")
        for fragment in ("swap_ms <= 250", "s_swap_interval == 0", "psx_present_note_slow() >= 3",
                         "SDL_GL_SetSwapInterval(0)", "psx_present_disable_vsync();"):
            if fragment not in heal:
                failures.append(f"self-heal is missing: {fragment}")
    setter = body("void gl_renderer_set_swap_interval(int interval) {")
    if "if (psx_present_vsync_disabled()) interval = 0;" not in setter:
        failures.append("the swap-interval setter can turn vsync back on after the self-heal")
    rebind = body("static void *pt_cb_present(")
    if "psx_present_vsync_disabled() ? 0 : s_swap_interval" not in rebind:
        failures.append("the present thread re-applies vsync after the self-heal")
    for f in failures:
        print("FAIL:", f)
    if failures:
        return 1
    check_publication(args.cc)
    print("gl swap self-heal: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
