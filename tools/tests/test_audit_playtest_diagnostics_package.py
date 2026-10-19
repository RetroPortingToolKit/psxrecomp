#!/usr/bin/env python3
"""Exercise the release ZIP auditor against actual synthetic archives."""
import importlib.util
import ast
import builtins
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
default_enabled = true
[[plugin]]
feature = "recording"
id = "fixture.diagnostics"
[[plugin]]
feature = "osd"
id = "fixture.diagnostics.osd"
'''


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

    def check(self):
        self.write()
        return audit.audit(self.zip, GAME, DIAG)

    def test_valid_flat_and_one_folder_packages(self):
        self.assertEqual(self.check(), 1)
        self.write("Release/")
        self.assertEqual(audit.audit(self.zip, GAME, DIAG), 1)

    def test_valid_setup_sources_and_excluded_framework_hd_catalog(self):
        self.files[SOURCE] = TEXT
        self.files["psxrecomp/mods/builtin/packages/psx.enhancement.hd-textures/1.0.0/manifest.toml"] = 'id="psx.enhancement.hd-textures"\n'
        self.files["psxrecomp/runtime/tests/fixtures/memcards/card1.mcd"] = b"synthetic"
        self.assertEqual(self.check(), 1)

    def test_cli_and_native_linux_executable(self):
        del self.files["Game.exe"]
        self.files["Game"] = b"\x7fELFfixture"
        self.write()
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.zip),
                                 "--game-id", GAME, "--diagnostics-id", DIAG],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("audit passed", result.stdout)

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
                    self.assertEqual(audit.audit(self.zip, GAME, DIAG), 1)

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
                    TEXT.replace('id = "osd"\ndefault_enabled = true', 'id = "osd"\ndefault_enabled = false'),
                    TEXT.replace('id = "fixture.diagnostics.osd"', 'id = "wrong.osd"'),
                    'channel="developer"\n' + TEXT):
            with self.subTest(manifest=bad):
                self.files[MANIFEST] = bad
                with self.assertRaises(ValueError):
                    self.check()

    def test_source_catalog_defaults_also_required(self):
        self.files[SOURCE] = TEXT.replace("default_enabled = true", "default_enabled = false")
        with self.assertRaisesRegex(ValueError, "enabled by default"):
            self.check()

    def test_missing_helper_executable_or_runtime_catalog(self):
        for name in ("replay-diagnostics.py", "Game.exe", MANIFEST):
            with self.subTest(file=name):
                content = self.files.pop(name)
                with self.assertRaises(ValueError):
                    self.check()
                self.files[name] = content

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
            audit.audit(self.zip, GAME, DIAG)


if __name__ == "__main__":
    unittest.main()
