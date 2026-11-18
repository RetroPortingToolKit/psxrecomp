#!/usr/bin/env python3
"""Guard how bios_hle and fast_boot pass between settings.toml and main().

settings.toml overrides game.toml. The launcher has no control for bios_hle or
fast_boot, yet main() marked both present in the settings it hands to the
launcher, so every save wrote them with whatever value was in force. That line
then held the value for good: a later change of bios_hle in the game's
game.toml never reached a player who had started an older build once, and two
netplay peers of one build could boot in different BIOS modes. It is the same
latch as turbo_loads.

recompiler/tests/boot_keys_settings_test.cpp proves the file side: a file from
before settings_format 2 has its two keys dropped, a format 2 file keeps them.
This proves main() does its part:

  1. it applies the keys only when the loader says the file set them, and
     remembers that;
  2. it marks them present for the launcher's save only in that case;
  3. it says once that an old file's lines were ignored.

Reading source rather than running a binary is deliberate: the code is in the
middle of main(), around a launcher round-trip, and no headless start saves
settings.
"""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
# Column alignment in main.cpp is not part of the contract: one space everywhere.
MAIN = re.sub(r"[ \t]+", " ", (ROOT / "runtime" / "src" / "main.cpp").read_text(encoding="utf-8"))
LOADER = (ROOT / "recompiler" / "src" / "config_loader.cpp").read_text(encoding="utf-8")

# ---- 1. applied only when the file set them, and remembered -----------------
for key in ("fast_boot", "bios_hle"):
    assert len(re.findall(rf"settings_chose_{key} = [^=]", MAIN)) == 2, (
        f"settings_chose_{key} must be set at its declaration and at the "
        "settings.toml read only"
    )
    applied = MAIN.find(f"if (us.has_{key}) {key} = us.{key};")
    assert applied >= 0, f"{key} from settings.toml is no longer applied"
    remembered = MAIN.find(f"settings_chose_{key} = us.has_{key};", applied)
    assert 0 <= remembered - applied < 300, (
        f"{key} is not remembered beside the settings.toml read"
    )

# ---- 2. marked present for the launcher's save only in that case ------------
for key in ("fast_boot", "bios_hle"):
    assert f"seed.has_{key} = settings_chose_{key};" in MAIN, (
        f"the launcher's settings mark {key} present without the player's line"
    )
    assert f"seed.has_{key} = true" not in MAIN, (
        f"`seed.has_{key} = true` is back: every launcher save would write "
        "the key and hold it over game.toml"
    )

# ---- 3. the old file's lines are reported, once -----------------------------
assert MAIN.count("if (us.boot_keys_were_echoes)") == 1, (
    "main() must say once that an old file's bios_hle/fast_boot were ignored"
)

# ---- the file side this depends on -------------------------------------------
assert "s.settings_format < 2 && (s.has_fast_boot || s.has_bios_hle)" in LOADER, (
    "the loader no longer drops the echoed keys of a file older than format 2"
)
assert 'f << "settings_format = " << UserSettings::kFormat' in LOADER, (
    "save_user_settings no longer writes settings_format"
)

print("settings boot keys wiring: PASS")
