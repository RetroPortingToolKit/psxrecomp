#!/usr/bin/env python3
"""Guard PSX display enhancements as trusted-mod-only features."""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "runtime" / "include" / "mod_plugins.h").read_text(
    encoding="utf-8"
)

for declaration in (
    "constexpr bool ws_offered = false;",
    "constexpr bool ws_ultrawide_offered = false;",
    "constexpr bool frame_interpolation_offered = false;",
    "constexpr bool skip_fmv_offered = false;",
):
    assert declaration in MAIN, f"PSX launcher capability must default off: {declaration}"

for legacy_route in (
    "ws_offered = gc.ws_offered;",
    "ws_ultrawide_offered = gc.ws_ultrawide_offered;",
    "frame_interpolation_offered =\n                gc.runtime.video_offer_frame_interpolation;",
    "skip_fmv_offered = gc.runtime.video_offer_skip_fmv;",
):
    assert legacy_route not in MAIN, f"legacy offer flag still controls UI: {legacy_route}"

for hidden_capability in (
    "gi->widescreen_supported = 0;",
    "gi->aspect_mask = 0;",
):
    assert hidden_capability in MAIN

for trusted_api in (
    "psx_mod_set_fixed_display_aspect",
    "psx_mod_set_adaptive_display_aspect",
    "psx_mod_set_frame_interpolation",
    "psx_mod_set_auto_skip_fmv",
):
    assert trusted_api in HEADER, f"missing trusted mod API: {trusted_api}"

assert MAIN.index("g_auto_skip_fmv = 0;") < MAIN.index("mod_runtime_activate_plugins();")

# Session hygiene: mod-owned presentation state must not outlive the session
# that set it. The one in-process second session is the lobby rematch after a
# netplay match; it jumps past the first-boot block, so both paths call the
# same session start, which resets before activating. The reachable leak is the netplay local viewport's Fit/aspect into an
# offline rematch; the rest of the list is defensive. The first-call capture
# itself is exercised by mod_session_baseline_test (behavioural), which cannot
# see main.cpp; this guard pins main.cpp's glue to that helper (the baseline it
# passes, the native-pacing flag as the helper reads it, the copy in and out)
# and to the call sites.
HELPER_SIG = "static void reset_mod_owned_presentation(void) {"
helper_start = MAIN.index(HELPER_SIG)
helper = MAIN[helper_start:MAIN.index("\n}\n", helper_start)]
assert '#include "mod_session_baseline.h"' in MAIN


def c_statements(block):
    """Comment-free, whitespace-normalised statements of straight-line C."""
    block = re.sub(r"/\*.*?\*/", "", block, flags=re.S)
    block = re.sub(r"//[^\n]*", "", block)
    return [" ".join(part.split()) + ";" for part in block.split(";") if part.strip()]


# The baseline must outlive a session: one file-scope static that only the
# helper's call touches. A local or re-zeroed baseline makes every call a first
# call, so a rematch would restore nothing and skip the later-call resets.
assert re.findall(r"^static PSXModSessionBaseline g_mod_owned_baseline;$",
                  MAIN, re.M) == ["static PSXModSessionBaseline g_mod_owned_baseline;"], \
    "the session baseline must be one file-scope static"
assert MAIN.index("static PSXModSessionBaseline g_mod_owned_baseline;") < helper_start
assert MAIN.count("PSXModSessionBaseline") == 1, \
    "no other session baseline may exist (every call would be a first call)"
assert MAIN.count("g_mod_owned_baseline") == 2, \
    "only reset_mod_owned_presentation() may use the session baseline"

# The call: the persistent baseline, and the native-pacing flag as it stands
# (a plugin's psx_mod_set_native_vblank_rate owns the frame periods only then).
CALL = "const int first = psx_mod_session_baseline_apply("
assert helper.count("psx_mod_session_baseline_apply(") == 1, \
    "reset must go through the tested first-call capture helper, once"
call_at = helper.index(CALL)
call_end = helper.index(";", call_at) + 1
assert "".join(helper[call_at:call_end].split()) == (
    "constintfirst=psx_mod_session_baseline_apply("
    "&g_mod_owned_baseline,&live,g_mod_native_vblank_rate?1:0);"
), "the helper must get the persistent baseline and the live native-pacing flag"

