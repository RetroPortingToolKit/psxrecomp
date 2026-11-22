#!/usr/bin/env python3
"""Exercise the release ZIP auditor against actual synthetic archives."""
import importlib.util
import ast
import builtins
import hashlib
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/ci/audit_playtest_diagnostics_package.py"
spec = importlib.util.spec_from_file_location("package_audit", SCRIPT)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)
DIAG = "fixture.diagnostics"
GAME = "SLUS-00000"
MANIFEST = "mods/bundled/{}/1.0.0/manifest.toml".format(DIAG)
SOURCE = "mods/preloaded/packages/{}/1.0.0/manifest.toml".format(DIAG)
TEXT = '''id = "fixture.diagnostics"
[[target]]
game_id = "SLUS-00000"
[[feature]]
id = "recording"
default_enabled = true
[[feature]]
id = "osd"
default_enabled = false
[[plugin]]
feature = "recording"
id = "fixture.diagnostics"
[[plugin]]
feature = "osd"
id = "fixture.diagnostics.osd"
'''
# Both release channels use the same recording-on/display-off policy.
PLAYTEST_TEXT = TEXT


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.zip = Path(self.tmp.name) / "release.zip"
        self.files = {"game.toml": '[game]\nid="SLUS-00000"\n',
                      "Game.exe": b"MZfixture", "replay-diagnostics.py": "# helper\n",
                      MANIFEST: TEXT}

    def write(self, prefix=""):
        with zipfile.ZipFile(self.zip, "w") as archive:
            for name, content in self.files.items():
                archive.writestr(prefix + name, content)

    def check(self, public=True):
        self.write()
        return audit.audit(self.zip, GAME, DIAG, public)

    def test_valid_flat_and_one_folder_packages(self):
        self.assertEqual(self.check(), 1)
        self.write("Release/")
        self.assertEqual(audit.audit(self.zip, GAME, DIAG, True), 1)

    def test_valid_setup_sources_and_excluded_framework_hd_catalog(self):
        self.files[SOURCE] = TEXT
        self.files["psxrecomp/mods/builtin/packages/psx.enhancement.hd-textures/1.0.0/manifest.toml"] = 'id="psx.enhancement.hd-textures"\n'
        self.files["psxrecomp/runtime/tests/fixtures/memcards/card1.mcd"] = b"synthetic"
        self.assertEqual(self.check(), 1)

    def test_cli_and_native_linux_executable(self):
        del self.files["Game.exe"]
        self.files["Game"] = b"\x7fELFfixture"
        self.write()
        command = [sys.executable, str(SCRIPT), str(self.zip),
                   "--game-id", GAME, "--diagnostics-id", DIAG]
        result = subprocess.run(command + ["--mode", "public"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("audit passed", result.stdout)
        self.assertIn("public mode", result.stdout)
        # The mode is mandatory: a packager must say which kind of build it audits.
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--mode", result.stderr)
        result = subprocess.run(command + ["--mode", "playtest"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.files[MANIFEST] = PLAYTEST_TEXT
        self.write()
        result = subprocess.run(command + ["--mode", "playtest"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("playtest mode", result.stdout)

    def test_complete_native_macos_archives(self):
        del self.files["Game.exe"]
        self.files[SOURCE] = TEXT
        self.files["game.toml"] += '[runtime]\nopenbios=false\n'
        for magic in (b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                      b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe",
                      b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                      b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
            for prefix in ("", "Release/"):
                with self.subTest(magic=magic.hex(), prefix=prefix):
                    self.files["Game"] = magic + b"synthetic Mach-O fixture"
                    self.write(prefix)
                    self.assertEqual(audit.audit(self.zip, GAME, DIAG, True), 1)

    def test_plain_truncated_or_nested_binaries_are_not_adjacent_executables(self):
        del self.files["Game.exe"]
        for name, content in (("Game", b"ordinary text"),
                              ("Game", b"\xcf\xfa\xed"),
                              ("Game", b"\x89PNGfixture"),
                              ("tools/Game", b"\xcf\xfa\xed\xfefixture"),
                              ("tools/Game.exe", b"MZfixture")):
            with self.subTest(name=name, content=content):
                self.files[name] = content
                with self.assertRaisesRegex(ValueError, "no executable adjacent"):
                    self.check()
                del self.files[name]

    def test_python38_syntax_and_tomli_fallback(self):
        original_import = builtins.__import__

        def import_without_tomllib(name, *args, **kwargs):
            if name == "tomllib":
                raise ModuleNotFoundError("simulated Python 3.8")
            if name == "tomli":
                return audit.tomllib
            return original_import(name, *args, **kwargs)

        for script in (SCRIPT, ROOT / "tools/replay_diagnostics.py", ROOT / "tools/check_setup_bios_assets.py"):
            with self.subTest(script=script.name):
                ast.parse(script.read_text(encoding="utf-8"), feature_version=8)
                with mock.patch("builtins.__import__", side_effect=import_without_tomllib):
                    namespace = runpy.run_path(str(script))
                self.assertIs(namespace["tomllib"], audit.tomllib)

    def test_each_default_and_plugin_is_required(self):
        for bad in (TEXT.replace("default_enabled = true", "default_enabled = false", 1),
                    TEXT.replace('id = "osd"\ndefault_enabled = false', 'id = "osd"\ndefault_enabled = true'),
                    TEXT.replace('id = "fixture.diagnostics.osd"', 'id = "wrong.osd"'),
                    'channel="developer"\n' + TEXT):
            with self.subTest(manifest=bad):
                self.files[MANIFEST] = bad
                with self.assertRaises(ValueError):
                    self.check()

    def test_recording_on_display_off_in_both_channels(self):
        for public in (False, True):
            self.files[SOURCE] = TEXT
            self.files[MANIFEST] = TEXT
            self.assertEqual(self.check(public), 1)
            for bad in (TEXT.replace("default_enabled = true", "default_enabled = false", 1),
                        TEXT.replace('id = "osd"\ndefault_enabled = false', 'id = "osd"\ndefault_enabled = true'),
                        TEXT.replace("default_enabled = true\n", "", 1),
                        TEXT.replace('id = "recording"', 'id = "recording"\nhidden = true'),
                        TEXT.replace('id = "recording"', 'id = "recording"\nquick = true')):
                with self.subTest(public=public, manifest=bad):
                    self.files[MANIFEST] = bad
                    with self.assertRaises(ValueError):
                        self.check(public)

    def test_legacy_source_default_does_not_override_staged_recording(self):
        self.files[SOURCE] = TEXT.replace("default_enabled = true", "default_enabled = false", 1)
        self.files[MANIFEST] = TEXT
        for public in (False, True):
            self.assertEqual(self.check(public), 1)

    def test_missing_helper_executable_or_runtime_catalog(self):
        for name in ("replay-diagnostics.py", "Game.exe", MANIFEST):
            with self.subTest(file=name):
                content = self.files.pop(name)
                with self.assertRaises(ValueError):
                    self.check()
                self.files[name] = content

    def test_title_recorder_without_replay_helper(self):
        # A title with its own recorder ships no replay-diagnostics.py.
        del self.files["replay-diagnostics.py"]
        self.write()
        with self.assertRaises(ValueError):
            audit.audit(self.zip, GAME, DIAG, True)
        self.assertEqual(audit.audit(self.zip, GAME, DIAG, True, replay_helper=False), 1)
        del self.files["Game.exe"]
        self.write()
        with self.assertRaisesRegex(ValueError, "no executable at the package root"):
            audit.audit(self.zip, GAME, DIAG, True, replay_helper=False)

    def test_known_x4_inputs_control_keeps_its_public_id_and_guards(self):
        del self.files[MANIFEST]
        package_id = "mmx4.diagnostics"
        name = "mods/bundled/mmx4.diagnostics/1.0.0/manifest.toml"
        text = TEXT.replace(DIAG, package_id).replace('"recording"', '"inputs"')
        for public in (False, True):
            self.files[name] = text
            self.write()
            self.assertEqual(audit.audit(self.zip, GAME, package_id, public), 1)
            for bad in (text.replace("default_enabled = true", "default_enabled = false", 1),
                        text.replace('feature = "inputs"', 'feature = "wrong"'),
                        text.replace('id = "inputs"', 'id = "inputs"\nhidden = true'),
                        text.replace('id = "osd"\ndefault_enabled = false',
                                     'id = "osd"\ndefault_enabled = true')):
                self.files[name] = bad
                self.write()
                with self.assertRaises(ValueError):
                    audit.audit(self.zip, GAME, package_id, public)
        self.files[name] = text.replace(package_id, DIAG)
        self.files[MANIFEST] = self.files.pop(name)
        self.write()
        with self.assertRaises(ValueError):
            audit.audit(self.zip, GAME, DIAG, True)

    def patch_asset(self):
        name = "mods/bundled/fixture.patch/1.0.0/manifest.toml"
        data = b"authored disc patch fixture"
        text = ('id="fixture.patch"\n[[overlay]]\ntarget="disc_raw"\noffset=16\n'
                'file="assets/silence.bin"\nsha256="%s"\nexpected_sha256="%s"\n'
                % (hashlib.sha256(data).hexdigest(), "0" * 64))
        self.files[name] = text
        self.files["mods/bundled/fixture.patch/1.0.0/assets/silence.bin"] = data
        return name, text

    def test_only_declared_hash_bound_disc_patch_assets_are_allowed(self):
        name, text = self.patch_asset()
        self.assertEqual(self.check(), 1)
        for invalid in (text.replace('target="disc_raw"', 'target="ram"'),
                        text.replace('file="assets/silence.bin"', 'file="../silence.bin"'),
                        text.replace('expected_sha256="' + "0" * 64 + '"', ''),
                        text.replace('sha256="' + hashlib.sha256(b"authored disc patch fixture").hexdigest() + '"',
                                     'sha256="' + "f" * 64 + '"')):
            self.files[name] = invalid
            with self.assertRaises(ValueError):
                self.check()
        self.files[name] = text
        self.files["Game.bin"] = b"undeclared image"
        with self.assertRaisesRegex(ValueError, "unexpected binary image"):
            self.check()
        del self.files["Game.bin"]
        self.files["mods/bundled/fixture.patch/1.0.0/assets/silence.bin"] = b"tampered"
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            self.check()

    def test_staged_directory_is_audited_like_a_zip(self):
        stage = Path(self.tmp.name) / "stage"
        for name, content in self.files.items():
            path = stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            if isinstance(content, str):
                content = content.encode()
            path.write_bytes(content)
        self.assertEqual(audit.audit(stage, GAME, DIAG, True), 1)
        self.assertEqual(audit.audit(stage, GAME, DIAG, False), 1)
        (stage / "saves").mkdir()
        (stage / "saves/card1.mcd").write_bytes(b"card")
        with self.assertRaisesRegex(ValueError, "personal state"):
            audit.audit(stage, GAME, DIAG, True)

    def test_wrong_game_recipe_and_manifest_target(self):
        self.files["game.toml"] = '[game]\nid="WRONG"\n'
        with self.assertRaisesRegex(ValueError, "game ID"):
            self.check()
        self.files["game.toml"] = '[game]\nid="SLUS-00000"\n'
        self.files[MANIFEST] = TEXT.replace(GAME, "WRONG")
        with self.assertRaisesRegex(ValueError, "target"):
            self.check()

    def test_hd_package_plugin_and_control_surfaces_fail(self):
        for name, content in (
                ("mods/bundled/psx.enhancement.hd-textures/1.0.0/manifest.toml", 'id="psx.enhancement.hd-textures"'),
                (SOURCE, TEXT + '\n[[plugin]]\nid="psx.enhancement.hd-textures"\n'),
                ("mods/preloaded/packages/fixture/1.0.0/options.toml", 'texture_pack_mode="off"\n')):
            with self.subTest(file=name):
                self.files[name] = content
                with self.assertRaisesRegex(ValueError, "HD texture"):
                    self.check()
                del self.files[name]

    def test_personal_state_save_and_retail_images_fail(self):
        for name in ("mods/state.toml", "settings.toml", "saves/card1.mcd", "saves/diagnostics/inputs.csv",
                     "mods/installed/private.psxmod", "SCPH1001.BIN", "disc/game.cue",
                     "psxrecomp/bios/SCPH5552.rom", "psxrecomp/runtime/tests/private.mcd"):
            with self.subTest(file=name):
                self.files[name] = "private"
                with self.assertRaises(ValueError):
                    self.check()
                del self.files[name]

    def test_explicit_hd_catalog_policy_keeps_other_gates(self):
        self.files["mods/bundled/psx.enhancement.hd-textures/1.0.0/manifest.toml"] = 'id="psx.enhancement.hd-textures"'
        self.write()
        with self.assertRaisesRegex(ValueError, "HD texture"):
            audit.audit(self.zip, GAME, DIAG, True)
        self.assertEqual(audit.audit(self.zip, GAME, DIAG, True, allow_hd_textures=True), 1)
        command = [sys.executable, str(SCRIPT), str(self.zip), "--game-id", GAME,
                   "--diagnostics-id", DIAG, "--mode", "public", "--allow-hd-textures"]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        for name, content in (("saves/card1.mcd", "private"),
                              (MANIFEST, TEXT.replace("default_enabled = true", "default_enabled = false", 1)), ("disc/game.cue", "disc")):
            with self.subTest(file=name):
                original = self.files.get(name)
                self.files[name] = content
                self.write()
                with self.assertRaises(ValueError):
                    audit.audit(self.zip, GAME, DIAG, True, allow_hd_textures=True)
                if original is None:
                    del self.files[name]
                else:
                    self.files[name] = original

    def test_retail_only_recipe_rejects_openbios(self):
        self.files["game.toml"] += '[runtime]\nopenbios=false\n'
        self.assertEqual(self.check(), 1)
        self.files["psxrecomp/bios/openbios.bin"] = "synthetic"
        with self.assertRaisesRegex(ValueError, "retail-BIOS-only"):
            self.check()

    def test_unsafe_and_duplicate_paths_fail(self):
        self.files["../escape.txt"] = "bad"
        with self.assertRaisesRegex(ValueError, "unsafe"):
            self.check()
        del self.files["../escape.txt"]
        self.write()
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with zipfile.ZipFile(self.zip, "a") as archive:
                archive.writestr(MANIFEST, TEXT)
        with self.assertRaisesRegex(ValueError, "duplicate"):
            audit.audit(self.zip, GAME, DIAG, True)


if __name__ == "__main__":
    unittest.main()
