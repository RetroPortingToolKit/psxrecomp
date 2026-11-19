"""generate --bios builds the BIOS profile the title names, not always SCPH1001.

A PAL title declares [recompiler] bios_config = "psxrecomp/bios/SCPH5552.toml"
and its host links the SCPH5552 pair. generate --bios used to stage every dump
as bios/SCPH1001.BIN and regenerate bios/SCPH1001.toml, so psxrecomp-bios
refused the SCPH5552 image against the SCPH1001 profile's pinned hash and the
build stopped at bios-generate.
"""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_generate_bios", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)


class _Stop(Exception):
    pass


def make_project(root: Path, bios_config: str) -> Path:
    (root / "prepared").mkdir()
    (root / "prepared" / "BOOT.EXE").write_bytes(b"PS-X EXE")
    fw = root / "psxrecomp"
    (fw / "bios").mkdir(parents=True)
    (fw / ".gitignore").write_text("", encoding="utf-8")
    (fw / "bios" / "OpenBIOS.toml").write_text('[program]\nrom = "bios/openbios.bin"\n', encoding="utf-8")
    (fw / "bios" / "SCPH1001.toml").write_text('[program]\nrom = "bios/SCPH1001.BIN"\n', encoding="utf-8")
    (fw / "bios" / "SCPH5552.toml").write_text('[program]\nrom = "bios/EUR-PSX-SCPH5552.bin"\n', encoding="utf-8")
    recomp = f'[recompiler]\nbios_config = "{bios_config}"\n' if bios_config else ""
    (root / "game.toml").write_text(
        '[game]\ndisc = "missing.chd"\nexe = "prepared/BOOT.EXE"\n'
        '[prepare_disc]\nout_dir = "prepared"\nboot_exe = "BOOT.EXE"\n'
        + recomp + '[runtime]\nopenbios = false\n',
        encoding="utf-8",
    )
    dump = root / "dump.bin"
    dump.write_bytes(b"retail dump")
    return dump


def run_generate(root: Path, dump: Path):
    """Run cmd_generate up to the emitter lookup; return the regenerated profiles."""
    args = cli.build_parser().parse_args(
        ["generate", "--config", str(root / "game.toml"), "--bios", str(dump)])
    regen = Mock()
    with patch.object(cli, "activate_embedded_toolchain", Mock()), \
         patch.object(cli, "ensure_emitters", Mock()), \
         patch.object(cli, "regen_bios_profile", regen), \
         patch.object(cli, "find_emitters", Mock(side_effect=_Stop)):
        with unittest.TestCase().assertRaises(_Stop):
            cli.cmd_generate(args, Mock())
    return [call.args[1] for call in regen.call_args_list]


class GenerateBiosProfileTests(unittest.TestCase):
    def test_pal_title_builds_its_own_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_project(root, "psxrecomp/bios/SCPH5552.toml")
            self.assertEqual(run_generate(root, dump), ["bios/SCPH5552.toml"])
            staged = root / "psxrecomp" / "bios" / "EUR-PSX-SCPH5552.bin"
            self.assertEqual(staged.read_bytes(), b"retail dump")
            self.assertFalse((root / "psxrecomp" / "bios" / "SCPH1001.BIN").exists())

    def test_title_without_profile_keeps_scph1001(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_project(root, "")
            self.assertEqual(run_generate(root, dump), ["bios/SCPH1001.toml"])
            self.assertEqual((root / "psxrecomp" / "bios" / "SCPH1001.BIN").read_bytes(), b"retail dump")

    def test_openbios_profile_uses_scph1001_for_the_retail_dump(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            dump = make_project(root, "psxrecomp/bios/OpenBIOS.toml")
            self.assertEqual(run_generate(root, dump), ["bios/SCPH1001.toml"])


if __name__ == "__main__":
    unittest.main()
