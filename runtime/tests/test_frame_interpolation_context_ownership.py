#!/usr/bin/env python3
"""Pin temporal blending to the renderer's original thread and GL context."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RENDERER = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(
    encoding="utf-8"
)

# The only second context is the present thread's ([video] present_thread,
# docs/RENDER_THREAD.md), created in pt_begin under the render thread, which
# frame interpolation makes ineligible.
assert RENDERER.count("SDL_GL_CreateContext(") == 2, (
    "temporal blending must not create a second GL context"
)
_pt = RENDERER[RENDERER.index("static int pt_begin(void)"):]
_pt = _pt[:_pt.index("\n}\n")]
assert "SDL_GL_CreateContext(" in _pt, "the second GL context belongs to the present thread"
assert "SDL_CreateThread(" not in RENDERER, (
    "temporal blending must not create a presentation thread"
)
assert "SDL_LockMutex(" not in RENDERER, (
    "the GL renderer must not hold an application mutex across driver work"
)
assert RENDERER.count("SDL_GL_SwapWindow(") == 1, (
    "all OpenGL presentation must converge on one owned swap call"
)
assert "gl_renderer_interpolation_owns_cadence" in RENDERER
assert "no motion vectors" in RENDERER

print("single-context temporal blending ownership guard passed")
