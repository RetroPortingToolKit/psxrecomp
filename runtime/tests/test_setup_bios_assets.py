#!/usr/bin/env python3
"""Exercise the production SDK BIOS gate with synthetic files only."""
import importlib.util
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gate", ROOT / "tools/check_setup_bios_assets.py")
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


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
        self.write("build/Fixture", "synthetic executable, never run\n")
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
                "--root", str(self.root), "--build-dir", "build", "--artifact", "fixture",
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
                stage = self.root / "dist/stage-setup-fixture"
                self.assertTrue((stage / "replay-diagnostics.py").is_file())
                self.assertTrue((stage / "psxrecomp/bios/SCPH1001.toml").is_file())
                for name in ("OpenBIOS.toml", "openbios.bin", "OpenBIOS.LICENSE"):
                    self.assertEqual((stage / "psxrecomp/bios" / name).exists(), not retail)
                readme = (stage / "README-SETUP.txt").read_text()
                if retail:
                    self.assertIn("a legal retail BIOS dump (required)", readme)
                    self.assertNotIn("otherwise OpenBIOS", readme)
                else:
                    self.assertIn("otherwise OpenBIOS is regenerated locally", readme)

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


if __name__ == "__main__":
    unittest.main()
