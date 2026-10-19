#!/usr/bin/env python3
"""No overlay demand may name a capture's trailing delay-slot guard word.

overlay_capture.c appends one guard word past a dirty-page run so a branch at
the run's last word has its delay slot. The word is the first word of the
NEXT page: readable, never code of this image, and the recompiler's analysis
ends before it (PS-EXE analysis-bound tag).

Measured on Ace Combat 3 (bead beads-eio.3.190): two dispatch entries at
load+size-4 (page-boundary resume points, 0x800D1000 and 0x800EA000) were
classified DISPATCH_INTERIOR because a host walk falls through into the guard
word, then requested as isolated fragments. The recompiler dropped the seed
silently (past Analysis End) and returned an empty manifest: SHARD FAIL
`fragment: no-func-ids`. The fix is on the request side, not a relabel:

  1. classify_overlay_seeds excludes every kind of evidence in the guard word
     as GUARD_WORD (audited, counted, never a seed or a demand);
  2. make_interior_fragment_job, hosted selection and compile_fragment_batch
     hold the same invariant, so no path can request it;
  3. the recompiler warns when it drops a requested seed past Analysis End;
  4. the capture writer no longer records PCs in the guard word (pinned by
     runtime/tests/test_overlay_capture_guard_word.cpp).

Usage: test_overlay_guard_word_requests.py [--recompiler <exe>]
"""
import argparse
import contextlib
import io
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import compile_overlays as CO  # noqa: E402

RECOMPILER = None

LOAD = 0x800D0000
PAGE = 0x1000
SIZE = PAGE + 4                 # one dirty page plus the guard word
GUARD = LOAD + PAGE             # 0x800D1000: first word of the next page
LAST = GUARD - 4                # last analysable word
NOP = 0x00000000
JR_RA = 0x03E00008
FRAME = 0x27BDFFE0              # addiu sp,sp,-32
ADDIU_S5 = 0x26B50014           # addiu s5,s5,0x14 (AC3 0x800D0FFC)
ADDIU_V0 = 0x24420001           # addiu v0,v0,1


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def image(words: dict, size: int = SIZE, load: int = LOAD) -> bytes:
    data = bytearray(size)
    for addr, word in words.items():
        struct.pack_into('<I', data, addr - load, word)
    return bytes(data)


def ac3_shape(extra=None) -> bytes:
    """A host function whose straight-line body runs to the page end and
    falls through into the guard word, as at AC3 0x800CF000 / 0x800E8000."""
    words = {LOAD + 0xF00: FRAME}
    for addr in range(LOAD + 0xF04, GUARD, 4):
        words[addr] = ADDIU_V0
    words[LAST] = ADDIU_S5
    words[GUARD] = ADDIU_V0     # next page's code, captured as the guard
    words.update(extra or {})
    return image(words)


def capture(**fields):
    cap = {'schema': 'psxrecomp overlay capture v2', 'guard_bytes': 4,
           'function_entry_pcs': [], 'dispatch_entry_pcs': []}
    for key, value in fields.items():
        cap[key] = ([f'0x{a:08X}' for a in value]
                    if isinstance(value, (list, set, tuple)) else value)
    return cap


def classify(data, toml=None, load=LOAD, **fields):
    return CO.classify_overlay_seeds(capture(**fields), data, load, len(data),
                                     0, toml or {}, root_enrichment=True)


def seed_addrs(seeds):
    out = set()
    for seed in seeds:
        parts = seed.split()
        if parts and parts[0] not in ('producer_range', 'cross_call_allow'):
            out.add(int(parts[-1], 16))
    return out


def quiet(fn, *a, **kw):
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        result = fn(*a, **kw)
    return result, buf.getvalue()


