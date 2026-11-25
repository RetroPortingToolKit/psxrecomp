#!/usr/bin/env python3
"""Audit a flat game/setup ZIP (or its staged directory) without launching it.

The argument is a release ZIP or a staged payload directory. A directory is
read through the same checks, so a Linux payload is audited before it is
wrapped into an AppImage (an AppImage has no ZIP to audit).
"""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
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


class _DirEntry:
    def __init__(self, root, name):
        self.filename = name
        self._path = os.path.join(root, *name.split("/"))
        self.file_size = os.path.getsize(self._path)

    def is_dir(self):
        return False


class _DirArchive:
    """The read-only subset of zipfile.ZipFile the audit uses, over a directory."""

    def __init__(self, root):
        self._root = str(root)
        self._entries = []
        for base, dirs, files in os.walk(self._root):
            dirs.sort()
            for leaf in sorted(files):
                rel = os.path.relpath(os.path.join(base, leaf), self._root)
                self._entries.append(_DirEntry(self._root, rel.replace(os.sep, "/")))

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False

    def infolist(self):
        return list(self._entries)

    def read(self, entry):
        with open(entry._path, "rb") as stream:
            return stream.read()

    def open(self, entry):
        return open(entry._path, "rb")


def _open_package(path):
    if os.path.isdir(path):
        return _DirArchive(path)
    return zipfile.ZipFile(path)


def declared_overlay_files(archive, files, read):
    """Use the payload gate's hash-bound disc-patch exception in ZIPs too."""
    allowed = set()
    for name in sorted(files):
        if not name.lower().startswith("mods/") or not name.lower().endswith("/manifest.toml"):
            continue
        manifest = tomllib.loads(read(name).decode("utf-8-sig"))
        for overlay in manifest.get("overlay", []):
            relative = PurePosixPath(overlay.get("file", ""))
            digest = overlay.get("sha256", "")
            expected = overlay.get("expected_sha256", "")
            if (relative.is_absolute() or not relative.parts or ".." in relative.parts or
                    relative.parts[0] != "assets" or relative.suffix.lower() != ".bin" or
                    overlay.get("target") not in ("disc_user", "disc_raw") or
                    not all(isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{64}", value)
                            for value in (digest, expected))):
                continue
            asset = (PurePosixPath(name).parent / relative).as_posix()
            if asset not in files:
                raise ValueError("declared overlay asset is missing: " + asset)
            actual = hashlib.sha256()
            with archive.open(files[asset]) as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    actual.update(chunk)
            if actual.hexdigest() != digest.lower():
                raise ValueError("declared overlay asset hash mismatch: " + asset)
            allowed.add(asset)
    return allowed


def audit(path, game_id, diagnostics_id, public, replay_helper=True, allow_hd_textures=False):
    """Audit either release channel: recording defaults on, display off.
    Explicit player choices live outside release archives."""
    with _open_package(path) as archive:
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
        # Titles on psxrecomp_add_playtest_diagnostics ship the shared replay
        # launcher beside the executable; a title with its own recorder does not.
        if replay_helper:
            read("replay-diagnostics.py")
        if not any(executable(name, entry) for name, entry in files.items()):
            raise ValueError("no executable adjacent to replay-diagnostics.py" if replay_helper
                             else "no executable at the package root")

        overlay_assets = declared_overlay_files(archive, files, read)
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
            if (suffix == ".bin" and lower not in {"bios/openbios.bin", "psxrecomp/bios/openbios.bin"}
                    and name not in overlay_assets):
                raise ValueError("unexpected binary image in package: " + name)
            if re.match(r"scph.*\.(bin|rom)(?:\..*)?$", leaf):
                raise ValueError("retail BIOS image in package: " + name)
            # The SDK retains its framework built-in authoring catalog. The
            # title's CMake exclusion controls whether it reaches runtime.
            if lower.startswith("mods/") and suffix in {".toml", ".json"}:
                text = read(name).decode("utf-8-sig")
                if not allow_hd_textures and (HD_SURFACE.search(text) or HD_SURFACE.search(name)):
                    raise ValueError("HD texture surface in title/runtime catalog: " + name)
                if leaf == "manifest.toml":
                    manifest = tomllib.loads(text)
                    staged = lower.startswith("mods/bundled/" + diagnostics_id.lower() + "/")
                    if staged and manifest.get("id") == diagnostics_id:
                        runtime.append((name, manifest))
                    if manifest.get("id") == diagnostics_id:
                        # Only the build-staged runtime copy follows the mode.
                        validate_diagnostics(manifest, name, game_id, diagnostics_id,
                                             staged=staged)
        if not runtime:
            raise ValueError("runtime mods/bundled catalog is missing " + diagnostics_id)
        return len(runtime)


