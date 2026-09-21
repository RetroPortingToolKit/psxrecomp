"""tools/add_retail_bios.py -- retail BIOS onboarding.

Runs entirely on synthetic ROMs and a synthetic reference profile, so it needs
no BIOS dump and runs in CI. The behaviour under test is the refusal: the tool
must not emit a profile claiming SCPH-1001's address model for an image whose
kernel is not SCPH-1001's, because a profile that lies about its address model
is the exact defect this work exists to remove.
"""
import importlib.util
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

try:
    import tomllib
except ModuleNotFoundError:
    import tomli as tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "add_retail_bios.py")

ROM_BYTES = 512 * 1024
KERNEL_END = 0x18000

REFERENCE_PROFILE = '''
[program]
name         = "Sony SCPH-1001 BIOS"
id           = "SCPH-1001"
rom          = "bios/SCPH1001.BIN"
load_address = "0xBFC00000"
entry_pc     = "0xBFC00000"
text_size    = "0x80000"

[program.image]
sha256          = "deadbeef"
license         = "proprietary"
redistributable = false

[recompiler]
seeds    = "recompiler/seeds/phase2_ghidra_seeds.json"
out_dir  = "generated"
out_stem = "SCPH1001"
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

[[recompiler.address_model.copy]]
name         = "Shell"
rom_lo       = "0x1FC18000"
rom_hi       = "0x1FC43000"
ram_lo       = "0x00030000"
runtime_base = "0x80030000"
dispatch_key = "rom"
kernel_bless = false

[[recompiler.install_slots]]
ram_addr = "0x00000C88"
len      = "0x30"
resume   = "fallthrough"

[[recompiler.install_slots]]
ram_addr = "0x00006444"
len      = "0x10"
resume   = "none"

[recompiler.runtime_exports]
shell_entry_phys  = "0x00030000"
deliver_event_ret = "0x80001720"
'''


def synth_rom(path, diverge_at=None, seed=1337):
    random.seed(seed)
    data = bytearray(random.randrange(256) for _ in range(ROM_BYTES))
    if diverge_at is not None:
        random.seed(seed + 99)
        for i in range(diverge_at, ROM_BYTES):
            data[i] = random.randrange(256)
    with open(path, "wb") as handle:
        handle.write(bytes(data))


def write_corpus(path, addresses):
    payload = {
        "schema": "psxrecomp phase2 seeds",
        "source": "synthetic",
        "seed_count": len(addresses),
        "seeds": [{"address": "0x%08X" % a, "label": "f_%X" % a, "rationale": "t"}
                  for a in addresses],
    }
    with open(path, "w") as handle:
        json.dump(payload, handle, indent=2)


class Fixture:
    """A throwaway framework root the tool can write into."""

    def __init__(self, tmp, diverge_at=KERNEL_END, target_name="SCPH5501.BIN"):
        self.root = os.path.join(tmp, "fw")
        os.makedirs(os.path.join(self.root, "bios"))
        os.makedirs(os.path.join(self.root, "recompiler", "seeds"))
        os.makedirs(os.path.join(self.root, "tools"))
        for name in ("add_retail_bios.py", "gen_retail_bios_seeds.py"):
            shutil.copy(os.path.join(ROOT, "tools", name),
                        os.path.join(self.root, "tools", name))
        self.reference = os.path.join(self.root, "bios", "SCPH1001.BIN")
        self.target = os.path.join(self.root, "bios", target_name)
        synth_rom(self.reference)
        if diverge_at is not None:
            synth_rom(self.target, diverge_at=diverge_at)
        with open(os.path.join(self.root, "bios", "SCPH1001.toml"), "w") as handle:
            handle.write(REFERENCE_PROFILE)
        write_corpus(os.path.join(self.root, "recompiler", "seeds",
                                  "phase2_ghidra_seeds.json"),
                     [0xBFC00000, 0xBFC10000, 0xBFC17F00, 0xBFC20000])
        self.tool = os.path.join(self.root, "tools", "add_retail_bios.py")

    def run(self, *extra):
        return subprocess.run(
            [sys.executable, self.tool, "--dump", self.target] + list(extra),
            capture_output=True, text=True, cwd=self.root)

    def path(self, *parts):
        return os.path.join(self.root, *parts)


