"""tools/gen_retail_bios_seeds.py -- the retail seed filter rebuilt after f55c731c.

The core cases run on synthetic ROMs and need no BIOS dump, so CI covers them.
The reproduction cases need bios/SCPH1001.BIN and skip without it; when the dump
IS present they are the real oracle, because they rebuild the two corpora that
are already committed and demand byte-identical output.
"""
import importlib.util
import json
import os
import random
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "gen_retail_bios_seeds.py")
REFERENCE = os.path.join(ROOT, "bios", "SCPH1001.BIN")
BASE_CORPUS = os.path.join(ROOT, "recompiler", "seeds", "phase2_ghidra_seeds.json")

spec = importlib.util.spec_from_file_location("gen_retail_bios_seeds", TOOL)
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

ROM_BYTES = 512 * 1024


def run(*args):
    return subprocess.run([sys.executable, TOOL] + list(args),
                          capture_output=True, text=True, cwd=ROOT)


def read_json(path):
    with open(path) as handle:
        return json.load(handle)


def read_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


def synth_rom(path, diverge_at=None, seed=1337):
    """A deterministic pseudo-ROM; bytes at/after diverge_at are randomised."""
    random.seed(seed)
    data = bytearray(random.randrange(256) for _ in range(ROM_BYTES))
    if diverge_at is not None:
        random.seed(seed + 1)
        for i in range(diverge_at, ROM_BYTES):
            data[i] = random.randrange(256)
    with open(path, "wb") as handle:
        handle.write(bytes(data))
    return bytes(data)


def write_corpus(path, addresses):
    payload = {
        "schema": "psxrecomp phase2 seeds",
        "source": "synthetic",
        "seed_count": len(addresses),
        "seeds": [{"address": "0x%08X" % a, "label": "f_%X" % a, "rationale": "test"}
                  for a in addresses],
        "excluded": [{"address": "0xBFC00100", "label": "utlb", "rationale": "test"}],
    }
    with open(path, "w") as handle:
        json.dump(payload, handle, indent=2)


class ModelTokenTests(unittest.TestCase):
    def test_matches_the_cxx_rule(self):
        # Mirrors bios_rom_alias.h so a region-qualified dump resolves alike.
        self.assertEqual(tool.model_token("SCPH1001.BIN"), "SCPH1001")
        self.assertEqual(tool.model_token("EUR-PSX-SCPH5502.bin"), "SCPH5502")
        self.assertEqual(tool.model_token("/a/b/US-PSOne-SCPH101.BIN"), "SCPH101")

    def test_stem_is_legal_even_for_the_dashed_spelling(self):
        # "SCPH-5501.BIN" is a name psx_known_bios_filenames() probes for, and
        # the raw model token for it is "5501" — a leading digit, which
        # runtime.cmake rejects as a backend stem.
        self.assertEqual(tool.model_token("SCPH-5501.BIN"), "5501")
        self.assertEqual(tool.image_stem("SCPH-5501.BIN"), "SCPH5501")
        for name in ("SCPH5501.BIN", "US-PSX-SCPH1001.BIN", "EUR-PSX-SCPH5502.bin"):
            stem = tool.image_stem(name)
            self.assertTrue(stem[0].isalpha() and stem.isalnum(), stem)

    def test_rom_offset_tolerates_every_segment(self):
        for spelling in ("0xBFC00000", "0x9FC00000", "0x1FC00000"):
            self.assertEqual(tool.rom_offset(spelling), 0)
        self.assertEqual(tool.rom_offset("0xBFC18000"), 0x18000)


