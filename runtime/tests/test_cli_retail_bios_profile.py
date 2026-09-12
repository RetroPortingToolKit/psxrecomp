"""CLI half of the BIOS setup contract (registered by PR #343)."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_under_test", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)


class RetailBiosProfileTests(unittest.TestCase):
    def test_pairs_require_matching_descriptor(self):
        with tempfile.TemporaryDirectory() as temporary:
            fw = Path(temporary)
            generated = fw / "generated"
            generated.mkdir()
            for stem in ("OpenBIOS", "SCPH1001", "SCPH5552"):
                with self.subTest(stem=stem):
                    dispatch = generated / f"{stem}_dispatch.c"
                    full = generated / f"{stem}_full.c"
                    self.assertFalse(cli.bios_backend_present(fw, stem))
                    dispatch.write_text(f"const PsxBiosBackend {stem}_psx_bios_backend;", encoding="utf-8")
                    self.assertFalse(cli.bios_backend_present(fw, stem))
                    full.write_text("", encoding="utf-8")
                    self.assertTrue(cli.bios_backend_present(fw, stem))
                    dispatch.write_text("const PsxBiosBackend unrelated_psx_bios_backend;", encoding="utf-8")
                    self.assertFalse(cli.bios_backend_present(fw, stem))

    def test_profile_and_utf8_are_forwarded_and_failures_propagate(self):
        with tempfile.TemporaryDirectory() as temporary:
            fw = Path(temporary)
            (fw / "bios").mkdir()
            (fw / "bios/SCPH5552.toml").write_text('[program]\nid="SCPH-5552"\n', encoding="utf-8")
            progress = Mock()
            with patch.object(cli, "framework_root", return_value=fw), \
                 patch.object(cli, "find_psxrecomp_bios", return_value=fw / "bios-tool"), \
                 patch.object(cli.subprocess, "run") as run:
                run.return_value = subprocess.CompletedProcess([], 0, "BIOS → generated", "")
                cli.regen_bios_profile(fw, "bios/SCPH5552.toml", progress=progress)
                args, kwargs = run.call_args
                self.assertEqual(args[0][1:], ["--config", "bios/SCPH5552.toml"])
                self.assertEqual(kwargs["cwd"], str(fw))
                self.assertEqual(kwargs["encoding"], "utf-8")
                self.assertEqual(kwargs["errors"], "replace")
                run.return_value = subprocess.CompletedProcess([], 2, "", "wrong image")
                with self.assertRaisesRegex(RuntimeError, "SCPH5552.*exit 2"):
                    cli.regen_bios_profile(fw, "bios/SCPH5552.toml", progress=progress)


if __name__ == "__main__":
    unittest.main()
