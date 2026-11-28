#!/usr/bin/env python3
"""PS1B-381: the packager's setup host lookup prefers the name with .exe.

Under Git Bash and MSYS2, [[ -f X ]] is also true when only X.exe exists. With
the bare name first, a Windows build settled on the name without .exe and the
packager's "*.exe" branches (sibling DLLs, signing, readme wording) never ran.
This runs the packager's own lookup block on build folders that hold both
spellings (what MSYS2 reports for a Windows build) and only the bare name (a
Linux or macOS build).
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

PACKAGER = Path(__file__).resolve().parents[1] / "package_setup_host.sh"
BEGIN = "# --- Setup host lookup"
END = "# --- End setup host lookup"


def lookup_block() -> str:
    text = PACKAGER.read_text(encoding="utf-8")
    block = text.split(BEGIN, 1)[1].split(END, 1)[0]
    return block.split("\n", 1)[1]


class HostLookup(unittest.TestCase):
    def run_lookup(self, files):
        bash = shutil.which("bash")
        if bash is None:
            self.skipTest("bash is required to run the actual lookup")
        with tempfile.TemporaryDirectory() as tmp:
            build = Path(tmp) / "build"
            for name in files:
                path = build / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"host")
            script = Path(tmp) / "lookup.sh"
            with script.open("w", encoding="utf-8", newline="\n") as out:
                out.write('set -euo pipefail\nBUILD_DIR="$1"\nEXE_NAME=game-setup\n')
                out.write(lookup_block())
                out.write('basename "${EXE}"\n')
            done = subprocess.run(
                [bash, script.as_posix(), build.as_posix()],
                env=dict(os.environ), capture_output=True, text=True,
                encoding="utf-8", errors="replace")
            self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
            return done.stdout.strip()

    def test_windows_build_settles_on_the_exe(self):
        self.assertEqual(self.run_lookup(["game-setup", "game-setup.exe"]), "game-setup.exe")

    def test_release_subfolder_exe_beats_a_bare_name(self):
        self.assertEqual(self.run_lookup(["game-setup", "Release/game-setup.exe"]),
                         "game-setup.exe")

    def test_unix_build_keeps_the_bare_name(self):
        self.assertEqual(self.run_lookup(["game-setup"]), "game-setup")

    def test_the_block_is_the_real_lookup(self):
        block = lookup_block()
        self.assertIn('"${BUILD_DIR}/${EXE_NAME}.exe"', block)
        self.assertLess(block.index('${EXE_NAME}.exe'), block.index('"${BUILD_DIR}/${EXE_NAME}"'))


if __name__ == "__main__":
    unittest.main()
