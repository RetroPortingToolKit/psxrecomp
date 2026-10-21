#!/usr/bin/env python3
"""Present skips retain a clock and honor explicit consecutive-frame capture.

The frontend's deadline pacer now runs alongside driver vsync. The renderer
must preserve swaps only when that independent cap is absent, and must still
compose every frame for a requested diagnostic burst.
"""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
RENDERER = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(
    encoding="utf-8"
)
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
PACING = (ROOT / "runtime" / "include" / "frame_pacing.h").read_text(
    encoding="utf-8"
)

# The skip sites are the branches that increment the probe skip counter and
# return without reaching SDL_GL_SwapWindow. Both consult the OSD, the
# interpolation-cadence owner, and the vsync-cadence owner.
skips = RENDERER.count("s_probe_skip++;")
assert skips >= 2, "expected the VRAM and wide-FBO present-skip sites"
assert RENDERER.count("!psx_present_vsync_owns_cadence() &&") == skips, (
    "every present-skip site must be gated on psx_present_vsync_owns_cadence(); "
    "an ungated skip lets the guest free-run when driver vsync owns cadence"
)
# The OSD query goes through ov_needs_present(), which is host_osd_needs_present()
# except when the render thread replays a present recorded with its own copy
# of the overlay state (docs/RENDER_THREAD.md).
assert RENDERER.count("!ov_needs_present() &&") == skips
assert ": host_osd_needs_present();" in RENDERER
assert RENDERER.count("!gl_renderer_interpolation_owns_cadence()) {") == skips

# The query is declared beside the independent pacer, and defined
# in the frontend that owns the cadence decision.
assert "int psx_present_vsync_owns_cadence(void);" in PACING
assert 'extern "C" int psx_present_vsync_owns_cadence(void) {' in MAIN
assert "return present_vsync_owns_cadence() && !present_should_wall_pace();" in MAIN
assert RENDERER.count("!present_burst_count() && !s_native_surface_enabled && s_force_present_remaining <= 0 &&") == skips

print("present-skip cadence guard passed (%d skip sites gated)" % skips)
