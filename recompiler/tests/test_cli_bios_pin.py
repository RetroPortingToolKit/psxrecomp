"""psxrecomp_cli.py: [recompiler] bios_config is the pin every consumer reads.

The stage filename, the regenerated profile and the linked backend list all
come from one value in game.toml. Before, all three were the literal SCPH1001,
so a title pinning another profile generated a backend nothing linked, and the
wizard staged every dump as SCPH1001.BIN and then failed the emitter's
declared-identity gate on the sha256 pin.

Synthetic framework trees only — no BIOS dump, no build.
"""
import importlib.util
import os
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
spec = importlib.util.spec_from_file_location("psxrecomp_cli_pin", ROOT / "psxrecomp_cli.py")
cli = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = cli
spec.loader.exec_module(cli)


PROFILE = '''
[program]
name         = "Sony {id} BIOS"
id           = "{id}"
rom          = "bios/{rom}"
load_address = "0xBFC00000"
entry_pc     = "0xBFC00000"
text_size    = "0x80000"

[program.image]
sha256          = "{sha}"
license         = "proprietary"
redistributable = false

[recompiler]
seeds    = "recompiler/seeds/phase2_ghidra_seeds_{stem}.json"
out_dir  = "generated"
out_stem = "{stem}"
strict   = true

[recompiler.address_model]
normalize_mask = "0x1FFFFFFF"

[[recompiler.address_model.copy]]
name         = "Kernel Part 2"
rom_lo       = "0x1FC10000"
rom_hi       = "0x1FC18000"
ram_lo       = "0x00000500"
runtime_base = "0x00000500"
dispatch_key = "ram"
kernel_bless = true
'''


def make_project(tmp, bios_config=None, profiles=("SCPH1001",)):
    project = Path(tmp) / "GameRecomp"
    fw = project / "psxrecomp"
    (fw / "bios").mkdir(parents=True)
    (fw / "recompiler").mkdir(parents=True)
    for stem in profiles:
        rom = "US-PSX-%s.BIN" % stem if stem == "SCPH5501" else "%s.BIN" % stem
        (fw / "bios" / f"{stem}.toml").write_text(
            PROFILE.format(id=stem.replace("SCPH", "SCPH-"), rom=rom,
                           stem=stem, sha="0" * 64),
            encoding="utf-8")
    (fw / "bios" / "OpenBIOS.toml").write_text("[program]\nname=\"ob\"\n", encoding="utf-8")
    lines = ["[game]", 'name = "Game"', "", "[recompiler]", 'seeds = "seeds/f.txt"']
    if bios_config:
        lines.append(f'bios_config = "{bios_config}"')
    (project / "game.toml").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return project


def recomp_section(project):
    return cli.parse_toml_simple(
        (project / "game.toml").read_text(encoding="utf-8")).get("recompiler") or {}


class PinResolutionTests(unittest.TestCase):
    def test_absent_pin_keeps_the_historical_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = make_project(tmp)
            pin = cli.resolve_pinned_bios(project, recomp_section(project))
            self.assertEqual(pin.stem, "SCPH1001")
            self.assertEqual(pin.profile_rel, "bios/SCPH1001.toml")
            self.assertEqual(pin.rom_name, "SCPH1001.BIN")
            self.assertEqual(pin.image_id, "SCPH-1001")
            # The reference image's pin, used to verify a dump BEFORE staging
            # it over the file the emitter will read.
            self.assertEqual(pin.sha256, "0" * 64)

    def test_project_relative_pin(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = make_project(tmp, "psxrecomp/bios/SCPH5501.toml",
                                   profiles=("SCPH1001", "SCPH5501"))
            pin = cli.resolve_pinned_bios(project, recomp_section(project))
            self.assertEqual(pin.stem, "SCPH5501")
            self.assertEqual(pin.profile_rel, "bios/SCPH5501.toml")
            self.assertEqual(pin.image_id, "SCPH-5501")

    def test_framework_relative_pin(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = make_project(tmp, "bios/SCPH5501.toml",
                                   profiles=("SCPH1001", "SCPH5501"))
            pin = cli.resolve_pinned_bios(project, recomp_section(project))
            self.assertEqual(pin.stem, "SCPH5501")

    def test_region_qualified_rom_name_is_carried_verbatim(self):
        # The stage filename must be what the profile's [program] rom expects,
        # or the emitter cannot open the ROM it was told to read.
        with tempfile.TemporaryDirectory() as tmp:
            project = make_project(tmp, "bios/SCPH5501.toml",
                                   profiles=("SCPH1001", "SCPH5501"))
            pin = cli.resolve_pinned_bios(project, recomp_section(project))
            self.assertEqual(pin.rom_name, "US-PSX-SCPH5501.BIN")

    def test_a_pin_naming_no_file_is_a_loud_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            project = make_project(tmp, "bios/SCPH9999.toml")
            with self.assertRaises(RuntimeError) as caught:
                cli.resolve_pinned_bios(project, recomp_section(project))
            self.assertIn("SCPH9999", str(caught.exception))


class StemListTests(unittest.TestCase):
    def _pin(self, stem):
        return cli.PinnedBios(profile_rel=f"bios/{stem}.toml", stem=stem,
                              rom_name=f"{stem}.BIN", image_id=stem,
                              sha256="0" * 64)

    def test_openbios_leads_when_the_title_allows_it(self):
        # runtime.cmake treats entry 0 as primary and the bundled backend is
        # the fallback for a player with no dump.
        self.assertEqual(
            cli.pinned_bios_stems(self._pin("SCPH5501"), True), "OpenBIOS;SCPH5501")

    def test_retail_only_when_the_title_forbids_openbios(self):
        self.assertEqual(
            cli.pinned_bios_stems(self._pin("SCPH5501"), False), "SCPH5501")

    def test_an_openbios_pin_is_not_duplicated(self):
        self.assertEqual(
            cli.pinned_bios_stems(self._pin("OpenBIOS"), True), "OpenBIOS")


if __name__ == "__main__":
    unittest.main()
