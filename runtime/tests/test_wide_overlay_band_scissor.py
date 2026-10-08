#!/usr/bin/env python3
"""The full-screen-overlay native-wide pass must clip to the draw-area rows.

Vertically double-buffered titles keep both display bands in ONE wide surface.
A full-width rect that extends above its draw area is clipped at the band top
in canonical VRAM; the wide pass has to clip it there too (the software
reference rt_wide() does), or it paints the other band's rows and the next
present shows a flat band over that buffer. The GL pixel check lives in
test_gl_readback_region.c; this guard covers both GPU backends' source.
"""
from pathlib import Path
import re

src = Path(__file__).parents[1] / "src"


def body(text, signature):
    m = re.search(re.escape(signature) + r".*?\n\}", text, flags=re.DOTALL)
    assert m, f"{signature} not found"
    return m.group(0)


gl = (src / "gpu_gl_renderer.c").read_text(encoding="utf-8")
band = body(gl, "static void wide_band_scissor_x(int x, int w) {")
assert "static void wide_band_scissor(void) { wide_band_scissor_x(0, g_wide_w); }" in gl, "GL band scissor does not cover the full wide width"
if "wide_band_rows(" in band:   # the rows are shared with the stencil rebuild
    band = body(gl, "static void wide_band_rows(")
assert "s_area_y1" in band and "s_area_y2" in band, "GL band scissor ignores the draw area"
flat = body(gl, "static void wide_flat_rect_direct(")
assert "wide_band_scissor();" in flat, "GL overlay pass does not use the band scissor"
assert "VRAM_H * s_scale);  /* full surface" not in flat, "GL overlay pass uses a full-height scissor"
target = body(gl, "static void wide_target_begin(")
assert "wide_band_scissor_x(" in target, "GL wide mirror does not use the band scissor"

vk = (src / "gpu_vk_renderer.c").read_text(encoding="utf-8")
begin = body(vk, "static void wide_pass_begin(VkCommandBuffer cb) {")
assert "s_da_y1" in begin and "s_da_y2" in begin, "Vulkan wide pass ignores the draw area"
overlay = body(vk, "static void wide_overlay_rect(")
assert "wide_pass_begin(cb);" in overlay, "Vulkan overlay pass does not open a wide pass"
assert "vkCmdSetScissor" not in overlay, "Vulkan overlay pass overrides the band scissor"

print("wide overlay band scissor test passed")