class ClassifierTests(unittest.TestCase):
    HOST = LOAD + 0xF00

    def assert_guard_excluded(self, seeds, audit, addr=GUARD):
        self.assertEqual(audit['excluded_reasons'].get(addr), 'GUARD_WORD')
        self.assertIn(addr, audit['guard_word_excluded'])
        self.assertNotIn(addr, audit['included_reasons'])
        self.assertNotIn(addr, seed_addrs(seeds))
        self.assertNotIn(addr, audit['dispatch_fragment_demands'])
        self.assertNotIn(addr, audit['static_exact_fragment_demands'])
        self.assertNotIn(addr, audit['static_interval_fragment_demands'])
        self.assertNotIn(addr, audit['interior_hosts'])
        self.assertEqual(audit['analysable_hi'], GUARD)

    def test_ac3_dispatch_entry_in_guard_word_is_never_requested(self):
        data = ac3_shape()
        # The host walk really does visit the guard word by fallthrough:
        # that is what made it a hosted DISPATCH_INTERIOR before the fix.
        walk = CO._walk_overlay_function(data, LOAD, SIZE, self.HOST,
                                         LOAD + SIZE)
        self.assertIn(GUARD, walk['visited'])
        seeds, audit = classify(data, function_entry_pcs=[self.HOST],
                                dispatch_entry_pcs=[self.HOST, GUARD],
                                executed_pcs=[self.HOST, GUARD])
        self.assert_guard_excluded(seeds, audit)
        self.assertIn(self.HOST, seed_addrs(seeds))
        job, log = quiet(CO.make_interior_fragment_job,
                         LOAD & 0x1FFFFFFF, LOAD, SIZE, data, audit, set(),
                         capture())
        if job is not None:
            self.assertNotIn(GUARD, job['candidates'])
            self.assertNotIn(GUARD, CO.select_fragment_orphans(
                job['candidates'], job['executed'], job['forced'],
                job['static_exact_demands'], job['static_interval_demands'],
                set(), []))
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            CO.print_seed_audit(audit)
        self.assertIn('guard_word_excluded: 1', out.getvalue())
        self.assertIn(f'{GUARD:08X}  excluded: GUARD_WORD', out.getvalue())

    def test_same_pc_is_code_in_the_neighbour_variant(self):
        # 0x800D1000 is ordinary analysable code in the image loaded there,
        # so excluding it from the guard-word image loses no coverage.
        data = image({GUARD: FRAME, GUARD + 4: ADDIU_V0, GUARD + 8: JR_RA,
                      GUARD + 12: NOP}, load=GUARD)
        seeds, audit = classify(data, load=GUARD, dispatch_entry_pcs=[GUARD],
                                executed_pcs=[GUARD])
        self.assertIn(GUARD, seed_addrs(seeds))
        self.assertFalse(audit['guard_word_excluded'])

    def test_last_analysable_word_is_unaffected(self):
        data = ac3_shape()
        _seeds, audit = classify(data, function_entry_pcs=[self.HOST],
                                 dispatch_entry_pcs=[LAST],
                                 executed_pcs=[LAST])
        self.assertEqual(audit['included_reasons'].get(LAST),
                         'DISPATCH_INTERIOR')
        self.assertIn(LAST, audit['dispatch_fragment_demands'])
        self.assertFalse(audit['guard_word_excluded'])

    def test_executed_only_pc_in_guard_word(self):
        seeds, audit = classify(ac3_shape(), function_entry_pcs=[self.HOST],
                                executed_pcs=[self.HOST, GUARD])
        self.assert_guard_excluded(seeds, audit)

    def test_every_other_evidence_source_in_guard_word(self):
        data = ac3_shape()
        cases = {
            'function_entry_pcs': dict(function_entry_pcs=[self.HOST, GUARD]),
            'static_discovery_entry_pcs': dict(
                function_entry_pcs=[self.HOST],
                static_discovery_entry_pcs=[GUARD]),
            'static_dispatch_entry_pcs': dict(
                function_entry_pcs=[self.HOST],
                dispatch_entry_pcs=[GUARD], static_dispatch_entry_pcs=[GUARD]),
            'legacy seeds': dict(function_entry_pcs=[self.HOST],
                                 seeds=[GUARD]),
        }
        for name, fields in cases.items():
            with self.subTest(name):
                seeds, audit = classify(data, **fields)
                self.assert_guard_excluded(seeds, audit)
        toml = {'overlays': [{'load_addr': f'0x{LOAD:08X}',
                              'entries': [f'0x{GUARD:08X}']}]}
        seeds, audit = classify(data, toml=toml,
                                function_entry_pcs=[self.HOST])
        self.assert_guard_excluded(seeds, audit)
        alias = capture(function_entry_pcs=[self.HOST])
        alias['static_alias_ranges'] = [{
            'entry': f'0x{GUARD:08X}', 'start': f'0x{self.HOST:08X}',
            'end': f'0x{LOAD + SIZE:08X}'}]
        seeds, audit = CO.classify_overlay_seeds(alias, data, LOAD, SIZE, 0,
                                                 {}, root_enrichment=True)
        self.assert_guard_excluded(seeds, audit)

    def test_call_into_guard_word_is_not_derived(self):
        caller = LOAD + 0x100
        data = ac3_shape({caller: FRAME, caller + 4: jal(GUARD),
                          caller + 8: NOP, caller + 12: JR_RA,
                          caller + 16: NOP})
        walk = CO._walk_overlay_function(data, LOAD, SIZE, caller,
                                         LOAD + SIZE)
        self.assertIn(GUARD, walk['direct_jals'])
        seeds, audit = classify(data, function_entry_pcs=[caller, self.HOST])
        self.assert_guard_excluded(seeds, audit)

    def test_declared_zero_guard_keeps_the_last_word_analysable(self):
        # The exclusion follows the writer's declaration, not the size.
        data = ac3_shape({GUARD: JR_RA})
        cap = capture(function_entry_pcs=[self.HOST],
                      dispatch_entry_pcs=[GUARD], executed_pcs=[GUARD])
        cap['guard_bytes'] = 0
        _seeds, audit = CO.classify_overlay_seeds(cap, data, LOAD, SIZE, 0,
                                                  {}, root_enrichment=True)
        self.assertFalse(audit['guard_word_excluded'])
        self.assertNotEqual(audit['excluded_reasons'].get(GUARD),
                            'GUARD_WORD')