class RefusalTests(unittest.TestCase):
    def test_kernel_divergence_is_refused_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp, diverge_at=0x10000)
            res = fx.run("--id", "SCPH-5501")
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("0x10000", res.stderr)
            self.assertIn("kernel copy window", res.stderr)
            self.assertIn("do NOT hold", res.stderr)
            self.assertFalse(os.path.exists(fx.path("bios", "SCPH5501.toml")))
            self.assertFalse(os.path.exists(fx.path(
                "recompiler", "seeds", "phase2_ghidra_seeds_SCPH5501.json")))

    def test_the_reference_image_itself_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp, diverge_at=None)
            shutil.copy(fx.reference, fx.target)
            res = fx.run("--id", "SCPH-5501")
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("IS the reference", res.stderr)

    def test_size_mismatch_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp, diverge_at=None)
            with open(fx.target, "wb") as handle:
                handle.write(b"\x00" * 4096)
            res = fx.run("--id", "SCPH-5501")
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("not the same class", res.stderr)

    def test_existing_outputs_need_force(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            self.assertEqual(fx.run("--id", "SCPH-5501").returncode, 0)
            again = fx.run("--id", "SCPH-5501")
            self.assertNotEqual(again.returncode, 0)
            self.assertIn("--force", again.stderr)
            self.assertEqual(fx.run("--id", "SCPH-5501", "--force").returncode, 0)


class CarriedRegionTests(unittest.TestCase):
    """Divergence OUTSIDE the carried regions is allowed for a NEW backend.

    SCPH-5500 is the real case: nine extra instructions in its reset stub, at
    ROM 0x24..0x47, which no carried value describes. That code is compiled
    from its own bytes into its own backend, so refusing it would be wrong --
    while accepting it INTO the shared backend would be wrong too, because
    there it would run the reference's compiled reset stub.
    """

    def test_boot_stub_divergence_is_allowed_and_recorded(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp, diverge_at=KERNEL_END, target_name="SCPH5500.BIN")
            # Perturb the reset stub only: before the kernel copy window and
            # clear of the LoadRunShell probe.
            with open(fx.target, "r+b") as handle:
                handle.seek(0x24)
                handle.write(b"\x11\x22\x33\x44")
            res = fx.run("--id", "SCPH-5500")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn("boot stub", res.stdout)
            self.assertIn("0x00024", res.stdout)
            with open(fx.path("bios", "SCPH5500.toml")) as handle:
                text = handle.read()
            self.assertIn("DIFFERS FROM THE REFERENCE OUTSIDE", text)
            self.assertIn("0x00024", text)

    def test_loadrunshell_divergence_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp, diverge_at=KERNEL_END, target_name="SCPH9000.BIN")
            with open(fx.target, "r+b") as handle:
                handle.seek(0x6F80)
                handle.write(b"\xDE\xAD\xBE\xEF")
            res = fx.run("--id", "SCPH-9000")
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("LoadRunShell", res.stderr)
            self.assertFalse(os.path.exists(fx.path("bios", "SCPH9000.toml")))


class EmissionTests(unittest.TestCase):
    def test_emits_a_profile_that_pins_identity_and_carries_the_model(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            res = fx.run("--id", "SCPH-5501", "--name", "Sony SCPH-5501 BIOS",
                         "--note", "NTSC-U, v3.0")
            self.assertEqual(res.returncode, 0, res.stderr)

            with open(fx.path("bios", "SCPH5501.toml"), "rb") as handle:
                profile = tomllib.load(handle)

            self.assertEqual(profile["program"]["id"], "SCPH-5501")
            self.assertEqual(profile["program"]["rom"], "bios/SCPH5501.BIN")
            # The identity pin is the whole point: without it the emitter's
            # declared-identity gate cannot refuse a wrong revision.
            self.assertEqual(len(profile["program"]["image"]["sha256"]), 64)
            self.assertFalse(profile["program"]["image"]["redistributable"])

            recomp = profile["recompiler"]
            self.assertEqual(recomp["out_stem"], "SCPH5501")
            self.assertEqual(recomp["seeds"],
                             "recompiler/seeds/phase2_ghidra_seeds_SCPH5501.json")
            # Carried from the reference, not invented.
            copies = recomp["address_model"]["copy"]
            self.assertEqual([c["name"] for c in copies], ["Kernel Part 2", "Shell"])
            self.assertTrue(copies[0]["kernel_bless"])
            self.assertEqual(copies[1]["rom_hi"], "0x1FC43000")
            self.assertEqual(len(recomp["install_slots"]), 2)
            self.assertEqual(recomp["runtime_exports"]["shell_entry_phys"], "0x00030000")
            self.assertEqual(recomp["runtime_exports"]["deliver_event_ret"], "0x80001720")

    def test_profile_records_the_evidence_and_its_limit(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            self.assertEqual(fx.run("--id", "SCPH-5501").returncode, 0)
            with open(fx.path("bios", "SCPH5501.toml")) as handle:
                text = handle.read()
            self.assertIn("IDENTICAL", text)
            self.assertIn("EVIDENCE LIMIT", text)
            self.assertIn("NOT re-measured", text)

    def test_seed_corpus_drops_the_diverging_region(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            self.assertEqual(fx.run("--id", "SCPH-5501").returncode, 0)
            with open(fx.path("recompiler", "seeds",
                              "phase2_ghidra_seeds_SCPH5501.json")) as handle:
                kept = [s["address"] for s in json.load(handle)["seeds"]]
            self.assertEqual(kept, ["0xBFC00000", "0xBFC10000", "0xBFC17F00"])

    def test_prints_the_known_images_row_but_does_not_edit_the_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp)
            res = fx.run("--id", "SCPH-5501")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn('{ "SCPH5501", "SCPH-5501", 0x', res.stdout)
            self.assertIn("524288u }", res.stdout)
            self.assertFalse(os.path.exists(
                fx.path("runtime", "include", "psx_bios_known_images.h")))

    def test_stem_comes_from_a_region_qualified_filename(self):
        with tempfile.TemporaryDirectory() as tmp:
            fx = Fixture(tmp, target_name="EUR-PSX-SCPH5502.bin")
            res = fx.run("--id", "SCPH-5502")
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertTrue(os.path.exists(fx.path("bios", "SCPH5502.toml")))


if __name__ == "__main__":
    unittest.main()
