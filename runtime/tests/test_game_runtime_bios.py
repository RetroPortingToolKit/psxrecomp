#!/usr/bin/env python3
"""Exercise BIOS discovery/registry and the real high-level setup policy.

Like test_game_runtime_pgxp.py, isolate the production CMake bodies from SDL
and renderer dependencies. The small target actually links and executes the
generated registry, including a synthetic custom retail backend.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "runtime/runtime.cmake").read_text(encoding="utf-8")


class GameRuntimeBiosTest(unittest.TestCase):
    def setUp(self):
        self.cmake = os.environ.get("PSX_TEST_CMAKE") or shutil.which("cmake")
        if not self.cmake:
            self.skipTest("CMake unavailable")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.project = Path(self.tmp.name)
        self.build = self.project / "build"
        (self.project / "generated").mkdir()
        (self.project / "framework/generated").mkdir(parents=True)
        registry = SOURCE[SOURCE.index("# Which recompiled BIOSes"):SOURCE.index(
            "# --- BIOS generated/ staleness check")]
        helper = re.search(r"^function\(psxrecomp_add_game_runtime\b.*?^endfunction\(\)",
                           SOURCE, re.S | re.M).group(0)
        (self.project / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.20)
project(GameRuntimeBiosProbe C)
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/framework")
set(PSXRECOMP_BIOS_STEMS FixtureRetail CACHE STRING "")
set(PSX_RECOMP_UI OFF)
''' + registry + '''
function(psxrecomp_add_runtime_target target)
    _psxrt_validate_bios()
    add_executable(${target} probe.c ${PSXRECOMP_BIOS_GENERATED})
    target_include_directories(${target} PRIVATE "''' +
            (ROOT / "runtime/include").as_posix() + '''")
endfunction()
''' + helper + '''
psxrecomp_add_game_runtime(probe ENABLE_SETUP_WIZARD
    GEN_MARKER generated/GAME_dispatch.c
    GEN_FULL_GLOB generated/GAME_full.c)
''', encoding="utf-8")
        self.probe(0)

    def probe(self, count):
        (self.project / "probe.c").write_text('''
#include "psx_bios_backend.h"
extern const PsxBiosBackend *const psx_bios_builtin_registry[];
extern const uint32_t psx_bios_builtin_registry_count;
int main(void) {
    if (psx_bios_builtin_registry_count != ''' + str(count) + '''u) return 1;
    return ''' + ("psx_bios_builtin_registry[0] == 0" if count else
                  "psx_bios_builtin_registry[0] != 0") + ''';
}
''', encoding="utf-8")

    def configure(self, *args):
        command = [self.cmake, "-S", str(self.project), "-B", str(self.build)]
        ninja = os.environ.get("PSX_TEST_NINJA") or shutil.which("ninja")
        if ninja:
            command += ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja]
        cc = os.environ.get("PSX_TEST_CC")
        if cc:
            command += ["-DCMAKE_C_COMPILER=" + cc]
        result = subprocess.run(command + list(args), capture_output=True, text=True)
        return result.returncode, result.stdout + result.stderr

    def build_and_run(self):
        result = subprocess.run([self.cmake, "--build", str(self.build),
                                 "--config", "Release", "--parallel", "1"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        suffix = ".exe" if os.name == "nt" else ""
        exe = self.build / ("probe" + suffix)
        if not exe.exists():
            exe = self.build / "Release" / ("probe" + suffix)
        self.assertEqual(subprocess.run([str(exe)]).returncode, 0)

    def game_c(self):
        for name in ("GAME_dispatch.c", "GAME_full.c"):
            (self.project / "generated" / name).write_text("/* synthetic game */\n")

    def test_clean_setup_links_empty_registry_without_caching_allowance(self):
        code, output = self.configure()
        self.assertEqual(code, 0, output)
        self.assertIn("PSXRECOMP_ALLOW_NO_BIOS:BOOL=OFF",
                      (self.build / "CMakeCache.txt").read_text())
        self.build_and_run()

    def test_full_runtime_without_backend_fails(self):
        self.game_c()
        code, output = self.configure()
        self.assertNotEqual(code, 0, output)
        self.assertIn("No recompiled BIOS backend available", output)

    def test_setup_to_full_reconfigure_is_strict(self):
        code, output = self.configure()
        self.assertEqual(code, 0, output)
        self.game_c()
        code, output = self.configure()
        self.assertNotEqual(code, 0, output)
        self.assertIn("No recompiled BIOS backend available", output)

    def test_custom_retail_backend_links_and_registers(self):
        self.game_c()
        self.probe(1)
        generated = self.project / "framework/generated"
        (generated / "FixtureRetail_full.c").write_text("/* synthetic BIOS */\n")
        (generated / "FixtureRetail_dispatch.c").write_text(
            '#include "psx_bios_backend.h"\n'
            'const PsxBiosBackend FixtureRetail_psx_bios_backend = {0};\n')
        code, output = self.configure()
        self.assertEqual(code, 0, output)
        self.assertIn("BIOS backends linked: FixtureRetail", output)
        self.build_and_run()


if __name__ == "__main__":
    unittest.main()