def validate_diagnostics(manifest, name, game_id, diagnostics_id, staged):
    if not any(target.get("game_id") == game_id for target in manifest.get("target", [])):
        raise ValueError("diagnostics manifest has the wrong game target: " + name)
    features = {feature.get("id"): feature for feature in manifest.get("feature", [])}
    recording = "recording" if "recording" in features else "input-recording"
    if (diagnostics_id in ("mmx4.diagnostics", "mmx5.diagnostics") and
            recording not in features and "inputs" in features):
        recording = "inputs"  # Preserve the saved public control ID and plugin binding.
    provided = [(recording, diagnostics_id)]
    if "osd" in features:
        provided.append(("osd", diagnostics_id + ".osd"))
    for feature_id, plugin_id in provided:
        feature = features.get(feature_id, {})
        # Regular visible Mods controls, independent of presentation packages.
        # Legacy authored false recording defaults are rewritten by staging.
        if feature.get("channel") == "developer":
            raise ValueError("diagnostics " + feature_id + " cannot be developer-only: " + name)
        wanted = feature_id != "osd"
        value = feature.get("default_enabled")
        if not isinstance(value, bool) or (staged and value is not wanted) or (feature_id == "osd" and value):
            raise ValueError("diagnostics {} must ship {} by default: {}".format(
                feature_id, "enabled" if wanted else "disabled", name))
        if feature.get("hidden") or feature.get("quick"):
            raise ValueError("diagnostics must be visible on Mods without default Quick Mods cards: " + name)
        if not any(plugin.get("feature") == feature_id and plugin.get("id") == plugin_id
                   for plugin in manifest.get("plugin", [])):
            raise ValueError("diagnostics " + feature_id + " plugin is missing: " + name)
    if manifest.get("channel") == "developer":
        raise ValueError("diagnostics package cannot be developer-only: " + name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("zip", help="release ZIP or staged payload directory")
    parser.add_argument("--game-id", required=True)
    parser.add_argument("--diagnostics-id", required=True)
    parser.add_argument("--mode", required=True, choices=("public", "playtest"),
                        help="release channel; recording defaults on and optional display off in both")
    parser.add_argument("--no-replay-helper", action="store_true",
                        help="the title records with its own diagnostics package and ships "
                             "no replay-diagnostics.py")
    parser.add_argument("--allow-hd-textures", action="store_true",
                        help="the title explicitly retains HD texture controls in its catalog")
    args = parser.parse_args()
    try:
        count = audit(args.zip, args.game_id, args.diagnostics_id, args.mode == "public",
                      replay_helper=not args.no_replay_helper,
                      allow_hd_textures=args.allow_hd_textures)
    except (ValueError, OSError, zipfile.BadZipFile, KeyError, UnicodeError) as error:
        sys.exit("diagnostics package audit failed: " + str(error))
    print("Diagnostics package audit passed: {} ({} runtime manifest(s), {} mode)".format(
        args.diagnostics_id, count, args.mode))


if __name__ == "__main__":
    main()
