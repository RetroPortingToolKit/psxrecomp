#!/usr/bin/env python3
"""Exercise the production SDK BIOS gate with synthetic files only."""
import importlib.util
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


if __name__ == "__main__":
    unittest.main()
