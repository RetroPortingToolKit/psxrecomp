#!/usr/bin/env python3
"""Audit a flat game/setup ZIP without extracting or launching its contents."""
import argparse
from pathlib import PurePosixPath
import re
import sys
import zipfile

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib


HD_SURFACE = re.compile(r"hd[ ._-]*(?:textures?|packs?)|texture[ ._-]*packs?", re.I)
# These checked-in synthetic cards are SDK test inputs, never player saves.
SDK_CARD_FIXTURES = {
    "psxrecomp/runtime/tests/fixtures/memcards/card1.mcd",
    "psxrecomp/runtime/tests/fixtures/memcards/card2.mcd",
    "psxrecomp/runtime/tests/fixtures/memcards/dummy.0.mcr",
    "psxrecomp/runtime/tests/fixtures/memcards/dummy.1.mcr",
    "psxrecomp/runtime/tests/test_card_synth.mcd",
    "psxrecomp/runtime/tests/test_dir/card1.mcd",
}
PERSONAL_NAMES = {
    "state.toml", "state.toml.tmp", "settings.toml", "keybinds.ini",
    "disc.cfg", "bios.cfg", "overlay_captures.json", "diagnostics-location.txt",
}


def audit(path, game_id, diagnostics_id):
    with zipfile.ZipFile(path) as archive:
        files = {}
        for entry in archive.infolist():
            name = entry.filename.replace("\\", "/")
            parts = PurePosixPath(name).parts
            if name.startswith("/") or ".." in parts or (parts and ":" in parts[0]):
                raise ValueError("unsafe archive path: " + name)
            if not entry.is_dir():
                if name in files:
                    raise ValueError("duplicate archive entry: " + name)
                files[name] = entry
        # Some publishers put the same flat payload inside one named folder.
        if "game.toml" not in files:
            recipes = [name for name in files if name.count("/") == 1 and name.endswith("/game.toml")]
            if len(recipes) == 1:
                prefix = recipes[0].rsplit("/", 1)[0] + "/"
                if all(name.startswith(prefix) for name in files):
                    files = {name[len(prefix):]: entry for name, entry in files.items()}

        def read(name):
            entry = files.get(name)
            if entry is None or entry.file_size == 0:
                raise ValueError("missing or empty package file: " + name)
            if entry.file_size > 4 * 1024 * 1024:
                raise ValueError("unexpectedly large package metadata: " + name)
            return archive.read(entry)

        def executable(name, entry):
            if "/" in name:
                return False
            if name.lower().endswith(".exe"):
                return entry.file_size > 0
            with archive.open(entry) as stream:
                magic = stream.read(4)
            return magic in {
                b"\x7fELF",
                # Thin Mach-O 32/64-bit, both byte orders.
                b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
                # Universal Mach-O fat/fat64, both byte orders.
                b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca",
            }

        recipe = tomllib.loads(read("game.toml").decode("utf-8-sig"))
        if recipe.get("game", {}).get("id") != game_id:
            raise ValueError("game.toml game ID does not match " + game_id)
        read("replay-diagnostics.py")
        if not any(executable(name, entry) for name, entry in files.items()):
            raise ValueError("no executable adjacent to replay-diagnostics.py")

        runtime = []
        for name in sorted(files):
            lower = name.lower()
            parts = PurePosixPath(lower).parts
            leaf = parts[-1]
            if leaf in PERSONAL_NAMES or "saves" in parts or "diagnostics" in parts or (
                    lower.startswith("mods/installed/")):
                raise ValueError("personal state in package: " + name)
            suffix = PurePosixPath(lower).suffix
            if recipe.get("runtime", {}).get("openbios") is False and lower in {
                    root + leaf for root in ("bios/", "psxrecomp/bios/")
                    for leaf in ("openbios.bin", "openbios.toml", "openbios.license")}:
                raise ValueError("OpenBIOS asset in retail-BIOS-only package: " + name)
            if suffix in {".cue", ".iso", ".img", ".chd", ".mcd", ".mcr", ".rom", ".pst"}:
                if name not in SDK_CARD_FIXTURES:
                    raise ValueError("disc, BIOS or save payload in package: " + name)
            if suffix == ".bin" and lower not in {"bios/openbios.bin", "psxrecomp/bios/openbios.bin"}:
                raise ValueError("unexpected binary image in package: " + name)
            if re.match(r"scph.*\.(bin|rom)(?:\..*)?$", leaf):
                raise ValueError("retail BIOS image in package: " + name)
            # The SDK retains its framework built-in authoring catalog. The
            # title's CMake exclusion controls whether it reaches runtime.
            if lower.startswith("mods/") and suffix in {".toml", ".json"}:
                text = read(name).decode("utf-8-sig")
                if HD_SURFACE.search(text) or HD_SURFACE.search(name):
                    raise ValueError("HD texture surface in title/runtime catalog: " + name)
                if leaf == "manifest.toml":
                    manifest = tomllib.loads(text)
                    if lower.startswith("mods/bundled/" + diagnostics_id.lower() + "/") and manifest.get("id") == diagnostics_id:
                        runtime.append((name, manifest))
                    if manifest.get("id") == diagnostics_id:
                        validate_diagnostics(manifest, name, game_id, diagnostics_id)
        if not runtime:
            raise ValueError("runtime mods/bundled catalog is missing " + diagnostics_id)
        return len(runtime)


def validate_diagnostics(manifest, name, game_id, diagnostics_id):
    if not any(target.get("game_id") == game_id for target in manifest.get("target", [])):
        raise ValueError("diagnostics manifest has the wrong game target: " + name)
    features = {feature.get("id"): feature for feature in manifest.get("feature", [])}
    for feature_id, plugin_id in (("recording", diagnostics_id), ("osd", diagnostics_id + ".osd")):
        feature = features.get(feature_id, {})
        if feature.get("default_enabled") is not True or feature.get("channel") == "developer":
            raise ValueError("diagnostics " + feature_id + " must ship enabled by default: " + name)
        if not any(plugin.get("feature") == feature_id and plugin.get("id") == plugin_id
                   for plugin in manifest.get("plugin", [])):
            raise ValueError("diagnostics " + feature_id + " plugin is missing: " + name)
    if manifest.get("channel") == "developer":
        raise ValueError("diagnostics package cannot be developer-only: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zip")
    parser.add_argument("--game-id", required=True)
    parser.add_argument("--diagnostics-id", required=True)
    args = parser.parse_args()
    try:
        count = audit(args.zip, args.game_id, args.diagnostics_id)
    except (ValueError, OSError, zipfile.BadZipFile, KeyError, UnicodeError) as error:
        sys.exit("diagnostics package audit failed: " + str(error))
    print("Diagnostics package audit passed: {} ({} runtime manifest(s))".format(args.diagnostics_id, count))


if __name__ == "__main__":
    main()