# Before the call: clear the session's local mouse accumulator and copy in
# the scalars. Neither may change the native-pacing state the helper captures.
COPIES = (
    ("video_vsync", "g_video_vsync"),
    ("frame_interpolation", "g_frame_interpolation"),
    ("frame_interpolation_fps", "g_frame_interpolation_fps"),
    ("auto_skip_fmv", "g_auto_skip_fmv"),
    ("guest_frame_period_ms", "g_guest_frame_period_ms"),
    ("frame_period_ms", "g_frame_period_ms"),
)
before = c_statements(helper[len(HELPER_SIG):call_at])
expected = ["psx_local_mouse_clear();", "PSXModSessionScalars live;"] + [
    f"live.{f} = {g};" for f, g in COPIES
]
assert sorted(before) == sorted(expected), \
    f"only mouse reset and scalar copy-in may precede the baseline call, got {before}"

# After the call: the copy-out, each once, then the native-pacing flag cleared.
after = helper[call_end:]
for field, name in COPIES:
    assert helper.count(f"{name} = live.{field};") == 1 and \
        f"{name} = live.{field};" in after, f"copy-out missing: {name}"
assert helper.count("g_mod_native_vblank_rate") == 2 and \
    "g_mod_native_vblank_rate = false;" in after, \
    "native pacing must be read by the call and cleared only after it"

# Every call: back to the initial value (checked against the file-scope
# initialiser where it is a literal).
for reset in (
    "g_mod_native_vblank_rate = false;",
    "g_mod_native_vblank_fps = 0;",
    "g_ws_adaptive_view = false;",
    "g_ws_adaptive_max_num = 16;",
    "g_ws_adaptive_max_den = 9;",
    "psx_mod_set_world_scene_predicate(nullptr);",
    "psx_mod_set_retained_scene_predicate(nullptr);",
    "psx_mod_set_adaptive_backdrop_preload(0);",
    "(void)psx_mod_set_draw_distance_clamp(0);",
    "g_bezel_path.clear();",
    "g_frame_interpolation_blend = g_frame_interpolation_blend_default;",
    "g_frame_interpolation_source = PSX_MOD_FRAME_SOURCE_VBLANK;",
    "render_pass_reset_session();",
):
    assert reset in after, f"mod-owned session reset is missing: {reset}"
    literal = re.fullmatch(r"(g_\w+) = (false|true|\d+);", reset)
    if literal:
        name, value = literal.groups()
        assert re.search(rf"^static\s+\w+\s+{name} = {value};", MAIN, re.M), \
            f"{reset} must match the file-scope initialiser"

# Later calls only (the first call changes nothing that is not already at its
# initial value): the 8 MiB RAM request, which memory_init() re-latches at
# session_reboot, and the texture-bank resolver/batching flag.
later = helper[helper.index("if (!first) {"):]
later = later[:later.index("}")]
for reset in (
    "psx_ram_reset_size_request();",
    "psx_mod_set_texture_bank_resolver(nullptr);",
    "psx_mod_set_texture_bank_batching(0);",
):
    assert reset in later, f"later-session reset is missing: {reset}"

# Bezel artwork: clearing g_bezel_path only stops the next load. The loaded
# texture must not outlive the session's GL context either, or a rematch would
# bind the stale name in its new context (present_bezel() draws whenever it is
# nonzero). Only a soft return and process exit shut the renderer down.
GL = (ROOT / "runtime" / "src" / "gpu_gl_renderer.c").read_text(encoding="utf-8")
shutdown = GL[GL.index("void gl_renderer_shutdown(void) {"):]
shutdown = shutdown[:shutdown.index("\n}\n")]
live_ctx = shutdown[shutdown.index("if (s_ctx) {"):shutdown.index("SDL_GL_DeleteContext(s_ctx);")]
assert "gl_renderer_set_bezel(NULL, 0, 0);" in live_ctx, \
    "gl_renderer_shutdown() must drop the bezel texture while its context is current"
