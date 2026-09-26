#!/usr/bin/env python3
"""Cross-image call evidence roots an overlay function only where the
overlay's own bytes prove the boundary (bead beads-eio.3.191, category a).

A jal in the main EXE or in another captured image is a call into whatever
image is resident at its target when it runs. Overlay regions are swapped, so
the same call can name a function start in one image and a word mid-way
through a function in another: Tomba's shared engine code (byte-identical in
12 area images) does `jal 0x8011B1CC`, a real start in one image and a `subu`
inside host 0x8011AC10 in X00. So the nomination counts only as STRONG
evidence (image_local_entry_proven: a prologue at a boundary, or a `jr $ra`
before it plus the bounded CFG probe) and never under strict producer ranges.

Measured on Ace Combat 3: frameless functions after `jr $ra` that only other
images call were not roots at all; category a roots them.

Usage: test_overlay_cross_image_calls.py [--recompiler <exe>]
"""
import argparse
import base64
import contextlib
import io
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import compile_overlays as CO  # noqa: E402

RECOMPILER = None

LOAD = 0x80100000
NOP = 0x00000000
JR_RA = 0x03E00008
FRAME = 0x27BDFFE0          # addiu sp,sp,-32
UNFRAME = 0x27BD0020        # addiu sp,sp,32
ADDIU = 0x24420001          # addiu v0,v0,1
SUBU = 0x00431023           # subu v0,v0,v1
LUI_V0 = 0x3C028012         # lui v0,0x8012
LW_V1 = 0x8C430010          # lw v1,0x10(v0)


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def image(words: dict, size: int, load: int = LOAD) -> bytes:
    data = bytearray(size)
    for addr, word in words.items():
        struct.pack_into('<I', data, addr - load, word)
    return bytes(data)


def record(load, data, **fields):
    rec = {'schema': 'psxrecomp overlay capture v2', 'load_addr': f'0x{load:08X}',
           'size': len(data), 'guard_bytes': 0,
           'bytes_b64': base64.b64encode(data).decode(),
           'function_entry_pcs': [], 'dispatch_entry_pcs': []}
    for key, values in fields.items():
        rec[key] = ([f'0x{a:08X}' if isinstance(a, int) else a
                     for a in values]
                    if isinstance(values, (list, set, tuple)) else values)
    return rec


def classify(rec, external=()):
    data = base64.b64decode(rec['bytes_b64'])
    load = int(rec['load_addr'], 16)
    return CO.classify_overlay_seeds(rec, data, load, len(data), 0, {},
                                     root_enrichment=True,
                                     external_call_targets=external)


def root_seeds(seeds):
    return {int(seed.split()[-1], 16) for seed in seeds
            if seed.split()[0] in ('call_root', 'dispatch_root') or
            seed.startswith('0x')}


def quiet(fn, *a, **kw):
    with contextlib.redirect_stdout(io.StringIO()):
        return fn(*a, **kw)


# The overlay: a framed function, then a frameless leaf right after its
# `jr $ra` that nothing in this image calls.
HOST = LOAD
LEAF = LOAD + 0x14
OVERLAY = image({HOST: FRAME, HOST + 4: ADDIU, HOST + 8: JR_RA,
                 HOST + 12: UNFRAME, HOST + 16: NOP,
                 LEAF: ADDIU, LEAF + 4: JR_RA, LEAF + 8: NOP}, 0x100)
# The X00 shape: the same address is mid-way through a host reached by
# fallthrough.
X00 = image({HOST: FRAME, HOST + 4: ADDIU, HOST + 8: ADDIU, HOST + 12: ADDIU,
             HOST + 16: ADDIU, LEAF: SUBU, LEAF + 4: ADDIU, LEAF + 8: JR_RA,
             LEAF + 12: UNFRAME}, 0x100)
ENGINE_LOAD = 0x80080000    # shared engine image, never overlapping 0x8010xxxx
ENGINE = image({ENGINE_LOAD: FRAME, ENGINE_LOAD + 4: jal(LEAF),
                ENGINE_LOAD + 8: NOP, ENGINE_LOAD + 12: JR_RA,
                ENGINE_LOAD + 16: UNFRAME}, 0x40, ENGINE_LOAD)


