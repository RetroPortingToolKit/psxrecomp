#!/usr/bin/env python3
"""The OpenGL swap shares the SDL_Renderer present's vsync self-heal.

After three swaps that block for more than 250 ms with vsync armed, the
present path turns driver vsync off for the session. The SDL_Renderer present
in main.cpp does this; every OpenGL swap goes through gl_window_swap, which
must do the same and keep vsync off afterwards.
"""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
GL = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(encoding="utf-8")


def body(start_marker: str) -> str:
    start = GL.index(start_marker)
    return GL[start:GL.index("\n}\n", start)]


def main() -> int:
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
        for fragment in ("swap_ms <= 250", "s_swap_interval == 0", "g_present_slow_count >= 3",
                         "SDL_GL_SetSwapInterval(0)", "g_present_vsync_disabled = 1;"):
            if fragment not in heal:
                failures.append(f"self-heal is missing: {fragment}")
    setter = body("void gl_renderer_set_swap_interval(int interval) {")
    if "if (g_present_vsync_disabled) interval = 0;" not in setter:
        failures.append("the swap-interval setter can turn vsync back on after the self-heal")
    rebind = body("static void *pt_cb_present(")
    if "g_present_vsync_disabled ? 0 : s_swap_interval" not in rebind:
        failures.append("the present thread re-applies vsync after the self-heal")
    for f in failures:
        print("FAIL:", f)
    if failures:
        return 1
    print("gl swap self-heal: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
