#!/usr/bin/env python3
"""Guard how the setup install's card folder is WIRED into main().

A setup install keeps settings.toml in <root>/build-release and its memory
cards in <root>/saves. The launcher wrote the card folder by full path, so a
moved or copied install kept using the old folder: an empty card for the
player, or the other copy's saves.

recompiler/tests/portable_settings_paths_test.cpp proves the rule itself:
with the install's root known, save_user_settings() writes "../saves", and a
moved copy resolves that to its own folder. This proves main() uses the rule:

  1. it gives the loader's project root to the settings writer, and
  2. every [memcard] value read from settings.toml goes through the helper
     that anchors a relative value on the exe folder and folds the climb.

Reading source rather than running a binary is deliberate: the code is in the
middle of main(), between the settings load and a launcher round-trip, and a
headless start never saves settings.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8")
LOADER = (ROOT / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")

# ---- 1. the writer is told the install's root, from the loaded game config --
assert "PSXRecompV4::set_user_settings_install_root(gc.project_root);" in MAIN, (
    "main() no longer gives the project root to the settings writer: a setup "
    "install would store its card folder by full path again"
)

# ---- 2. the three card keys are written through the install-root rule -------
for key in ("s.memcard_dir", "s.memcard1_path", "s.memcard2_path"):
    assert f"rel_card({key})" in LOADER, f"{key} is not written through rel_card()"
for key in ("s.bios_path", "s.disc_path"):
    assert f"rel({key})" in LOADER, (
        f"{key} must keep the plain rule: a disc or BIOS file is the player's "
        "own file, not part of the install"
    )

# ---- 3. every [memcard] value from settings.toml is anchored and folded -----
helper = re.search(
    r"static std::filesystem::path anchor_card_path\(.*?\n}\n", MAIN, re.S)
assert helper, "anchor_card_path() is gone from main.cpp"
assert "lexically_normal()" in helper.group(0), (
    "anchor_card_path() must fold the climb of \"../saves\""
)
assert "host_path_is_absolute(p)" in helper.group(0), (
    "anchor_card_path() must return an absolute value unchanged"
)
for name in ("us.memcard_dir", "us.memcard1_path", "us.memcard2_path",
             "seed.memcard1_path", "seed.memcard2_path"):
    assert f"anchor_card_path(argv[0], {name})" in MAIN, (
        f"{name} is read without anchor_card_path()"
    )
    assert f"anchor_on_exe_dir(argv[0], {name})" not in MAIN, (
        f"{name} is still read through the plain anchor at one site"
    )

print("setup install card path wiring: PASS")
