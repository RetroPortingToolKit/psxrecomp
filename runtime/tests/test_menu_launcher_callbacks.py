"""The in-game menu's launcher-Settings callbacks run on a provider's draw
thread while the game runs: they must not write runtime state (psxrecomp#674
review P1).

settings_get reads only the main-thread snapshot (mh_video_get), and the
menu's game info reads no live video globals. Neither offers the internal
resolution: the built-in Internal Resolution mod owns it (its option changes
live through the Mods page), so the Settings view never draws that row."""
import re
from pathlib import Path
from test_netplay_load_apply_flow import function_body, require

source = (Path(__file__).resolve().parents[1] / "src/main.cpp").read_text(encoding="utf-8")
WRITE = re.compile(r"\bg_\w+\s*(?:\.\w+\s*)?(?:=(?!=)|\+\+|--|\+=|-=)")
STATEFUL = ("psx_video_current", "psx_sdl_display_pixel_height",
            "session_host_look", "session_internal_resolution", "psx_host_enh_")

for sig in ("void mh_launcher_settings_get(RecompLauncherCSettings* out)",
            "void mh_launcher_game_info(RecompLauncherCGameInfo* out)"):
    body = function_body(source, sig)
    m = WRITE.search(body)
    require(m is None, f"{sig}: writes runtime state ({m.group(0) if m else ''})")
    for name in STATEFUL:
        require(name not in body, f"{sig}: calls {name}")

gi = function_body(source, "void mh_launcher_game_info(RecompLauncherCGameInfo* out)")
require("internal_resolution" not in gi,
        "menu game info offers no Internal resolution row (the mod owns it)")
require("psx_launcher_hide_mod_owned_rows(&gi);" in gi,
        "menu game info clears the mod-owned rows, Internal resolution included")
ls_get = function_body(source, "void mh_launcher_settings_get(RecompLauncherCSettings* out)")
require("internal_resolution" not in ls_get and "supersample" not in ls_get,
        "settings_get hands the Settings view no internal resolution")
print("menu launcher callbacks: draw-thread safe PASS")