class DerivationTests(unittest.TestCase):
    def test_target_proven_in_this_image_is_rooted(self):
        rec = record(LOAD, OVERLAY)
        seeds, audit = classify(rec)
        self.assertNotIn(LEAF, root_seeds(seeds))       # no evidence here
        seeds, audit = classify(rec, {LEAF})
        self.assertIn(LEAF, root_seeds(seeds))
        self.assertEqual(audit['included_reasons'][LEAF],
                         'STATIC_DISCOVERY_ROOT')
        self.assertIn(HOST, root_seeds(seeds))
        self.assertEqual(audit['enrichment_stats']['cross_image_jal_only'], 1)

    def test_same_target_mid_function_is_not_rooted(self):
        rec = record(LOAD, X00)
        seeds, audit = classify(rec, {LEAF})
        self.assertNotIn(LEAF, root_seeds(seeds))
        self.assertNotIn(LEAF, audit['derived_static_roots'])
        self.assertIn(HOST, root_seeds(seeds))
        self.assertEqual(audit['enrichment_stats']['cross_image_jal_roots'], 0)
        # Not weak evidence either: the CFG probe alone would accept it.
        self.assertTrue(CO.plausible_callable_target(
            X00, LOAD, len(X00), LEAF, LOAD + len(X00)))

    def test_late_prologue_target_roots_nothing_new(self):
        # A call into a stack adjust that loads precede: the call names the
        # adjust, the image proves the start in front of it. Only the true
        # start is a root, as without the call.
        start, adjust = LOAD + 0x40, LOAD + 0x48
        data = image({HOST: FRAME, HOST + 4: JR_RA, HOST + 8: UNFRAME,
                      start - 4: NOP, start: LUI_V0, start + 4: LW_V1,
                      adjust: FRAME, adjust + 4: ADDIU, adjust + 8: JR_RA,
                      adjust + 12: UNFRAME}, 0x100)
        rec = record(LOAD, data)
        base_seeds, _ = classify(rec)
        seeds, audit = classify(rec, {adjust})
        self.assertEqual(root_seeds(seeds), root_seeds(base_seeds))
        self.assertIn(start, root_seeds(seeds))
        self.assertNotIn(adjust, root_seeds(seeds))

    def test_strict_producer_ranges_ignore_other_images(self):
        rec = record(LOAD, OVERLAY, strict_producer_ranges=True,
                     producer_ranges=[{'start': f'0x{LOAD:08X}',
                                       'end': f'0x{LOAD + len(OVERLAY):08X}'}])
        seeds, _audit = classify(rec, {LEAF})
        self.assertNotIn(LEAF, root_seeds(seeds))

    def test_enrichment_off_uses_no_cross_image_evidence(self):
        rec = record(LOAD, OVERLAY)
        data = base64.b64decode(rec['bytes_b64'])
        seeds, _audit = CO.classify_overlay_seeds(
            rec, data, LOAD, len(data), 0, {}, root_enrichment=False,
            external_call_targets={LEAF})
        self.assertNotIn(LEAF, root_seeds(seeds))


class IndexTests(unittest.TestCase):
    def test_jal_targets_in(self):
        data = image({LOAD: jal(LEAF), LOAD + 8: jal(LOAD + 0x80),
                      LOAD + 16: ADDIU}, 0x20)
        self.assertEqual(CO.jal_targets_in(data, LOAD),
                         [LEAF, LOAD + 0x80])

    def test_sources_are_other_non_overlapping_images_and_the_exe(self):
        overlay = record(LOAD, OVERLAY)
        engine = record(ENGINE_LOAD, ENGINE)
        # An alternative occupant of the same RAM: never co-resident, so its
        # calls into this range nominate nothing here.
        sibling = record(LOAD + 0x80, image({LOAD + 0x80: jal(HOST + 4)},
                                            0x100, LOAD + 0x80))
        calls = CO.CrossImageCalls([overlay, engine, sibling])
        self.assertEqual(calls.targets_for(LOAD, len(OVERLAY)), {LEAF})
        self.assertEqual(CO.CrossImageCalls([overlay, sibling]).targets_for(
            LOAD, len(OVERLAY)), set())
        # The main EXE is a source too.
        exe_text = image({0x80010000: jal(LEAF)}, 0x10, 0x80010000)
        calls = CO.CrossImageCalls([overlay], (0x80010000, exe_text, 'exe'))
        self.assertEqual(calls.targets_for(LOAD, len(OVERLAY)), {LEAF})
        # Nothing at or past the analysable end.
        guard = record(LOAD, OVERLAY)
        tail = image({ENGINE_LOAD: jal(LOAD + 0xFC)}, 0x10, ENGINE_LOAD)
        calls = CO.CrossImageCalls([guard, record(ENGINE_LOAD, tail)])
        self.assertEqual(calls.targets_for(LOAD, 0x100, LOAD + 0xFC), set())
        self.assertEqual(calls.targets_for(LOAD, 0x100), {LOAD + 0xFC})

    def test_tomba_shared_engine_call(self):
        # The engine image calls LEAF. In the image where it is a frameless
        # function it is rooted; in X00 it stays inside its host.
        engine = record(ENGINE_LOAD, ENGINE)
        for data, rooted in ((OVERLAY, True), (X00, False)):
            rec = record(LOAD, data)
            calls = CO.CrossImageCalls([engine, rec])
            seeds, audit = classify(rec, calls.targets_for(LOAD, len(data)))
            self.assertEqual(LEAF in root_seeds(seeds), rooted)
            self.assertIn(HOST, root_seeds(seeds))

    def test_main_exe_resolution(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'disc').mkdir()
            text = image({0x80010000: jal(LEAF)}, 0x10, 0x80010000)
            header = bytearray(0x800)
            header[:8] = b'PS-X EXE'
            struct.pack_into('<I', header, 0x18, 0x80010000)
            struct.pack_into('<I', header, 0x1C, len(text))
            (tmp / 'disc' / 'MAIN.EXE').write_bytes(bytes(header) + text)
            toml = {'game': {'exe': 'disc/MAIN.EXE'}}
            got = quiet(CO.load_main_exe_image, toml, str(tmp / 'game.toml'))
            self.assertEqual(got[:2], (0x80010000, text))
            # Missing: reported, not fatal.
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertIsNone(CO.load_main_exe_image(
                    {'game': {'exe': 'disc/NOPE.EXE'}},
                    str(tmp / 'game.toml')))
            self.assertIn('main EXE not found', buf.getvalue())
            # --main-exe wins over [game] exe.
            got = quiet(CO.load_main_exe_image, {'game': {'exe': 'x'}}, None,
                        None, str(tmp / 'disc' / 'MAIN.EXE'))
            self.assertEqual(got[0], 0x80010000)


