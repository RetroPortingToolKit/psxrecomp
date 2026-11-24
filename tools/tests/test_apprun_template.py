#!/usr/bin/env python3
"""The scaffold's AppImage entry point (templates/AppRun.in), run for real.

The template is rendered the way a title gets it: setup_project.sh's
fill_tokens.py pass at project creation, then package_title_release.py's
render_template at release time. The rendered script is run under each POSIX
shell available (sh, and dash, which is /bin/sh on Debian-family hosts) with a
fake APPDIR whose runtime only reports that it ran. Checks that:

  1. a relative <PREFIX>_DATA_DIR is made absolute, both for the path
     <PREFIX>_SEED_ONLY prints and for the launcher symlink that is exec'd
     after the script cd's into the data directory;
  2. a <PREFIX>_DISC path holding backslashes and quotes reaches game.toml
     byte for byte (awk -v would turn a literal backslash-n into a newline);
  3. a <PREFIX>_DISC value holding a control character is refused, and no
     disc line is written;
  4. the override lands in [game] even when [game] is the last table.

Run:  python3 tools/tests/test_apprun_template.py
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

FW = Path(__file__).resolve().parents[2]
TOOLKIT = FW / "tools" / "new_project_layout"
TEMPLATE = TOOLKIT / "templates" / "AppRun.in"
sys.path.insert(0, str(FW / "tools"))
import package_title_release as ptr  # noqa: E402

tomllib = ptr.tomllib

PREFIX = "FIXTURE"
EXE = "fixture-runtime"
PAYLOAD = "fixture"
RELEASE_TOKENS = dict(VERSION="1.2.3", APP_NAME="Fixture", EXE_NAME=EXE,
                      PAYLOAD_DIR=PAYLOAD, ARTIFACT_NAME="fixture")
GAME_TOML = '[game]\nname = "Fixture"\ndisc = "disc/old.cue"\n\n[recompiler]\nstrict = true\n'


def _shells():
    found = {}
    for name in ("sh", "dash"):
        path = shutil.which(name)
        if path:
            found.setdefault(os.path.realpath(path), path)
    return list(found.values())


@unittest.skipIf(os.name == "nt", "AppRun is a POSIX shell script")
class AppRunTemplate(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="psx-apprun-"))
        self.addCleanup(shutil.rmtree, self.tmp)
        project = self.tmp / "AppRun.project"
        r = subprocess.run([sys.executable, str(TOOLKIT / "fill_tokens.py"),
                            str(TEMPLATE), str(project), "--set", "ENV_PREFIX=" + PREFIX],
                           capture_output=True, text=True, check=False)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.app_run = self.tmp / "AppRun"
        self.app_run.write_text(ptr.render_template(project, RELEASE_TOKENS),
                                encoding="utf-8", newline="\n")
        self.app_dir = self.tmp / "appdir"
        runtime = self.app_dir / "usr" / "bin" / EXE
        runtime.parent.mkdir(parents=True)
        runtime.write_text('#!/bin/sh\nprintf "runtime ran in %s\\n" "$PWD"\n', encoding="utf-8")
        runtime.chmod(0o755)
        self.payload = self.app_dir / "usr" / "share" / PAYLOAD
        self.payload.mkdir(parents=True)
        (self.payload / "game.toml").write_text(GAME_TOML, encoding="utf-8")

    def run_app(self, shell, data_dir, **env):
        full = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(self.tmp),
                "APPDIR": str(self.app_dir), PREFIX + "_DATA_DIR": str(data_dir)}
        full.update({PREFIX + "_" + key: value for key, value in env.items()})
        return subprocess.run([shell, str(self.app_run)], cwd=self.tmp, env=full,
                              capture_output=True, text=True, check=False)

    def test_relative_data_dir_is_made_absolute(self):
        for shell in _shells():
            with self.subTest(shell=shell):
                r = self.run_app(shell, "data-" + Path(shell).name, SEED_ONLY="1")
                self.assertEqual(r.returncode, 0, r.stderr)
                printed = r.stdout.strip()
                self.assertTrue(os.path.isabs(printed), printed)
                self.assertTrue(Path(printed).samefile(self.tmp / ("data-" + Path(shell).name)))

                r = self.run_app(shell, "data-" + Path(shell).name)
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("runtime ran in", r.stdout)

    def test_disc_path_is_written_byte_for_byte(self):
        disc = self.tmp / 'dumps\\new \\d "quoted".cue'
        disc.write_text("", encoding="utf-8")
        for shell in _shells():
            with self.subTest(shell=shell):
                data = self.tmp / ("data-" + Path(shell).name)
                r = self.run_app(shell, data, DISC=str(disc))
                self.assertEqual(r.returncode, 0, r.stderr)
                config = tomllib.loads((data / "game.toml").read_text(encoding="utf-8"))
                self.assertEqual(config["game"]["disc"], str(disc))
                self.assertEqual(config["recompiler"], {"strict": True})

    def test_disc_value_with_a_control_character_is_refused(self):
        disc = self.tmp / "dumps\nnew.cue"
        disc.write_text("", encoding="utf-8")
        for shell in _shells():
            with self.subTest(shell=shell):
                data = self.tmp / ("data-" + Path(shell).name)
                r = self.run_app(shell, data, DISC=str(disc))
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("control character", r.stderr)
                self.assertIn("runtime ran in", r.stdout)
                text = (data / "game.toml").read_text(encoding="utf-8")
                self.assertEqual(text, GAME_TOML, "the release game.toml is left untouched")

    def test_disc_lands_in_game_when_game_is_the_last_table(self):
        (self.payload / "game.toml").write_text(
            '[recompiler]\nstrict = true\n\n[game]\nname = "Fixture"\ndisc = "disc/old.cue"\n',
            encoding="utf-8")
        disc = self.tmp / "game.cue"
        disc.write_text("", encoding="utf-8")
        for shell in _shells():
            with self.subTest(shell=shell):
                data = self.tmp / ("data-" + Path(shell).name)
                r = self.run_app(shell, data, DISC=str(disc))
                self.assertEqual(r.returncode, 0, r.stderr)
                config = tomllib.loads((data / "game.toml").read_text(encoding="utf-8"))
                self.assertEqual(config["game"]["disc"], str(disc))
                self.assertNotIn("disc", config["recompiler"])

    def test_disc_lands_in_game_whatever_its_header_looks_like(self):
        disc = self.tmp / "game.cue"
        disc.write_text("", encoding="utf-8")
        headers = {"a trailing comment": "[game] # the title\n",
                   "indentation": "  [game]\n",
                   "CRLF line endings": "[game]\r\n"}
        for what, header in headers.items():
            newline = "\r\n" if header.endswith("\r\n") else "\n"
            body = newline.join(('name = "Fixture"', 'disc = "disc/old.cue"', "",
                                 "[recompiler]", "strict = true", ""))
            (self.payload / "game.toml").write_bytes((header + body).encode("utf-8"))
            for shell in _shells():
                with self.subTest(header=what, shell=shell):
                    data = self.tmp / ("data-%s-%s" % (len(header), Path(shell).name))
                    r = self.run_app(shell, data, DISC=str(disc))
                    self.assertEqual(r.returncode, 0, r.stderr)
                    config = tomllib.loads((data / "game.toml").read_text(encoding="utf-8"))
                    self.assertEqual(config["game"]["disc"], str(disc))

    def test_relative_disc_path_is_made_absolute(self):
        (self.tmp / "game.cue").write_text("", encoding="utf-8")
        for shell in _shells():
            with self.subTest(shell=shell):
                data = self.tmp / ("data-" + Path(shell).name)
                r = self.run_app(shell, data, DISC="game.cue")
                self.assertEqual(r.returncode, 0, r.stderr)
                config = tomllib.loads((data / "game.toml").read_text(encoding="utf-8"))
                self.assertTrue(os.path.isabs(config["game"]["disc"]), config["game"]["disc"])
                self.assertTrue(Path(config["game"]["disc"]).samefile(self.tmp / "game.cue"))

    def test_quoted_toml_keys(self):
        disc = self.tmp / "game.cue"
        disc.touch()
        for quote in ('"', "'"):
            game = f'[{quote}game{quote}]\nname = "Fixture"\n'
            single = game + f'{quote}disc{quote} = "disc/old.cue"\n'
            multiple = game + f'{quote}discs{quote} = ["disc/1.cue", "disc/2.cue"]\n'
            for shell in _shells():
                with self.subTest(quote=quote, shell=shell):
                    data = self.tmp / ("data-" + Path(shell).name)
                    staged = data / "game.toml"
                    (self.payload / "game.toml").write_text(single, encoding="utf-8")
                    result = self.run_app(shell, data, DISC=str(disc))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    config = tomllib.loads(staged.read_text(encoding="utf-8"))
                    self.assertEqual(config["game"]["disc"], str(disc))

                    (self.payload / "game.toml").write_text(multiple, encoding="utf-8")
                    result = self.run_app(shell, data, DISC=str(disc))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("lists discs", result.stderr)
                    self.assertEqual(staged.read_text(encoding="utf-8"), multiple)

    def test_disc_with_no_game_table_is_reported(self):
        release = '[recompiler]\nstrict = true\n'
        (self.payload / "game.toml").write_text(release, encoding="utf-8")
        disc = self.tmp / "game.cue"
        disc.write_text("", encoding="utf-8")
        for shell in _shells():
            with self.subTest(shell=shell):
                data = self.tmp / ("data-" + Path(shell).name)
                r = self.run_app(shell, data, DISC=str(disc))
                self.assertEqual(r.returncode, 0, r.stderr)
                self.assertIn("no [game] table", r.stderr)
                self.assertIn("runtime ran in", r.stdout)
                self.assertEqual((data / "game.toml").read_text(encoding="utf-8"), release)

    def test_disc_override_that_cannot_apply_is_reported(self):
        multi = '[game]\nname = "Fixture"\ndiscs = [\n    "disc/1.cue",\n    "disc/2.cue",\n]\n'
        cases = {"a missing file": (GAME_TOML, str(self.tmp / "missing.cue"), "not a file"),
                 "a multi-disc title": (multi, str(self.tmp / "game.cue"), "lists discs")}
        (self.tmp / "game.cue").write_text("", encoding="utf-8")
        for what, (release, disc, message) in cases.items():
            (self.payload / "game.toml").write_text(release, encoding="utf-8")
            for shell in _shells():
                with self.subTest(case=what, shell=shell):
                    data = self.tmp / ("data-%s-%s" % (len(release), Path(shell).name))
                    r = self.run_app(shell, data, DISC=disc)
                    self.assertEqual(r.returncode, 0, r.stderr)
                    self.assertIn(message, r.stderr)
                    self.assertIn("runtime ran in", r.stdout)
                    self.assertEqual((data / "game.toml").read_text(encoding="utf-8"), release)

if __name__ == "__main__":
    unittest.main(verbosity=1)
