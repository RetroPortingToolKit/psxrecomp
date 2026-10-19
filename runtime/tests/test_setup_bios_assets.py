#!/usr/bin/env python3
"""Exercise the production SDK BIOS gate with synthetic files only."""
import importlib.util
import ast
import types
import os
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock
import zipfile
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gate", ROOT / "tools/check_setup_bios_assets.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
spec = importlib.util.spec_from_file_location("platform_copy", ROOT / "tools/ci/audit_setup_package_platform_copy.py")
platform_copy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(platform_copy)
spec = importlib.util.spec_from_file_location("toolchain_pack", ROOT / "tools/toolchain_pack.py")
toolchain_pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(toolchain_pack)


class GateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / "psxrecomp/bios").mkdir(parents=True)

    def recipe(self, text):
        (self.root / "game.toml").write_text(text)

    def asset(self, name):
        (self.root / "psxrecomp/bios" / name).write_text("synthetic fixture")

    def test_retail_profile_without_openbios(self):
        self.recipe('[runtime]\nopenbios=false\n[recompiler]\nbios_config="psxrecomp/bios/SCPH5552.toml"\n')
        self.asset("SCPH5552.toml")
        self.assertEqual(gate.check(self.root), [str(Path("psxrecomp/bios/SCPH5552.toml"))])

    def test_omit_requires_retail_recipe_and_no_openbios_assets(self):
        self.recipe('[runtime]\nopenbios=true\n')
        with self.assertRaisesRegex(ValueError, "requires runtime.openbios=false"):
            gate.check(self.root, omit_openbios=True)
        self.recipe('[runtime]\nopenbios=false\n')
        self.asset("SCPH1001.toml")
        self.assertEqual(len(gate.check(self.root, omit_openbios=True)), 1)
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            self.asset(name)
            with self.assertRaisesRegex(ValueError, "survived"):
                gate.check(self.root, omit_openbios=True)
            (self.root / "psxrecomp/bios" / name).unlink()

    def test_retail_missing_profile_fails(self):
        self.recipe('[runtime]\nopenbios=false\n[recompiler]\nbios_config="psxrecomp/bios/SCPH5552.toml"\n')
        with self.assertRaisesRegex(ValueError, "SCPH5552"):
            gate.check(self.root)

    def test_default_openbios_requires_all_assets(self):
        self.recipe('[runtime]\n')
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            with self.assertRaises(ValueError):
                gate.check(self.root)
            self.asset(name)
        self.assertEqual(len(gate.check(self.root)), 3)

    def test_external_profile_fails(self):
        self.recipe('[runtime]\nopenbios=false\n[recompiler]\nbios_config="../external.toml"\n')
        with self.assertRaisesRegex(ValueError, "inside"):
            gate.check(self.root)

    def test_absent_recipe_fails(self):
        with self.assertRaisesRegex(ValueError, "game.toml"):
            gate.check(self.root)

    def symlink(self, link, target, directory=False):
        try:
            link.symlink_to(target, target_is_directory=directory)
        except (OSError, NotImplementedError) as error:
            self.skipTest("symlink creation unavailable: " + str(error))

    def outside(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return Path(tmp.name)

    def test_default_retail_rejects_external_profile_symlink(self):
        self.recipe('[runtime]\nopenbios=false\n')
        target = self.outside() / "SCPH1001.toml"
        target.write_text("synthetic fixture")
        self.symlink(self.root / "psxrecomp/bios/SCPH1001.toml", target)
        with self.assertRaisesRegex(ValueError, "inside"):
            gate.check(self.root)

    def test_openbios_rejects_external_asset_symlinks(self):
        self.recipe('[runtime]\n')
        names = ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE")
        for name in names:
            self.asset(name)
        target = self.outside() / "asset"
        target.write_text("synthetic fixture")
        for name in names:
            with self.subTest(asset=name):
                link = self.root / "psxrecomp/bios" / name
                link.unlink()
                self.symlink(link, target)
                with self.assertRaisesRegex(ValueError, "inside"):
                    gate.check(self.root)
                link.unlink()
                self.asset(name)

    def test_default_retail_rejects_external_bios_directory(self):
        self.recipe('[runtime]\nopenbios=false\n')
        target = self.outside()
        (target / "SCPH1001.toml").write_text("synthetic fixture")
        link = self.root / "psxrecomp/bios"
        link.rmdir()
        self.symlink(link, target, directory=True)
        with self.assertRaisesRegex(ValueError, "inside"):
            gate.check(self.root)

    def test_openbios_rejects_external_framework_directory(self):
        self.recipe('[runtime]\n')
        target = self.outside()
        (target / "bios").mkdir()
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            (target / "bios" / name).write_text("synthetic fixture")
        (self.root / "psxrecomp/bios").rmdir()
        link = self.root / "psxrecomp"
        link.rmdir()
        self.symlink(link, target, directory=True)
        with self.assertRaisesRegex(ValueError, "inside"):
            gate.check(self.root)

    def test_internal_asset_symlinks_are_accepted(self):
        self.recipe('[runtime]\n')
        target = self.root / "shared-asset"
        target.write_text("synthetic fixture")
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            self.symlink(self.root / "psxrecomp/bios" / name, target)
        self.assertEqual(len(gate.check(self.root)), 3)


@unittest.skipIf(os.name == "nt" or not shutil.which("bash"),
                 "full shell packaging fixture requires POSIX bash")
class SetupPackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        for path in ("build/assets/fonts/font.txt", "build/assets/img/image.txt",
                     "build/mods/bundled/fixture/1.0.0/manifest.toml",
                     "mods/preloaded/packages/fixture/1.0.0/manifest.toml",
                     "psxrecomp/psxrecomp_cli.py", "recomp-ui/CMakeLists.txt",
                     "emitters/psxrecomp-game", "emitters/psxrecomp-bios"):
            self.write(path, "synthetic fixture\n")
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE", "SCPH1001.toml"):
            self.write("psxrecomp/bios/" + name, "synthetic BIOS fixture")
        self.write("CMakeLists.txt", "# synthetic title\n")
        self.write("VERSION", "0.0.1\n")
        self.write("build/psx_game_version.txt", "0.0.1\n")
        self.write("build/Fixture", "synthetic executable, never run\n"
                   "1. Native build tools\n"
                   "Install CMake, Ninja, Python 3, and either Clang or GCC\nCheck tools\n")
        self.write("build/replay-diagnostics.py", "# staged replay helper\n")
        # Packaging tests exercise staging, not a system compression utility.
        # Ubuntu developer images may omit zip; write a real ZIP with stdlib.
        self.write("fixture-bin/zip", '''#!/usr/bin/env python3
import pathlib, sys, zipfile
with zipfile.ZipFile(sys.argv[-2], "w") as archive:
    for path in pathlib.Path(".").rglob("*"):
        if path.is_file():
            archive.write(path, path.as_posix())
''')
        (self.root / "fixture-bin/zip").chmod(0o755)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def package(self, retail):
        self.write("game.toml", "[runtime]\noverlay_cache=true\nopenbios=" +
                   ("false" if retail else "true") + "\n")
        args = ["bash", str(ROOT / "tools/package_setup_host.sh"),
                "--root", str(self.root), "--build-dir", "build", "--artifact", "linux-x64",
                "--zip-prefix", "test", "--exe-name", "Fixture", "--recompiler-build", "emitters",
                "--project-file", "CMakeLists.txt", "--project-file", "game.toml"]
        if retail:
            args += ["--omit-openbios", "--bios-hint", "a legal retail BIOS dump (required)"]
        env = dict(os.environ, RELEASE_VERSION="0.0.1", EXCLUDE_DEV_MODS="0")
        env["PATH"] = str(self.root / "fixture-bin") + os.pathsep + env["PATH"]
        return subprocess.run(args, cwd=self.root, env=env, capture_output=True, text=True)

    def test_default_and_retail_packages_use_real_sdk_gate_and_keep_helper(self):
        for retail in (False, True):
            with self.subTest(retail=retail):
                result = self.package(retail)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                stage = self.root / "dist/stage-setup-linux-x64"
                self.assertTrue((stage / "replay-diagnostics.py").is_file())
                self.assertTrue((stage / "psxrecomp/bios/SCPH1001.toml").is_file())
                for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
                    self.assertEqual((stage / "psxrecomp/bios" / name).exists(), not retail)
                readme = (stage / "README-SETUP.txt").read_text()
                self.assertIn("Install CMake, Ninja, Python 3", readme)
                self.assertIn("native build tools", readme)
                self.assertNotIn("cmake-clang-v1", readme)
                self.assertEqual(platform_copy.audit(self.root / "dist/test-0.0.1-linux-x64.zip")["status"], "pass")
                if retail:
                    self.assertIn("a legal retail BIOS dump (required)", readme)
                    self.assertNotIn("otherwise OpenBIOS", readme)
                else:
                    self.assertIn("otherwise OpenBIOS is regenerated locally", readme)

    def test_sdk_omits_ui_state_and_demo_rom_but_keeps_build_sources(self):
        self.write("recomp-ui/keybinds.ini", "[player1]\ncross=X\n")
        self.write("recomp-ui/test_data/msu1_demo_vanilla.rom", "synthetic demo ROM")
        self.write("recomp-ui/src/common/required.h", "/* required build input */\n")
        result = self.package(retail=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        with zipfile.ZipFile(self.root / "dist/test-0.0.1-linux-x64.zip") as archive:
            names = archive.namelist()
            self.assertNotIn("recomp-ui/keybinds.ini", names)
            self.assertFalse(any(name.startswith("recomp-ui/test_data/") for name in names))
            self.assertEqual(archive.read("recomp-ui/src/common/required.h"),
                             b"/* required build input */\n")

    def test_sdk_omission_runs_after_overlay_and_rejects_openbios_recipe(self):
        for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
            self.write("overlay/bios/" + name, "overlay BIOS fixture")
        self.write("game.toml", "[runtime]\nopenbios=false\n")
        args = ["bash", str(ROOT / "tools/stage_setup_sdk.sh"), "--stage", str(self.root),
                "--framework", str(self.root / "psxrecomp"), "--sdk-overlay", str(self.root / "overlay"),
                "--recompiler-build", str(self.root / "emitters"), "--allow-no-toolchain", "--omit-openbios"]
        result = subprocess.run(args, cwd=self.root, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / "psxrecomp/bios/openbios.bin").exists())
        self.write("game.toml", "[runtime]\nopenbios=true\n")
        result = subprocess.run(args, cwd=self.root, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires runtime.openbios=false", result.stderr)


class RuntimeBiosStagingTests(unittest.TestCase):
    def test_actual_runtime_staging_requires_only_linked_openbios_assets(self):
        cmake = os.environ.get("PSX_TEST_CMAKE") or shutil.which("cmake")
        if not cmake:
            self.skipTest("CMake is unavailable")
        source = (ROOT / "runtime/runtime.cmake").read_text(encoding="utf-8")
        start = source.index("    # OpenBIOS is part of the native runtime product")
        end = source.index("    # Mod catalog:", start)
        # Exercise the production block, closing its outer non-oracle scope
        # before the unrelated mod catalog code. The tiny probe is never run.
        block = source[start:end] + "\nendif()\n"
        cases = (
            ("linked", "OpenBIOS;SCPH1001", True, True, False, None, True),
            ("retail", "SCPH1001", False, False, False, None, False),
            ("missing-rom", "OpenBIOS;SCPH1001", False, True, False,
             "Bundled OpenBIOS image is missing", False),
            ("missing-license", "OpenBIOS", True, False, False,
             "Bundled OpenBIOS license is missing", False),
            ("oracle", "OpenBIOS", False, False, True, None, False),
        )
        with tempfile.TemporaryDirectory() as tmp:
            for name, linked, image, license, oracle, error, staged in cases:
                with self.subTest(case=name):
                    project = Path(tmp) / name
                    project.mkdir()
                    if image:
                        (project / "openbios.bin").write_bytes(b"synthetic ROM")
                    if license:
                        (project / "OpenBIOS.LICENSE").write_text("synthetic MIT notice")
                    (project / "probe.c").write_text("int main(void) { return 0; }\n")
                    (project / "CMakeLists.txt").write_text(
                        'cmake_minimum_required(VERSION 3.20)\n'
                        'project(BiosStageProbe C)\nadd_executable(probe probe.c)\n'
                        'set(target probe)\n'
                        'set(PSXRECOMP_BIOS_STEMS "OpenBIOS;SCPH1001")\n'
                        'set(_psxrt_bios_linked "' + linked + '")\n'
                        'set(PSXRT_ORACLE ' + ("TRUE" if oracle else "FALSE") + ')\n'
                        'set(PSXRECOMP_BUNDLED_BIOS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/openbios.bin")\n'
                        'set(PSXRECOMP_BUNDLED_BIOS_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/OpenBIOS.LICENSE")\n'
                        'set(PSXRECOMP_BUNDLED_BIOS_PATH "bios/openbios.bin")\n' + block)
                    build = project / "build"
                    command = [cmake, "-S", str(project), "-B", str(build)]
                    ninja = os.environ.get("PSX_TEST_NINJA") or shutil.which("ninja")
                    if ninja:
                        command += ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + ninja]
                    if os.environ.get("PSX_TEST_CC"):
                        command += ["-DCMAKE_C_COMPILER=" + os.environ["PSX_TEST_CC"]]
                    configure = subprocess.run(command, capture_output=True, text=True)
                    output = configure.stdout + configure.stderr
                    if error:
                        self.assertNotEqual(configure.returncode, 0, output)
                        self.assertIn(error, output)
                        continue
                    self.assertEqual(configure.returncode, 0, output)
                    compile_probe = subprocess.run(
                        [cmake, "--build", str(build), "--parallel", "1"],
                        capture_output=True, text=True)
                    self.assertEqual(compile_probe.returncode, 0,
                                     compile_probe.stdout + compile_probe.stderr)
                    self.assertEqual((build / "bios/openbios.bin").exists(), staged)
                    self.assertEqual((build / "bios/OpenBIOS.LICENSE").exists(), staged)
                    if staged:
                        self.assertEqual((build / "bios/openbios.bin").read_bytes(), b"synthetic ROM")
                        self.assertEqual((build / "bios/OpenBIOS.LICENSE").read_text(), "synthetic MIT notice")


class PlatformCopyTests(unittest.TestCase):
    def test_gate_requires_real_platform_copy_and_controls(self):
        with tempfile.TemporaryDirectory() as tmp:
            for platform in ("windows-x64", "linux-x64", "macos-arm64", "macos-x64"):
                windows = platform.startswith("windows")
                host = "Fixture.exe" if windows else "Fixture"
                readme = "1. Run {}.\n2. Provide a legal retail BIOS (required).\n".format(host)
                if windows:
                    readme += "downloads cmake-clang-v1 toolchain pack\n"
                    controls = "Download latest portable toolchain Select cmake-clang-v1 toolchain zip cmake-clang-v1"
                else:
                    readme += "Install CMake, Ninja, Python 3 and a C/C++ compiler; available on PATH; native build tools\n"
                    controls = "1. Native build tools Install CMake, Ninja, Python 3, and either Clang or GCC Check tools"
                archive = Path(tmp) / ("setup-" + platform + ".zip")

                def write(extra=""):
                    with zipfile.ZipFile(archive, "w") as output:
                        output.writestr("README-SETUP.txt", readme)
                        output.writestr(host, controls + extra)

                write()
                self.assertEqual(platform_copy.audit(archive)["status"], "pass")
                if windows:
                    controls = controls.replace("Select cmake-clang-v1 toolchain zip", "")
                    write()
                    with self.assertRaisesRegex(ValueError, "Select cmake-clang-v1 toolchain zip"):
                        platform_copy.audit(archive)
                else:
                    write("1. Portable toolchain")
                    with self.assertRaisesRegex(ValueError, "forbidden"):
                        platform_copy.audit(archive)


class NativeToolchainTests(unittest.TestCase):
    def test_cli_does_not_activate_cached_packs_on_posix(self):
        tree = ast.parse((ROOT / "psxrecomp_cli.py").read_text(encoding="utf-8"))
        function = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
                        and node.name == "activate_embedded_toolchain")
        function.returns = None
        for argument in function.args.args:
            argument.annotation = None
        namespace = {"sys": types.SimpleNamespace(platform="linux"),
                     "resolve_toolchain_bin": mock.Mock(side_effect=AssertionError("cached pack"))}
        exec(compile(ast.Module(body=[function], type_ignores=[]), "cli_activation", "exec"), namespace)
        self.assertFalse(namespace[function.name](Path(".")))
        namespace["resolve_toolchain_bin"].assert_not_called()

    def test_host_native_guards_precede_cache_and_download_paths(self):
        source = (ROOT / "host/psxrecomp_codegen_host.c").read_text(encoding="utf-8")
        for start, native, portable in (
                ("static void activate_toolchain_path(void)", "return;", "if (!resolve_toolchain_bin"),
                ("static int host_toolchain_is_ready(void)", "return host_system_toolchain_ready();", "migrate_legacy_psxrecomp_toolchain();"),
                ("static int host_ensure_toolchain_with_progress(", "if (host_system_toolchain_ready())", "migrate_legacy_psxrecomp_toolchain();")):
            body = source[source.index(start):]
            self.assertLess(body.index("#if !defined(_WIN32)"), body.index(native))
            self.assertLess(body.index(native), body.index(portable))
        body = source[source.index("static int host_system_toolchain_ready(void)"):
                      source.index("/* True when pack bin/ can compile+link")]
        for tool in ("cmake", "ninja", "python3", "cc", "c++"):
            self.assertIn('find_on_path("' + tool + '"', body)
        self.assertIn("xcode-select -p", body)

    def test_posix_uses_native_probes_and_never_touches_pack_cache_or_network(self):
        for platform in ("linux", "darwin"):
            with self.subTest(platform=platform), \
                 mock.patch.object(toolchain_pack.sys, "platform", platform), \
                 mock.patch.object(toolchain_pack.shutil, "which", side_effect=lambda name: "/native/bin/" + name), \
                 mock.patch.object(toolchain_pack.subprocess, "run", return_value=types.SimpleNamespace(returncode=0)) as probe, \
                 mock.patch.object(toolchain_pack, "migrate_legacy_psxrecomp_cache", side_effect=AssertionError("touched cache")):
                self.assertEqual(toolchain_pack.ensure_toolchain(download=True), Path("/native/bin"))
                self.assertEqual(len(probe.call_args_list), 5)
                self.assertTrue(all(call.args[0][-1] == "--version" for call in probe.call_args_list))

    def test_posix_missing_and_broken_tools_fail_without_download(self):
        for broken in (False, True):
            with self.subTest(broken=broken), \
                 mock.patch.object(toolchain_pack, "sys_platform_is_windows", return_value=False), \
                 mock.patch.object(toolchain_pack.shutil, "which", return_value="/native/cmake" if broken else None), \
                 mock.patch.object(toolchain_pack.subprocess, "run", return_value=types.SimpleNamespace(returncode=1)), \
                 mock.patch.object(toolchain_pack, "migrate_legacy_psxrecomp_cache", side_effect=AssertionError("touched cache")):
                with self.assertRaisesRegex(RuntimeError, "package manager"):
                    toolchain_pack.ensure_toolchain(download=True)

    def test_windows_retains_existing_portable_pack_activation(self):
        binary = Path("fixture-pack/bin")
        with mock.patch.object(toolchain_pack, "sys_platform_is_windows", return_value=True), \
             mock.patch.object(toolchain_pack, "is_windows_store_python", return_value=False), \
             mock.patch.object(toolchain_pack, "migrate_legacy_psxrecomp_cache"), \
             mock.patch.object(toolchain_pack, "resolve_toolchain_bin", return_value=binary), \
             mock.patch.object(toolchain_pack, "toolchain_bin_runs", return_value=True), \
             mock.patch.object(toolchain_pack, "register_toolchain_user_env"), \
             mock.patch.object(toolchain_pack, "activate_toolchain_bin") as activate:
            self.assertEqual(toolchain_pack.ensure_toolchain(download=True), binary)
            activate.assert_called_once_with(binary, log=None)


if __name__ == "__main__":
    unittest.main()