set_bezel = GL[GL.index("int gl_renderer_set_bezel(const void *rgba, int w, int h) {"):]
assert set_bezel.index("if (s_bezel_tex) { glDeleteTextures(1, &s_bezel_tex); s_bezel_tex = 0; }") < \
    set_bezel.index("if (!rgba || w <= 0 || h <= 0) return 1;"), \
    "gl_renderer_set_bezel(NULL, ...) must delete and forget the texture"
assert "if (!s_bezel_tex || ww <= 0 || wh <= 0) return;" in GL

# Session start: one sequence for every session, after its commit or netplay
# clear. It clears the controller, load and disc-speed choices, resets
# mod-owned presentation state, activates, then applies what activation chose.
session_start = MAIN.index("auto start_mod_session = [&](bool netplay) {")
session = MAIN[session_start:MAIN.index("\n    };\n", session_start)]
order = [session.index(step) for step in (
    "g_mod_controller_mode_override.fill(-1);",
    "policy = ModControllerPresentationPolicy{};",
    "g_mod_load_wall_multiplier = -1;",
    "g_mod_disc_speed_divisor = -1;",
    "g_mod_disc_instant_rate = -1;",
    "g_turbo_load_wall_multiplier = 0;",
    "reset_mod_owned_presentation();\n        mod_runtime_activate_plugins();",
    "apply_netplay_local_viewport_aspect(netplay);",
    "player_mode[i] = g_mod_controller_mode_override[i];",
    "g_turbo_load_wall_multiplier = g_mod_load_wall_multiplier;",
)]
assert order == sorted(order), \
    "session start must reset, then activate, then apply what activation chose"
# Nothing activates or resets outside that sequence.
assert MAIN.count("mod_runtime_activate_plugins();") == 1, \
    "plugins must activate only in start_mod_session"
assert MAIN.count("reset_mod_owned_presentation();") == 1, \
    "the mod-owned reset must run only in start_mod_session"

# First boot: commit (or netplay clear), then the session start, then the
# disc the plan selects, all above session_reboot.
reboot = MAIN.index("\nsession_reboot:\n")
first_commit = MAIN.index("mod_runtime_commit(resolved_disc, &mod_error)")
first_start = MAIN.index("start_mod_session(net_cfg.enabled);")
first_disc = MAIN.index("std::string disc_path_str = session_disc_path(resolved_disc);")
assert first_commit < session_start < first_start < first_disc < reboot, \
    "first boot must commit, start the session, then mount the plan's disc"

# Soft return (rematch) re-enters below that block via `goto session_reboot`,
# so the rematch path runs the same session start after its commit / netplay
# clear, and mounts the disc the new plan selects.
rematch_start = MAIN.index('"psxrecomp: cannot apply netplay mods "',
                           MAIN.index("soft_return_lobby:"))
rematch = MAIN[rematch_start:]
rematch = rematch[:rematch.index("goto session_reboot;")]
commit = rematch.index("mod_runtime_commit(resolved_disc,")
rematch_session = rematch.index("start_mod_session(net_cfg.enabled);")
rematch_disc = rematch.index("disc_path_str = session_disc_path(resolved_disc);")
assert commit < rematch_session < rematch_disc, \
    "rematch must commit, start the session, then mount the plan's disc"
assert MAIN.count("start_mod_session(net_cfg.enabled);") == 2, \
    "exactly the first boot and the rematch start a session"

# Rematch 4:3 re-clamp: the launcher round-trips the previous match's aspect
# (16:9/21:9 from the netplay local viewport) through ls.aspect_index. While
# widescreen is mod-owned the Settings aspect is 4:3, so the rematch path
# re-applies that clamp after the launcher, before the session start, where
# a plugin's activation or the netplay local viewport can still replace it.
clamp = "if (!ws_offered) {\n                g_video_aspect_num = 4;\n                g_video_aspect_den = 3;\n            }"
assert rematch.count(clamp) == 1, "rematch must re-clamp the aspect to 4:3"
clamp_at = rematch.index(clamp)
assert commit < clamp_at < rematch_session, \
    "4:3 re-clamp must follow the commit and precede the session start"
assert MAIN.rfind("case 1:  g_video_aspect_num = 16; g_video_aspect_den = 9; break;",
                  0, rematch_start) > MAIN.index("soft_return_lobby:"), \
    "the launcher's aspect must be applied before the rematch re-clamp"

print("mod-owned PSX display controls guard passed")