class RequestInvariantTests(unittest.TestCase):
    """Paths that could still name the guard word hold the same rule."""

    def audit(self, **sets):
        audit = {'included_reasons': {}, 'executed_pcs': set(),
                 'dispatch_fragment_demands': set(),
                 'static_exact_fragment_demands': set(),
                 'cross_variant_hosted_demands': set(),
                 'static_interval_fragment_demands': set(),
                 'producer_ranges': [], 'accepted_cross_producer_calls': set()}
        audit.update(sets)
        return audit

    def test_fragment_job_drops_forced_and_leaked_demands(self):
        data = ac3_shape()
        audit = self.audit(
            executed_pcs={LAST, GUARD},
            dispatch_fragment_demands={GUARD},
            static_exact_fragment_demands={GUARD, LAST},
            static_interval_fragment_demands={GUARD},
            cross_variant_hosted_demands={GUARD},
            included_reasons={GUARD: 'DISPATCH_INTERIOR',
                              LAST: 'STATIC_DISCOVERY_ROOT'})
        job, log = quiet(CO.make_interior_fragment_job,
                         LOAD & 0x1FFFFFFF, LOAD, SIZE, data, audit,
                         {GUARD, LAST}, capture())
        for field in ('candidates', 'static_demands', 'static_exact_demands',
                      'hosted_donor_demands', 'static_interval_demands',
                      'forced'):
            self.assertNotIn(GUARD, job[field], field)
        self.assertIn(LAST, job['candidates'])
        self.assertIn(LAST, job['forced'])
        self.assertEqual(job['guard_word_excluded'], {GUARD})
        self.assertIn(f'fragment demand 0x{GUARD:08X} excluded: GUARD_WORD',
                      log)
        self.assertEqual(CO.partition_strong_root_demands(
            job['static_exact_demands'], job['executed'], job['forced'],
            job['static_interval_demands'], set()), ([], [LAST]))

    def test_hosted_selection_rejects_a_guard_word_target(self):
        job = {'load_addr': LOAD, 'size': SIZE, 'guard_bytes': 4,
               'included_reasons': {}, 'producer_ranges': ()}
        audit = {}
        selected = CO.select_hosted_interior_demands(
            job, {GUARD: True}, [], audit)
        self.assertEqual(selected, {})
        self.assertEqual(audit.get('target_guard_word'), 1)

    def test_fragment_batch_refuses_a_guard_word_request(self):
        args = types.SimpleNamespace(recompiler=None, game_toml=None)
        frag_ids, status = CO.compile_fragment_batch(
            {GUARD}, ac3_shape(), LOAD, SIZE, LOAD & 0x1FFFFFFF, '', args,
            {}, {}, guard_bytes=4)
        self.assertIsNone(frag_ids)
        self.assertTrue(status.startswith('guard-word-request'), status)
        self.assertIn(f'0x{GUARD:08X}', status)


