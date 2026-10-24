#!/usr/bin/env python3
"""Guard the FLIP frame-interpolation source's wiring (docs/FRAME_RATE.md).

The pure pieces (frame_flip_is_new_frame, frame_flip_tracker_vblank,
begin_phase) are exercised by frame_interpolation_schedule_test. This guard
pins where they are wired in, which no unit test reaches:

- main.cpp hands the session's source to the GL renderer at every session
  start, right after the renderer's interpolation settings;
- interp_capture's FLIP branch asks frame_flip_is_new_frame with the display
  origin and the redraw test, feeds the answer to the tracker, and returns
  before the copy on a duplicate, so the history is not rotated;
- both present paths pass the display origin and the redraw test.

Without the hand-off FLIP never reaches the renderer: frame blend stays per
VBlank, and render passes (which require the FLIP source) are always refused.
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
GL = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(encoding="utf-8")


def squash(text):
    return re.sub(r"\s+", " ", text).strip()


def function_body(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start)]


# ---- session-start hand-off (main.cpp) --------------------------------------
HANDOFF = "gl_renderer_set_interpolation_source(g_frame_interpolation_source);"
assert MAIN.count(HANDOFF) == 1, "the source hand-off must exist exactly once"
handoff_at = MAIN.index(HANDOFF)
reboot_at = MAIN.index("session_reboot:")
assert reboot_at < handoff_at, \
    "the hand-off must run at every session start (after session_reboot:)"
settings_at = MAIN.rindex("gl_renderer_set_interpolation(", 0, handoff_at)
assert reboot_at < settings_at, "hand-off must sit beside the session's GL setup"
between = MAIN[settings_at:handoff_at]
assert between.count(";") == 1 and between.rstrip().endswith(
    "g_frame_interpolation_blend);"), \
    "the hand-off must directly follow the session's interpolation settings"

setter = function_body(GL, "void gl_renderer_set_interpolation_source(int source) {")
assert "if (source != s_interp_source) interp_reset_history_unlocked();" in setter, \
    "a source change must restart the blend history"
assert "s_interp_source = source;" in setter

# ---- interp_capture's FLIP branch (gpu_gl_renderer.c) -----------------------
wrapper = function_body(GL, "static int interp_capture(GLuint fbo,")
assert "interp_capture_timed_impl(" in wrapper, "timed wrapper must invoke the capture implementation"
capture = function_body(GL, "static int interp_capture_timed_impl(GLuint fbo,")
flip_at = capture.index("if (s_interp_source == 1) {")
flip = capture[flip_at:capture.index("} else {", flip_at)]
flat = squash(flip)
assert squash("""int new_frame = frame_flip_is_new_frame(
    geometry_changed, s_interp_valid == 0, redrawn, origin_x, origin_y,
    s_interp_origin_x, s_interp_origin_y);""") in flat, \
    "FLIP must decide new frames with the tested helper, from origin and redraw"
assert "s_interp_origin_x = origin_x;" in flip and \
    "s_interp_origin_y = origin_y;" in flip, "the last origin must be kept"
assert squash("""frame_flip_tracker_vblank(&s_interp_flip, new_frame,
    &s_interp_phase_lo, &s_interp_phase_hi);""") in flat, \
    "the tracker must see every presented VBlank with the decision"
assert squash("""if (!new_frame) { s_interp_duplicates++; return 1; }""") in flat, \
    "a duplicate must return before the history is rotated"
assert flip.index("frame_flip_is_new_frame(") < \
    flip.index("frame_flip_tracker_vblank(") < flip.index("if (!new_frame) {")
assert flip_at < capture.index("glCopyTexSubImage2D("), \
    "the duplicate return must precede the capture copy"

# ---- present paths ----------------------------------------------------------
calls = [squash(m.group(0)) for m in
         re.finditer(r"interp_capture\(\s*[^;]*?\);", GL[GL.index("\n}\n",
                     GL.index("static int interp_capture(GLuint fbo,")):])]
assert len(calls) == 2, f"expected the VRAM and wide present calls, got {len(calls)}"
assert any("GL_PRES_VRAM, disp_x, disp_y, present_dirty_test(disp_x, disp_y,"
           in c for c in calls), "VRAM present must pass origin and redraw"
assert any("GL_PRES_WIDE, disp_x, disp_y, present_dirty_test(0, disp_y,"
           in c for c in calls), "wide present must pass origin and redraw"

print("frame interpolation FLIP source wiring guard passed")