class FilterTests(unittest.TestCase):
    def test_keeps_matching_drops_diverging_and_reports_the_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            ref = os.path.join(tmp, "ref.bin")
            tgt = os.path.join(tmp, "SCPH9999.bin")
            synth_rom(ref)
            synth_rom(tgt, diverge_at=0x18000)
            # ref and tgt share [0, 0x18000) because both start from seed 1337.
            corpus = os.path.join(tmp, "base.json")
            write_corpus(corpus, [0xBFC00000, 0xBFC10000, 0xBFC17F00,
                                  0xBFC18000, 0xBFC20000])
            out = os.path.join(tmp, "out.json")
            res = run("--target", tgt, "--reference", ref, "--base", corpus, "--out", out)
            self.assertEqual(res.returncode, 0, res.stderr)
            kept = [s["address"] for s in read_json(out)["seeds"]]
            self.assertEqual(kept, ["0xBFC00000", "0xBFC10000", "0xBFC17F00"])
            self.assertIn("ROM 0x18000", res.stdout)

    def test_window_straddling_the_boundary_is_dropped(self):
        # A seed 16 bytes below a divergence: its 64-byte window reaches past it.
        with tempfile.TemporaryDirectory() as tmp:
            ref, tgt = os.path.join(tmp, "r.bin"), os.path.join(tmp, "SCPH9999.bin")
            synth_rom(ref)
            synth_rom(tgt, diverge_at=0x18000)
            corpus = os.path.join(tmp, "base.json")
            write_corpus(corpus, [0xBFC17FF0])
            out = os.path.join(tmp, "out.json")
            self.assertEqual(run("--target", tgt, "--reference", ref,
                                 "--base", corpus, "--out", out).returncode, 1)

    def test_identical_dumps_keep_everything(self):
        with tempfile.TemporaryDirectory() as tmp:
            ref = os.path.join(tmp, "r.bin")
            synth_rom(ref)
            corpus = os.path.join(tmp, "base.json")
            write_corpus(corpus, [0xBFC00000, 0xBFC30000])
            out = os.path.join(tmp, "out.json")
            res = run("--target", ref, "--reference", ref, "--base", corpus, "--out", out)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertEqual(len(read_json(out)["seeds"]), 2)
            self.assertIn("identical", res.stdout)

    def test_unrelated_dump_fails_loud_rather_than_emitting_an_empty_corpus(self):
        with tempfile.TemporaryDirectory() as tmp:
            ref, tgt = os.path.join(tmp, "r.bin"), os.path.join(tmp, "SCPH9999.bin")
            synth_rom(ref, seed=1)
            synth_rom(tgt, seed=2)
            corpus = os.path.join(tmp, "base.json")
            write_corpus(corpus, [0xBFC00000, 0xBFC10000])
            out = os.path.join(tmp, "out.json")
            res = run("--target", tgt, "--reference", ref, "--base", corpus, "--out", out)
            self.assertNotEqual(res.returncode, 0)
            self.assertFalse(os.path.exists(out))

    def test_size_mismatch_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            ref, tgt = os.path.join(tmp, "r.bin"), os.path.join(tmp, "SCPH9999.bin")
            synth_rom(ref)
            with open(tgt, "wb") as handle:
                handle.write(b"\x00" * 1024)
            corpus = os.path.join(tmp, "base.json")
            write_corpus(corpus, [0xBFC00000])
            res = run("--target", tgt, "--reference", ref, "--base", corpus,
                      "--out", os.path.join(tmp, "o.json"))
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("sizes differ", res.stderr)

    def test_base_corpus_is_never_written(self):
        with tempfile.TemporaryDirectory() as tmp:
            ref, tgt = os.path.join(tmp, "r.bin"), os.path.join(tmp, "SCPH9999.bin")
            synth_rom(ref)
            synth_rom(tgt, diverge_at=0x18000)
            corpus = os.path.join(tmp, "base.json")
            write_corpus(corpus, [0xBFC00000])
            before = read_bytes(corpus)
            run("--target", tgt, "--reference", ref, "--base", corpus,
                "--out", os.path.join(tmp, "o.json"))
            self.assertEqual(read_bytes(corpus), before)


@unittest.skipUnless(os.path.isfile(REFERENCE) and os.path.isfile(BASE_CORPUS),
                     "needs bios/SCPH1001.BIN and the base corpus")
class ReproductionTests(unittest.TestCase):
    """Rebuild the two committed corpora and demand byte-identical output."""

    def _pseudo_shell_divergent_rom(self, tmp):
        path = os.path.join(tmp, "pseudo.bin")
        random.seed(1337)
        data = bytearray(read_bytes(REFERENCE))
        for i in range(0x18000, len(data)):
            data[i] = random.randrange(256)
        with open(path, "wb") as handle:
            handle.write(bytes(data))
        return path

    def test_reproduces_both_committed_corpora(self):
        for stem in ("SCPH5552", "SCPH101"):
            committed = os.path.join(ROOT, "recompiler", "seeds",
                                     "phase2_ghidra_seeds_%s.json" % stem)
            if not os.path.isfile(committed):
                continue
            with self.subTest(stem=stem), tempfile.TemporaryDirectory() as tmp:
                res = run("--target", self._pseudo_shell_divergent_rom(tmp),
                          "--stem", stem, "--check", committed)
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
                self.assertIn("MATCH", res.stdout)

    def test_kernel_divergence_is_reported_not_swallowed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "badkernel.bin")
            random.seed(7)
            data = bytearray(read_bytes(REFERENCE))
            for i in range(0x10000, len(data)):
                data[i] = random.randrange(256)
            with open(path, "wb") as handle:
                handle.write(bytes(data))
            res = run("--target", path, "--stem", "BADKERNEL",
                      "--out", os.path.join(tmp, "o.json"))
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn("ROM 0x10000", res.stdout)
            # Far short of the 534 a shell-only divergence yields: the operator
            # must see that this image's kernel is NOT SCPH-1001's.
            kept = read_json(os.path.join(tmp, "o.json"))["seeds"]
            self.assertLess(len(kept), 534)


if __name__ == "__main__":
    unittest.main()
