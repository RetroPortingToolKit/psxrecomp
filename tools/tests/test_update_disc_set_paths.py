"""update_disc_set.py: game.toml disc paths are portable.

A disc inside the project must be written relative with '/', whatever
separator the caller used, so the same game.toml resolves on Windows, Linux
and macOS. A disc outside the project keeps an absolute path.
"""
import os
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools' / 'new_project_layout'))
import update_disc_set as uds  # noqa: E402


class ConfigDiscPathTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.project = base / 'GameRecomp'
        (self.project / 'disc').mkdir(parents=True)
        self.inside = self.project / 'disc' / 'Game (Disc 2).cue'
        self.inside.write_text('')
        self.outside = base / 'dumps' / 'Game (Disc 3).cue'
        self.outside.parent.mkdir()
        self.outside.write_text('')

    def tearDown(self):
        self.tmp.cleanup()

    def test_inside_project_is_relative_posix(self):
        self.assertEqual(uds.config_disc_path(self.inside, self.project),
                         'disc/Game (Disc 2).cue')

    def test_relative_input_with_native_separator(self):
        cwd = os.getcwd()
        os.chdir(self.project)
        try:
            rel = Path('disc' + os.sep + 'Game (Disc 2).cue')
            self.assertEqual(uds.config_disc_path(rel, self.project),
                             'disc/Game (Disc 2).cue')
        finally:
            os.chdir(cwd)

    def test_outside_project_is_absolute(self):
        got = uds.config_disc_path(self.outside, self.project)
        self.assertTrue(Path(got).is_absolute())
        self.assertEqual(Path(got), self.outside.resolve())


if __name__ == '__main__':
    unittest.main()
