#!/usr/bin/env python3
"""Configure-time backstop of runtime/psx_release_channel.cmake.

Usage: test_release_channel_cmake.py [--cmake PATH]

A release-package build must name its channel; PSX_PUBLIC_RELEASE is derived
from it. Each case configures a throwaway project that includes the module.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "runtime/psx_release_channel.cmake"
CMAKE = os.environ.get("PSX_TEST_CMAKE") or shutil.which("cmake")


class ReleaseChannelTests(unittest.TestCase):
    def setUp(self):
        if not CMAKE:
            self.skipTest("cmake not found")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = Path(self.tmp.name) / "src"
        self.src.mkdir()
        (self.src / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\nproject(channel_fixture NONE)\n"
            'include("%s")\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/result.txt" '
            '"${PSX_RELEASE_CHANNEL}|${PSX_PUBLIC_RELEASE}")\n' % MODULE.as_posix())
        self.build = Path(self.tmp.name) / "build"

    def configure(self, *defs):
        result = subprocess.run([CMAKE, "-S", str(self.src), "-B", str(self.build)] + list(defs),
                                capture_output=True, text=True)
        return result

    def result(self):
        return (self.build / "result.txt").read_text()

    def test_dev_build_has_no_channel(self):
        r = self.configure()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.result(), "|OFF")
        self.assertIn("not packageable", r.stdout)

    def test_public_channel_derives_public_release(self):
        r = self.configure("-DPSX_RELEASE_CHANNEL=public")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.result(), "public|ON")

    def test_playtest_channel(self):
        r = self.configure("-DPSX_RELEASE_CHANNEL=playtest")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.result(), "playtest|OFF")

    def test_switching_channel_on_the_same_tree(self):
        self.assertEqual(self.configure("-DPSX_RELEASE_CHANNEL=public").returncode, 0)
        r = self.configure("-DPSX_RELEASE_CHANNEL=playtest")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.result(), "playtest|OFF")

    def test_unknown_channel_fails(self):
        r = self.configure("-DPSX_RELEASE_CHANNEL=release")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("must be 'public' or 'playtest'", r.stderr)

    def test_public_release_without_channel_fails(self):
        # The pre-channel way to ask for a public package: forgetting the
        # channel must not configure.
        r = self.configure("-DPSX_PUBLIC_RELEASE=ON")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("without a release channel", r.stderr)

    def test_contradiction_fails(self):
        r = self.configure("-DPSX_RELEASE_CHANNEL=playtest", "-DPSX_PUBLIC_RELEASE=ON")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("contradicts", r.stderr)
        r = self.configure("-UPSX_PUBLIC_RELEASE", "-DPSX_RELEASE_CHANNEL=public",
                           "-DPSX_PUBLIC_RELEASE=OFF")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("contradicts", r.stderr)

    def test_consistent_explicit_value_is_accepted(self):
        r = self.configure("-DPSX_RELEASE_CHANNEL=public", "-DPSX_PUBLIC_RELEASE=ON")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.result(), "public|ON")

    def test_legacy_off_without_channel_stays_a_dev_build(self):
        # Every dev tree configured by the old option() carries OFF.
        r = self.configure("-DPSX_PUBLIC_RELEASE:BOOL=OFF")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(self.result(), "|OFF")

    def test_source_commit_must_be_full(self):
        r = self.configure("-DPSX_SOURCE_COMMIT=abc123")
        self.assertNotEqual(r.returncode, 0)
        r = self.configure("-DPSX_SOURCE_COMMIT=" + "a" * 40)
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    if "--cmake" in sys.argv:
        index = sys.argv.index("--cmake")
        CMAKE = sys.argv[index + 1]
        del sys.argv[index:index + 2]
    unittest.main()