class CompileOverlaysWiringTests(unittest.TestCase):
    """compile_overlays main() feeds the other captures and the main EXE to
    the classifier (static mode: no gcc publication needed)."""

    def setUp(self):
        if not RECOMPILER:
            self.skipTest('pass --recompiler (ctest always does)')

    def run_static(self, records, extra=()):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            caps = tmp / 'captures.json'
            caps.write_text(json.dumps(records))
            # The recompiler needs [game] exe; a real PS-X EXE there also
            # makes it the main-EXE source (it calls nothing here).
            header = bytearray(0x800)
            header[:8] = b'PS-X EXE'
            struct.pack_into('<I', header, 0x18, 0x80010000)
            struct.pack_into('<I', header, 0x1C, 0x10)
            (tmp / 'MAIN.EXE').write_bytes(bytes(header) + bytes(0x10))
            toml = tmp / 'game.toml'
            toml.write_text('[game]\nid = "TEST-00000"\nname = "x"\n'
                            'exe = "MAIN.EXE"\nload_address = "0x80010000"\n'
                            'entry_pc = "0x80010000"\ntext_size = "0x10"\n'
                            'stack_base = "0x801FFFF0"\n\n[recompiler]\n'
                            'seeds = "seeds.txt"\nout_dir = "generated"\n')
            proc = subprocess.run(
                [sys.executable, str(ROOT / 'tools' / 'compile_overlays.py'),
                 '--static', '--captures', str(caps),
                 '--out-dir', str(tmp / 'out'), '--game-toml', str(toml),
                 '--recompiler', RECOMPILER, '--project-root', str(ROOT),
                 '--runtime-include', str(ROOT / 'runtime' / 'include'),
                 *extra],
                capture_output=True, text=True, timeout=300, cwd=str(ROOT))
            return proc

    def test_other_capture_nominates_a_root(self):
        proc = self.run_static([record(LOAD, OVERLAY,
                                       function_entry_pcs=[HOST]),
                                record(ENGINE_LOAD, ENGINE,
                                       function_entry_pcs=[ENGINE_LOAD])])
        self.assertEqual(proc.returncode, 0, proc.stdout[-3000:] + proc.stderr)
        self.assertIn(f'{LEAF:08X}  STATIC_DISCOVERY_ROOT', proc.stdout)
        self.assertIn('cross-image jal roots 1 (only evidence 1)', proc.stdout)
        # --only-region keeps the other capture as evidence.
        proc = self.run_static(
            [record(LOAD, OVERLAY, function_entry_pcs=[HOST]),
             record(ENGINE_LOAD, ENGINE, function_entry_pcs=[ENGINE_LOAD])],
            ['--only-region', f'0x{LOAD:08X}'])
        self.assertEqual(proc.returncode, 0, proc.stdout[-3000:] + proc.stderr)
        self.assertIn(f'{LEAF:08X}  STATIC_DISCOVERY_ROOT', proc.stdout)
        # Diagnostic opt-out: no cross-image roots.
        proc = self.run_static(
            [record(LOAD, OVERLAY, function_entry_pcs=[HOST]),
             record(ENGINE_LOAD, ENGINE, function_entry_pcs=[ENGINE_LOAD])],
            ['--no-root-enrichment'])
        self.assertEqual(proc.returncode, 0, proc.stdout[-3000:] + proc.stderr)
        self.assertNotIn(f'{LEAF:08X}  STATIC_DISCOVERY_ROOT', proc.stdout)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--recompiler')
    args, rest = ap.parse_known_args()
    RECOMPILER = args.recompiler
    unittest.main(argv=[sys.argv[0]] + rest)
