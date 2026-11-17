#!/usr/bin/env python3
"""Drive runtime/psx_playtest_diagnostics_defaults.cmake against staged catalogs.

Usage: test_playtest_diagnostics_defaults.py [--cmake PATH]
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "runtime/psx_playtest_diagnostics_defaults.cmake"
CMAKE = os.environ.get("PSX_TEST_CMAKE") or shutil.which("cmake")
DIAG = "fixture.diagnostics"
SOURCE = '''format_version = 5
id = "fixture.diagnostics"
version = "1.0.0"
name = "Playtest Diagnostics"
# Brackets [x]; semicolons; and "default_enabled = false" in prose stay as-is.
description = "Off by default; [press] L3 + R3 to mark a problem."

[[target]]
game_id = "SLUS-00000"

[[feature]]
id = "recording"
name = "Input Recording"
default_enabled = false

[[feature]]
id = "other"
default_enabled = false

[[feature]]
name = "Input Display"
default_enabled = false
id = "osd"

[[plugin]]
feature = "recording"
id = "fixture.diagnostics"

[[plugin]]
feature = "osd"
id = "fixture.diagnostics.osd"
'''


def defaults(manifest):
    data = tomllib.loads(manifest)
    return {f["id"]: f.get("default_enabled") for f in data["feature"]}


class DefaultsTests(unittest.TestCase):
    def setUp(self):
        if not CMAKE:
            self.skipTest("cmake not found")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.mods = Path(self.tmp.name) / "mods"

    def stage(self, text, version="1.0.0", package=DIAG):
        path = self.mods / "bundled" / package / version / "manifest.toml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.encode("utf-8"))
        return path

    def run_script(self, public):
        return subprocess.run(
            [CMAKE, "-DPSX_MODS_DIR=" + str(self.mods), "-DPSX_DIAGNOSTICS_ID=" + DIAG,
             "-DPSX_PUBLIC_RELEASE=" + ("1" if public else "0"), "-DPSX_LABEL=fixture",
             "-P", str(SCRIPT)],
            capture_output=True, text=True,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))

    def test_every_channel_records_without_default_overlay(self):
        for public in (False, True):
            for newline in ("\n", "\r\n"):
                with self.subTest(public=public, newline=repr(newline)):
                    source = SOURCE.replace("\n", newline)
                    path = self.stage(source)
                    result = self.run_script(public)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    after = path.read_bytes().decode("utf-8")
                    self.assertEqual(defaults(after),
                                     {"recording": True, "other": False, "osd": False})
                    self.assertEqual(after.replace("default_enabled = true", "default_enabled = false"), source)

    def test_saved_off_and_source_bytes_are_not_modified(self):
        path = self.stage(SOURCE)
        saved = self.mods / "state.toml"
        choice = 'format_version = 2\n[[feature]]\npackage_id = "fixture.diagnostics"\nid = "recording"\nenabled = false\n'
        saved.write_text(choice, encoding="utf-8")
        for public in (True, False):
            result = self.run_script(public)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(defaults(path.read_text())["recording"])
            self.assertEqual(saved.read_text(), choice)
        self.assertFalse(defaults(SOURCE)["recording"])

    def test_existing_input_recording_id_without_overlay(self):
        source = SOURCE[:SOURCE.index('[[feature]]\nname = "Input Display"')]
        source = source.replace('id = "recording"', 'id = "input-recording"')
        path = self.stage(source)
        result = self.run_script(public=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(defaults(path.read_text())["input-recording"])

    def test_missing_default_is_inserted_per_feature(self):
        path = self.stage(SOURCE.replace('name = "Input Display"\ndefault_enabled = false\n',
                                         'name = "Input Display"\n'))
        result = self.run_script(public=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(defaults(path.read_text()),
                         {"recording": True, "other": False, "osd": False})

    def test_every_staged_version_is_handled(self):
        paths = self.stage(SOURCE, "1.0.0"), self.stage(SOURCE, "1.1.0")
        result = self.run_script(public=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        for path in paths:
            self.assertEqual(defaults(path.read_text()),
                             {"recording": True, "other": False, "osd": False})

    def test_missing_recording_or_package_fails(self):
        self.stage(SOURCE.replace('id = "recording"', 'id = "unrelated"'))
        result = self.run_script(public=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("recording feature", result.stderr)
        shutil.rmtree(self.mods)
        self.stage(SOURCE, package="other.diagnostics")
        result = self.run_script(public=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("was not staged", result.stderr)


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "--cmake":
        CMAKE = sys.argv[2]
        del sys.argv[1:3]
    unittest.main()