class RecompilerWarningTests(unittest.TestCase):
    """The recompiler states it dropped a requested seed past Analysis End."""

    def run_recompiler(self, entry, seed_lines):
        data = ac3_shape({LAST: JR_RA, GUARD: NOP})
        with tempfile.TemporaryDirectory() as tmp:
            psx = Path(tmp) / 'frag.psx'
            psx.write_bytes(CO.make_psxexe(LOAD, entry, data, guard_bytes=4))
            seeds = Path(tmp) / 'seeds.txt'
            seeds.write_text(''.join(line + '\n' for line in seed_lines))
            out = Path(tmp) / 'out'
            out.mkdir()
            return subprocess.run(
                [RECOMPILER, str(psx), '--seeds', str(seeds),
                 '--out-dir', str(out), '--overlay'],
                capture_output=True, text=True, cwd=str(ROOT))

    def setUp(self):
        if not RECOMPILER:
            self.skipTest('pass --recompiler (ctest always does)')

    def test_seed_past_analysis_end_is_reported(self):
        host = LOAD + 0xF00
        proc = self.run_recompiler(host, [f'dispatch_root 0x{host:08X}',
                                          f'dispatch_root 0x{GUARD:08X}',
                                          f'interior 0x{GUARD:08X}'])
        self.assertEqual(proc.returncode, 0, proc.stderr or proc.stdout)
        self.assertIn(
            f'WARNING: dropped 2 requested seed(s) at or past Analysis End '
            f'0x{GUARD:08X}', proc.stdout)
        self.assertNotIn('WARNING: entry point', proc.stdout)
        # compile_overlays copies the warning into the compile log.
        count, log = quiet(CO.report_recompiler_seed_drops, proc.stdout)
        self.assertEqual(count, 1)
        self.assertIn(f'recompiler WARNING: dropped 2 requested seed(s)', log)

    def test_entry_point_past_analysis_end_is_reported(self):
        proc = self.run_recompiler(GUARD, [f'dispatch_root 0x{GUARD:08X}'])
        self.assertIn(f'WARNING: entry point 0x{GUARD:08X} is at or past '
                      f'Analysis End 0x{GUARD:08X}', proc.stdout)
        self.assertIn('WARNING: dropped 1 requested seed(s) at or past '
                      'Analysis End', proc.stdout)

    def test_in_range_seeds_warn_nothing(self):
        host = LOAD + 0xF00
        proc = self.run_recompiler(host, [f'dispatch_root 0x{host:08X}',
                                          f'interior 0x{LAST:08X}'])
        self.assertEqual(proc.returncode, 0, proc.stderr or proc.stdout)
        self.assertNotIn('WARNING: dropped', proc.stdout)
        self.assertNotIn('WARNING: entry point', proc.stdout)


class StaticModeTests(unittest.TestCase):
    """--static binds captured dispatch entries to demands before the
    classifier runs; the guard word must not become one there either."""

    def setUp(self):
        if not RECOMPILER:
            self.skipTest('pass --recompiler (ctest always does)')

    def test_static_mode_never_demands_the_guard_word(self):
        host = LOAD + 0xF00
        data = ac3_shape({LAST: JR_RA, GUARD: NOP})
        rec = capture(function_entry_pcs=[host],
                      dispatch_entry_pcs=[host, GUARD],
                      executed_pcs=[host, GUARD])
        rec.update({'load_addr': f'0x{LOAD:08X}', 'size': SIZE,
                    'bytes_b64': __import__('base64').b64encode(data).decode()})
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'captures.json').write_text(__import__('json').dumps([rec]))
            header = bytearray(0x800)
            header[:8] = b'PS-X EXE'
            struct.pack_into('<I', header, 0x18, 0x80010000)
            struct.pack_into('<I', header, 0x1C, 0x10)
            (tmp / 'MAIN.EXE').write_bytes(bytes(header) + bytes(0x10))
            (tmp / 'game.toml').write_text(
                '[game]\nid = "TEST-00000"\nname = "x"\nexe = "MAIN.EXE"\n'
                'load_address = "0x80010000"\nentry_pc = "0x80010000"\n'
                'text_size = "0x10"\nstack_base = "0x801FFFF0"\n\n'
                '[recompiler]\nseeds = "seeds.txt"\nout_dir = "generated"\n')
            proc = subprocess.run(
                [sys.executable, str(ROOT / 'tools' / 'compile_overlays.py'),
                 '--static', '--captures', str(tmp / 'captures.json'),
                 '--out-dir', str(tmp / 'out'),
                 '--game-toml', str(tmp / 'game.toml'),
                 '--recompiler', RECOMPILER, '--project-root', str(ROOT),
                 '--runtime-include', str(ROOT / 'runtime' / 'include'),
                 '--force-interior', f'0x{GUARD:08X}'],
                capture_output=True, text=True, timeout=300, cwd=str(ROOT))
        out = proc.stdout + proc.stderr
        self.assertEqual(proc.returncode, 0, out[-3000:])
        self.assertIn(f'demand 0x{GUARD:08X} excluded: GUARD_WORD', out)
        self.assertNotIn('recompiler WARNING', out)
        self.assertNotIn('no-func-ids', out)
        self.assertNotIn('SHARD FAIL', out)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--recompiler')
    args, rest = ap.parse_known_args()
    RECOMPILER = args.recompiler
    unittest.main(argv=[sys.argv[0]] + rest)
