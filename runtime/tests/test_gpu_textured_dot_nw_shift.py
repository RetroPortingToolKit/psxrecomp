from pathlib import Path


GPU = Path(__file__).resolve().parents[1] / "src" / "gpu.c"
SOURCE = GPU.read_text(encoding="utf-8")

HELPER_START = SOURCE.index("static void gp0_exec_textured_dot(void) {")
HELPER_END = SOURCE.index("/* Execute 8x8 textured sprite", HELPER_START)
HELPER = SOURCE[HELPER_START:HELPER_END]

SWITCH_START = SOURCE.rindex("case 0x6C: case 0x6D: case 0x6E: case 0x6F:")
SWITCH_END = SOURCE.index("case 0x70: case 0x71: case 0x72: case 0x73:", SWITCH_START)
SWITCH_BLOCK = SOURCE[SWITCH_START:SWITCH_END]

shift = "x0 += ws_nw_hud_shift(x0, 1);"
offset = "x0 += draw_offset_x; y0 += draw_offset_y;"

assert "x0 = ws_nw_hud_shift" not in HELPER
assert shift in HELPER
assert offset in HELPER
assert HELPER.index(shift) < HELPER.index(offset)
assert "gp0_exec_textured_dot();" in SWITCH_BLOCK
