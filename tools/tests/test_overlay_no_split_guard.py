#!/usr/bin/env python3
"""The overlay no-split guard, image-local root enrichment, and the cache key.

A walk root is a hard cap: the walk of the root below it stops there, so a
root inside another function's body splits that function. The generated C
still passes every audit, which is why this class never shows up as a shard
failure. Measured on Tomba (bead beads-eio.3.177): shared engine code, byte
identical in 12 swapped area images, does `jal 0x8011B1CC`; that address is a
function start in one image and a `subu` mid-way through host 0x8011AC10 in
X00. Rooting it there split the host and moved guest timing by one cycle
against the interpreter.

Usage: test_overlay_no_split_guard.py --cmake <cmake> [--recompiler <exe>]
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import compile_overlays as CO  # noqa: E402

LOAD = 0x80100000
NOP = 0x00000000
JR_RA = 0x03E00008
FRAME = 0x27BDFFE0          # addiu sp,sp,-32
UNFRAME = 0x27BD0020        # addiu sp,sp,32
ADDIU = 0x24420001          # addiu v0,v0,1
SUBU = 0x00431023           # subu v0,v0,v1
CMAKE = shutil.which('cmake')   # --cmake overrides; ctest always passes it
RECOMPILER = None


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def bne_v0(pc, target):
    return 0x14400000 | (((target - (pc + 4)) >> 2) & 0xFFFF)


def image(words: dict, size: int) -> bytes:
    data = bytearray(size)
    for addr, word in words.items():
        struct.pack_into('<I', data, addr - LOAD, word)
    return bytes(data)


def classify(data, enrichment=True, **cap_fields):
    cap = {'schema': 'psxrecomp overlay capture v2',
           'function_entry_pcs': [], 'dispatch_entry_pcs': []}
    for key, values in cap_fields.items():
        cap[key] = [f'0x{addr:08X}' for addr in values]
    return CO.classify_overlay_seeds(cap, data, LOAD, len(data), 0, {},
                                     root_enrichment=enrichment)


def root_seeds(seeds):
    return {int(seed.split()[-1], 16) for seed in seeds
            if seed.split()[0] in ('call_root', 'dispatch_root') or
            seed.startswith('0x')}


class NoSplitGuardTests(unittest.TestCase):
    # (a) ------------------------------------------------------------------
    def test_fallthrough_candidate_is_demoted_not_rooted(self):
        host, mid = LOAD + 0x20, LOAD + 0x30
        data = image({host: FRAME, host + 4: ADDIU, host + 8: ADDIU,
                      host + 12: ADDIU, mid: SUBU, mid + 4: ADDIU,
                      mid + 8: JR_RA, mid + 12: UNFRAME}, 0x100)
        # A #386-style enriched capture declared the mid-body word as a
        # static discovery root; it passes the old CFG-only probe.
        self.assertTrue(CO.plausible_callable_target(
            data, LOAD, len(data), mid, LOAD + len(data)))
        for enrichment in (True, False):
            seeds, audit = classify(data, enrichment,
                                    function_entry_pcs=[host],
                                    static_discovery_entry_pcs=[mid])
            self.assertEqual(audit['included_reasons'][mid],
                             'DISPATCH_INTERIOR')
            self.assertIn(f'interior 0x{mid:08X}', seeds)
            self.assertNotIn(mid, root_seeds(seeds))
            self.assertIn(host, root_seeds(seeds))
            self.assertEqual(audit['guard_demoted'][mid],
                             ('STATIC_DISCOVERY_ROOT', host))

    # (b) ------------------------------------------------------------------
    def test_branch_reached_candidate_is_demoted(self):
        host = LOAD + 0x40
        target = host + 0x14
        caller = LOAD + 0x80
        data = image({host: FRAME,
                      host + 4: bne_v0(host + 4, target), host + 8: NOP,
                      host + 12: JR_RA, host + 16: UNFRAME,
                      # The branch target sits right after `jr ra; delay`:
                      # it looks like a frameless function and passes the
                      # image-local proof, and something jals it, so
                      # enrichment derives it.
                      target: ADDIU, target + 4: JR_RA,
                      target + 8: UNFRAME,
                      caller: FRAME, caller + 4: jal(target),
                      caller + 8: NOP, caller + 12: JR_RA,
                      caller + 16: UNFRAME}, 0x100)
        self.assertIn(target, CO.derive_static_roots(data, LOAD, len(data)))
        seeds, audit = classify(data, True, function_entry_pcs=[host])
        self.assertEqual(audit['included_reasons'][target],
                         'DISPATCH_INTERIOR')
        self.assertNotIn(target, root_seeds(seeds))
        self.assertEqual(audit['guard_demoted'][target][1], host)

    def test_enrichment_root_in_unreached_body_word_is_dropped(self):
        # The host branches (b) over a dead block whose first word looks like
        # a prologue. As a root it would cap the host and turn the branch
        # into a cross-function exit, although nothing reaches it. Enrichment
        # evidence is weak: it is dropped, not rooted and not aliased.
        host = LOAD + 0x20
        dead, label = host + 0x10, host + 0x30
        data = image({host: FRAME,
                      host + 4: 0x10000000 | (((label - (host + 8)) >> 2)
                                              & 0xFFFF),
                      host + 8: NOP,
                      dead: FRAME, dead + 4: ADDIU, dead + 8: JR_RA,
                      dead + 12: UNFRAME,
                      label: ADDIU, label + 4: JR_RA,
                      label + 8: UNFRAME}, 0x100)
        self.assertIn(dead, CO.derive_static_roots(data, LOAD, len(data)))
        seeds, audit = classify(data, True, function_entry_pcs=[host])
        self.assertNotIn(dead, root_seeds(seeds))
        self.assertNotIn(dead, audit['included_reasons'])
        self.assertEqual(audit['excluded_reasons'][dead],
                         'ENRICHMENT_SHADOWED')
        self.assertEqual(root_seeds(seeds), {host})
        # An absolute j over it is how tail calls are encoded, so that alone
        # does not make the candidate part of the host.
        tail = bytearray(data)
        struct.pack_into('<I', tail, host + 4 - LOAD,
                         0x08000000 | ((label >> 2) & 0x03FFFFFF))
        seeds, audit = classify(bytes(tail), True, function_entry_pcs=[host])
        self.assertIn(dead, root_seeds(seeds))

    def test_delay_slot_candidate_is_never_root_nor_alias(self):
        host, callee = LOAD + 0x20, LOAD + 0x80
        slot = host + 8
        data = image({host: FRAME, host + 4: jal(callee), slot: ADDIU,
                      host + 12: JR_RA, host + 16: UNFRAME,
                      callee: JR_RA, callee + 4: NOP}, 0x100)
        seeds, audit = classify(data, True, function_entry_pcs=[host],
                                static_discovery_entry_pcs=[slot],
                                dispatch_entry_pcs=[slot])
        self.assertNotIn(slot, audit['included_reasons'])
        self.assertEqual(audit['excluded_reasons'][slot], 'DELAY_SLOT')
        self.assertFalse(any(seed.endswith(f'0x{slot:08X}') for seed in seeds))
        self.assertIn(slot, audit['delay_slot_rejected'])
        self.assertFalse(CO.image_local_entry_proven(
            data, LOAD, len(data), slot, LOAD + len(data)))

    def test_switch_case_roots_absorbed_together(self):
        # Every case label passes the image-local proof (each case before it
        # ends in `jr ra`), and the jump table is a >=3 pointer run, so
        # enrichment derives them all. The table resolves only when every
        # target is inside the host's walk, so they must be absorbed as a run.
        host = LOAD
        table = LOAD + 0x200
        cases = [LOAD + 0x68, LOAD + 0x78, LOAD + 0x88]
        words = {host: FRAME,
                 LOAD + 0x04: 0x3C088010,     # lui t0,0x8010
                 LOAD + 0x08: 0x25100200,     # addiu s0,t0,0x200
                 LOAD + 0x0C: 0x2C620003,     # sltiu v0,v1,3
                 LOAD + 0x10: 0x10400000 | ((0x98 - 0x14) >> 2),  # beq v0,zero
                 LOAD + 0x14: NOP,
                 LOAD + 0x18: 0x00031080,     # sll v0,v1,2
                 LOAD + 0x1C: 0x00501021,     # addu v0,v0,s0
                 LOAD + 0x20: 0x8C420000,     # lw v0,0(v0)
                 LOAD + 0x24: NOP,
                 LOAD + 0x28: 0x00400008,     # jr v0
                 LOAD + 0x2C: NOP,
                 LOAD + 0x98: JR_RA, LOAD + 0x9C: UNFRAME}
        for index, case in enumerate(cases):
            words[case] = ADDIU
            words[case + 4] = JR_RA
            words[case + 8] = UNFRAME
            words[table + index * 4] = case
        data = image(words, 0x300)
        derived = CO.derive_static_roots(data, LOAD, len(data))
        self.assertTrue(set(cases[1:]) <= derived)
        # The guard alone (the case labels supplied as explicit roots, as a
        # #386-enriched capture would) absorbs the whole run.
        seeds, audit = classify(data, False, function_entry_pcs=[host],
                                static_discovery_entry_pcs=cases[1:])
        for case in cases[1:]:
            self.assertEqual(audit['included_reasons'][case],
                             'DISPATCH_INTERIOR', hex(case))
            self.assertNotIn(case, root_seeds(seeds))
        # Default enrichment: the guarded prepass resolves the table, so the
        # case labels are never offered as roots at all.
        seeds, audit = classify(data, True, function_entry_pcs=[host])
        for case in cases:
            self.assertIn(audit['included_reasons'].get(case),
                          (None, 'DISPATCH_INTERIOR'), hex(case))
            self.assertNotIn(case, root_seeds(seeds))
        self.assertEqual(root_seeds(seeds), {host})

    # (c) ------------------------------------------------------------------
    def test_jal_proven_function_start_stays_a_root(self):
        caller, framed, frameless = LOAD, LOAD + 0x40, LOAD + 0x80
        data = image({caller: FRAME, caller + 4: jal(framed),
                      caller + 8: NOP, caller + 12: jal(frameless),
                      caller + 16: NOP, caller + 20: JR_RA,
                      caller + 24: UNFRAME,
                      framed: FRAME, framed + 4: ADDIU, framed + 8: JR_RA,
                      framed + 12: UNFRAME,
                      frameless - 8: JR_RA, frameless - 4: NOP,
                      frameless: ADDIU, frameless + 4: JR_RA,
                      frameless + 8: NOP}, 0x100)
        derived = CO.derive_static_roots(data, LOAD, len(data))
        self.assertTrue({caller, framed, frameless} <= derived)
        seeds, audit = classify(data, True)
        roots = root_seeds(seeds)
        self.assertTrue({caller, framed, frameless} <= roots)
        self.assertEqual(audit['guard_demoted'], {})

    # (d) ------------------------------------------------------------------
    def _cross_image(self, target_is_function):
        engine, host = LOAD, LOAD + 0x100
        target = LOAD + 0x110
        words = {engine: FRAME, engine + 4: jal(target), engine + 8: NOP,
                 engine + 12: JR_RA, engine + 16: UNFRAME}
        if target_is_function:
            # Sibling image: the called address is a real function here.
            words.update({host: ADDIU, host + 4: JR_RA, host + 8: NOP,
                          target: FRAME, target + 4: ADDIU,
                          target + 8: JR_RA, target + 12: UNFRAME})
        else:
            # This image: the same address is mid-way through a host
            # reached by pure fallthrough (the X00 shape).
            words.update({host: FRAME, host + 4: ADDIU, host + 8: ADDIU,
                          host + 12: ADDIU, target: SUBU,
                          target + 4: ADDIU, target + 8: JR_RA,
                          target + 12: UNFRAME})
        return image(words, 0x200), engine, host, target

    def test_shared_jal_into_mid_body_is_not_rooted(self):
        data, engine, host, target = self._cross_image(False)
        weak = set()
        strong = CO.derive_static_roots(data, LOAD, len(data), weak_out=weak)
        self.assertNotIn(target, strong)
        self.assertIn(target, weak)      # the CFG probe alone accepts it
        self.assertTrue({engine, host} <= strong)
        derived, stats = CO.derive_enrichment_roots(data, LOAD, len(data))
        self.assertNotIn(target, derived)   # host's prologue reaches it
        self.assertEqual(stats['weak_dropped'], 1)
        # Enrichment on (default): the rooted engine walk still derives the
        # jal target, and the guard absorbs it into its host.
        seeds, audit = classify(data, True)
        self.assertNotIn(target, root_seeds(seeds))
        self.assertEqual(audit['included_reasons'][target],
                         'DISPATCH_INTERIOR')
        self.assertIn(host, root_seeds(seeds))
        # A #386-enriched capture that already names it is demoted too.
        seeds, audit = classify(data, True,
                                static_discovery_entry_pcs=[target])
        self.assertNotIn(target, root_seeds(seeds))
        self.assertEqual(audit['guard_demoted'][target],
                         ('STATIC_DISCOVERY_ROOT', host))

    def test_weak_target_nothing_reaches_is_rooted(self):
        # A frameless function right after another function's tail jump,
        # called by jal: no `jr $ra` before it, so no boundary proof, but no
        # possible function start reaches it either.
        caller, tail, leaf, far = LOAD, LOAD + 0x40, LOAD + 0x4C, LOAD + 0x80
        data = image({caller: FRAME, caller + 4: jal(leaf), caller + 8: NOP,
                      caller + 12: JR_RA, caller + 16: UNFRAME,
                      tail: ADDIU,
                      tail + 4: 0x08000000 | ((far >> 2) & 0x03FFFFFF),
                      tail + 8: NOP,
                      leaf: ADDIU, leaf + 4: JR_RA, leaf + 8: NOP,
                      far: JR_RA, far + 4: NOP}, 0x100)
        weak = set()
        CO.derive_static_roots(data, LOAD, len(data), weak_out=weak)
        self.assertIn(leaf, weak)
        derived, _stats = CO.derive_enrichment_roots(data, LOAD, len(data))
        self.assertIn(leaf, derived)
        seeds, _audit = classify(data, True)
        self.assertIn(leaf, root_seeds(seeds))

    def test_shared_jal_into_real_function_in_sibling_image_is_rooted(self):
        data, _engine, _host, target = self._cross_image(True)
        self.assertIn(target, CO.derive_static_roots(data, LOAD, len(data)))
        seeds, _audit = classify(data, True)
        self.assertIn(target, root_seeds(seeds))

    def test_cross_producer_call_evidence(self):
        # Producer A calls into producer B. A cross-producer call is only
        # ever weaker evidence: never WEAK evidence, STRONG only when B's own
        # bytes bound the target and the capture is not strict.
        a_fn, bounded, frameless = LOAD, LOAD + 0x110, LOAD + 0x130
        data = image({a_fn: FRAME, a_fn + 4: jal(bounded), a_fn + 8: NOP,
                      a_fn + 12: jal(frameless), a_fn + 16: NOP,
                      a_fn + 20: JR_RA, a_fn + 24: UNFRAME,
                      bounded - 8: JR_RA, bounded - 4: NOP,
                      bounded: ADDIU, bounded + 4: JR_RA, bounded + 8: NOP,
                      frameless - 4: ADDIU,
                      frameless: ADDIU, frameless + 4: JR_RA,
                      frameless + 8: NOP}, 0x200)
        ranges = [(LOAD, LOAD + 0x100), (LOAD + 0x100, LOAD + 0x200)]
        for strict in (True, False):
            roots, _ = CO.derive_enrichment_roots(
                data, LOAD, len(data), ranges, strict_producer_ranges=strict)
            self.assertNotIn(frameless, roots)
            self.assertEqual(bounded in roots, not strict)

    # late prologues (beads-eio.3.191) -------------------------------------
    # A stack adjust is a boundary only when it is at one. Compilers schedule
    # loads in front of it, so rooting every `addiu sp,sp,-N` capped 359 Ace
    # Combat 3 functions two to five words in, 175 of them with the real
    # start executed. The prologue must root the function's true start.
    AC3 = {   # Ace Combat 3 USA image 0 (load 0x800C8000), 0x800D1250..
        0x00: JR_RA,            # jr ra
        0x04: 0x27BD0018,       # addiu sp,sp,24        (delay slot)
        0x08: 0x3C02800F,       # lui v0,0x800F         <- real start
        0x0C: 0x8C435D4C,       # lw v1,0x5D4C(v0)
        0x10: 0x27BDFFE8,       # addiu sp,sp,-24       <- was rooted
        0x14: 0xAFBF0014,       # sw ra,20(sp)
        0x18: 0xAFB00010,       # sw s0,16(sp)
        0x1C: 0x8C620000,       # lw v0,0(v1)
        0x20: NOP,
        0x24: 0x8FBF0014,       # lw ra,20(sp)
        0x28: 0x8FB00010,       # lw s0,16(sp)
        0x2C: JR_RA,
        0x30: 0x27BD0018,       # addiu sp,sp,24
    }

    def ac3_image(self, base=LOAD + 0x40, extra=None):
        words = {LOAD: FRAME, LOAD + 4: JR_RA, LOAD + 8: UNFRAME}
        words.update({base + off: word for off, word in self.AC3.items()})
        words.update(extra or {})
        return image(words, 0x100), base + 0x08, base + 0x10

    def test_late_prologue_roots_its_true_start(self):
        data, start, prologue = self.ac3_image()
        self.assertEqual(CO.prologue_function_start(data, LOAD, prologue),
                         start)
        self.assertFalse(CO.image_local_entry_proven(
            data, LOAD, len(data), prologue, LOAD + len(data)))
        sources = {}
        strong = CO.derive_static_roots(data, LOAD, len(data),
                                        sources=sources)
        self.assertIn(start, strong)
        self.assertNotIn(prologue, strong)
        self.assertIn('prologue_start', sources[start])
        _roots, stats = CO.derive_enrichment_roots(data, LOAD, len(data))
        self.assertEqual(stats['late_prologue_starts'], 1)
        seeds, audit = classify(data, True)
        self.assertIn(start, root_seeds(seeds))
        self.assertNotIn(prologue, root_seeds(seeds))
        # Captured entry evidence at the stack adjust is absorbed by the true
        # start: an alias, never a cap.
        seeds, audit = classify(data, True, dispatch_entry_pcs=[prologue])
        self.assertIn(start, root_seeds(seeds))
        self.assertEqual(audit['included_reasons'][prologue],
                         'DISPATCH_INTERIOR')
        self.assertEqual(audit['guard_demoted'][prologue][1], start)

    def test_prologue_at_a_boundary_is_its_own_start(self):
        # image start, after jr ra + padding, after j, after a break trap
        far = LOAD + 0xF0
        data = image({LOAD: FRAME, LOAD + 4: JR_RA, LOAD + 8: UNFRAME,
                      LOAD + 0x20: FRAME, LOAD + 0x24: JR_RA,
                      LOAD + 0x28: UNFRAME,
                      LOAD + 0x40: 0x08000000 | ((far >> 2) & 0x03FFFFFF),
                      LOAD + 0x44: NOP, LOAD + 0x48: FRAME,
                      LOAD + 0x4C: JR_RA, LOAD + 0x50: UNFRAME,
                      LOAD + 0x60: 0x0000000D,          # break
                      LOAD + 0x64: 0x3C02800F,          # lui v0 <- start
                      LOAD + 0x68: FRAME, LOAD + 0x6C: JR_RA,
                      LOAD + 0x70: UNFRAME,
                      far: JR_RA, far + 4: NOP}, 0x100)
        for addr in (LOAD, LOAD + 0x20, LOAD + 0x48):
            self.assertEqual(CO.prologue_function_start(data, LOAD, addr),
                             addr, hex(addr))
            self.assertTrue(CO.image_local_entry_proven(
                data, LOAD, len(data), addr, LOAD + len(data)), hex(addr))
        self.assertEqual(CO.prologue_function_start(data, LOAD, LOAD + 0x68),
                         LOAD + 0x64)
        self.assertTrue({LOAD, LOAD + 0x20, LOAD + 0x48, LOAD + 0x64} <=
                        CO.derive_static_roots(data, LOAD, len(data)))

    def test_preamble_with_branch_or_call_proves_no_start(self):
        # `jr ra; nop; lw v0,0(a0); beqz v0,out; nop; addiu sp` -- the stack
        # adjust is reached by fallthrough from code this walk does not
        # bound. It is weak evidence, and the start after `jr ra` (a possible
        # function start) reaches it, so it is dropped: never a root.
        base = LOAD + 0x40
        out = base + 0x30
        for shape, word in (('branch', bne_v0(base + 0x0C, out)),
                            ('call', jal(LOAD))):
            data = image({LOAD: FRAME, LOAD + 4: JR_RA, LOAD + 8: UNFRAME,
                          base: JR_RA, base + 4: NOP,
                          base + 8: 0x8C820000,          # lw v0,0(a0)
                          base + 0x0C: word, base + 0x10: NOP,
                          base + 0x14: FRAME, base + 0x18: ADDIU,
                          base + 0x1C: JR_RA, base + 0x20: UNFRAME,
                          out: JR_RA, out + 4: NOP}, 0x100)
            prologue = base + 0x14
            self.assertIsNone(CO.prologue_function_start(data, LOAD,
                                                         prologue), shape)
            self.assertFalse(CO.image_local_entry_proven(
                data, LOAD, len(data), prologue, LOAD + len(data)), shape)
            weak = set()
            strong = CO.derive_static_roots(data, LOAD, len(data),
                                            weak_out=weak)
            self.assertNotIn(prologue, strong, shape)
            self.assertNotIn(base + 8, strong, shape)
            roots, _stats = CO.derive_enrichment_roots(data, LOAD, len(data))
            self.assertNotIn(prologue, roots, shape)
            seeds, _audit = classify(data, True)
            self.assertNotIn(prologue, root_seeds(seeds), shape)

    def test_branch_from_below_into_preamble_proves_no_start(self):
        # The function before returns early (`jr ra`) and branches past it to
        # a label inside what looks like the next function's preamble: the
        # words after that `jr ra` are still its body. Also the same with the
        # label exactly at the would-be start.
        host = LOAD + 0x20
        for label_off in (0x0C, 0x08):
            label = host + 0x10 + label_off
            data = image({host: FRAME,
                          host + 4: bne_v0(host + 4, label),
                          host + 8: NOP,
                          host + 0x0C: JR_RA, host + 0x10: UNFRAME,
                          host + 0x18: 0x3C02800F,        # lui v0,0x800F
                          host + 0x1C: 0x8C435D4C,        # lw v1,..(v0)
                          host + 0x20: FRAME, host + 0x24: ADDIU,
                          host + 0x28: JR_RA, host + 0x2C: UNFRAME}, 0x100)
            prologue = host + 0x20
            self.assertIsNone(CO.prologue_function_start(data, LOAD,
                                                         prologue),
                              hex(label))
            strong = CO.derive_static_roots(data, LOAD, len(data))
            self.assertNotIn(host + 0x18, strong, hex(label))
            self.assertNotIn(prologue, strong, hex(label))
            seeds, _audit = classify(data, True, function_entry_pcs=[host])
            self.assertEqual(root_seeds(seeds), {host}, hex(label))

    def test_prologue_after_data(self):
        # A pointer table (two pointer-shaped words) or an invalid word ends
        # the walk: the prologue right after it is its own start; a preamble
        # after data must be global-data setup, since data decodes as
        # instructions too.
        table = LOAD + 0x40
        words = {LOAD: FRAME, LOAD + 4: JR_RA, LOAD + 8: UNFRAME,
                 table: LOAD + 0x80, table + 4: LOAD + 0x90,
                 table + 8: FRAME, table + 12: JR_RA, table + 16: UNFRAME}
        data = image(words, 0x100)
        self.assertEqual(CO.prologue_function_start(data, LOAD, table + 8),
                         table + 8)
        setup = dict(words)
        setup.update({table + 8: 0x3C02800F, table + 12: 0x8C435D4C,
                      table + 16: FRAME, table + 20: JR_RA,
                      table + 24: UNFRAME})
        data = image(setup, 0x100)
        self.assertEqual(CO.prologue_function_start(data, LOAD, table + 16),
                         table + 8)
        other = dict(setup)
        other[table + 12] = 0xA6020020                     # sh v0,32(s0)
        data = image(other, 0x100)
        self.assertIsNone(CO.prologue_function_start(data, LOAD, table + 16))
        invalid = {LOAD + 0x40: 0x0000003D, LOAD + 0x44: FRAME,
                   LOAD + 0x48: JR_RA, LOAD + 0x4C: UNFRAME,
                   # REGIMM with no such rt: data shaped like a branch
                   LOAD + 0x60: 0x06040200, LOAD + 0x64: FRAME,
                   LOAD + 0x68: JR_RA, LOAD + 0x6C: UNFRAME}
        data = image(invalid, 0x100)
        for addr in (LOAD + 0x44, LOAD + 0x64):
            self.assertEqual(CO.prologue_function_start(data, LOAD, addr),
                             addr, hex(addr))

    def test_single_pointer_shaped_word_is_code(self):
        # `lui v0,0x8010; lb v0,0x5CEC(v0)` is 0x80425CEC: one aligned RAM
        # address in a preamble is an instruction, not a table.
        base = LOAD + 0x40
        data = image({base: JR_RA, base + 4: UNFRAME,
                      base + 8: 0x3C028010, base + 12: 0x80425CEC,
                      base + 16: FRAME, base + 20: JR_RA,
                      base + 24: UNFRAME}, 0x100)
        self.assertEqual(CO.prologue_function_start(data, LOAD, base + 16),
                         base + 8)

    def test_late_prologue_start_respects_producer_start(self):
        # The walk never leaves the prologue's producer.
        data, start, prologue = self.ac3_image(base=LOAD + 0xC0)
        ranges = [(LOAD, LOAD + 0xC8), (LOAD + 0xC8, LOAD + 0x100)]
        self.assertEqual(CO.prologue_function_start(
            data, LOAD, prologue, LOAD + 0xC8), start)
        self.assertEqual(CO.prologue_function_start(
            data, LOAD, prologue, LOAD + 0xCC), LOAD + 0xCC)
        self.assertIn(start, CO.derive_static_roots(data, LOAD, len(data),
                                                    ranges))

    # enrichment policy ----------------------------------------------------
    def test_enrichment_is_default_and_opt_out_is_diagnostic(self):
        data, engine, host, _target = self._cross_image(False)
        old = os.environ.pop('PSX_OVERLAY_ROOT_ENRICHMENT', None)
        try:
            self.assertTrue(CO.root_enrichment_default())
            _seeds, audit = classify(data, None)
            self.assertTrue(audit['root_enrichment'])
            self.assertTrue({engine, host} <= audit['derived_static_roots'])
            os.environ['PSX_OVERLAY_ROOT_ENRICHMENT'] = '0'
            self.assertFalse(CO.root_enrichment_default())
            _seeds, audit = classify(data, None)
            self.assertFalse(audit['root_enrichment'])
            self.assertEqual(audit['derived_static_roots'], set())
        finally:
            os.environ.pop('PSX_OVERLAY_ROOT_ENRICHMENT', None)
            if old is not None:
                os.environ['PSX_OVERLAY_ROOT_ENRICHMENT'] = old
        # A conservative retry recipe is never enriched again.
        cap = {'schema': 'psxrecomp overlay capture v2',
               CO.ROOT_ENRICHMENT_OFF_KEY: True}
        _seeds, audit = CO.classify_overlay_seeds(
            cap, data, LOAD, len(data), 0, {}, root_enrichment=True)
        self.assertEqual(audit['derived_static_roots'], set())
        retry = CO.conservative_retry_capture(
            {'schema': 'x'}, {'derived_static_roots': {engine}})
        self.assertTrue(retry[CO.ROOT_ENRICHMENT_OFF_KEY])
        self.assertIsNone(CO.conservative_retry_capture(
            retry, {'derived_static_roots': {engine}}))


# (e) ----------------------------------------------------------------------
class CacheKeyTests(unittest.TestCase):
    def _cmake_list(self):
        self.assertIsNotNone(CMAKE, 'cmake not found: pass --cmake or put '
                                    'cmake on PATH')
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / 'list.cmake'
            script.write_text(
                f'set(PSXRECOMP_CODEGEN_HASH_ROOT "{ROOT.as_posix()}")\n'
                f'include("{(ROOT / "runtime/codegen_hash_sources.cmake").as_posix()}")\n'
                'message(STATUS "SRCS=${PSXRECOMP_CODEGEN_HASH_SRCS}")\n',
                encoding='utf-8')
            proc = subprocess.run([CMAKE, '-P', str(script)],
                                  capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        match = re.search(r'SRCS=(.*)', proc.stdout + proc.stderr)
        self.assertIsNotNone(match)
        return match.group(1).strip().split(';')

    def _hash_header(self, sources, out_dir):
        header = Path(out_dir) / 'overlay_codegen_hash.h'
        proc = subprocess.run(
            [CMAKE, f'-DOUT={header.as_posix()}',
             '-DSRCS=' + ';'.join(sources),
             '-P', str(ROOT / 'runtime' / 'hash_codegen.cmake')],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        shutil.copy2(ROOT / 'runtime' / 'include' / 'overlay_api.h',
                     Path(out_dir) / 'overlay_api.h')
        return CO.codegen_hash(str(out_dir))

    def test_root_policy_is_part_of_the_cache_namespace(self):
        sources = self._cmake_list()
        classifier = (ROOT / 'tools' / 'compile_overlays.py').as_posix()
        self.assertIn(classifier, sources)
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            changed = tmp / 'compile_overlays.py'
            changed.write_text(
                Path(classifier).read_text(encoding='utf-8') +
                '\n# a root-policy change\n', encoding='utf-8')
            (tmp / 'a').mkdir()
            (tmp / 'b').mkdir()
            before = self._hash_header(sources, tmp / 'a')
            after = self._hash_header(
                [changed.as_posix() if s == classifier else s
                 for s in sources], tmp / 'b')
            self.assertNotEqual(before, after)
            saved = CO.overlay_config_hash
            CO.overlay_config_hash = lambda recompiler, toml: 0x1234ABCD
            try:
                tag_a = CO.cache_tag(str(tmp / 'a'), 'r', 'g', 0)
                tag_b = CO.cache_tag(str(tmp / 'b'), 'r', 'g', 0)
            finally:
                CO.overlay_config_hash = saved
            self.assertNotEqual(tag_a, tag_b)


# recompiler enforcement ---------------------------------------------------
class RecompilerGuardTests(unittest.TestCase):
    def test_recompiler_absorbs_explicit_fallthrough_root(self):
        if not RECOMPILER:
            self.skipTest('pass --recompiler (ctest always does)')
        host, mid = LOAD + 0x20, LOAD + 0x30
        data = image({host: FRAME, host + 4: ADDIU, host + 8: ADDIU,
                      host + 12: ADDIU, mid: SUBU, mid + 4: ADDIU,
                      mid + 8: JR_RA, mid + 12: UNFRAME}, 0x1000)
        with tempfile.TemporaryDirectory() as tmp:
            psx = Path(tmp) / 'split.psx'
            psx.write_bytes(CO.make_psxexe(LOAD, host, data, guard_bytes=0))
            seeds = Path(tmp) / 'seeds.txt'
            # Seeds the classifier would never write: both as call roots.
            seeds.write_text(f'call_root 0x{host:08X}\ncall_root 0x{mid:08X}\n')
            out = Path(tmp) / 'out'
            proc = subprocess.run(
                [RECOMPILER, str(psx), '--seeds', str(seeds),
                 '--out-dir', str(out), '--overlay'],
                capture_output=True, text=True, cwd=str(ROOT))
            self.assertEqual(proc.returncode, 0, proc.stderr or proc.stdout)
            self.assertEqual(
                CO.report_recompiler_guard_absorptions(proc.stdout), 1,
                proc.stdout)
            ranges = next(out.glob('*_full.ranges')).read_text()
        # Both PCs stay callable; the mid-body one only as an alias whose
        # identity covers the host range (it starts at the host).
        self.assertIn(f'F {host:08X}', ranges)
        self.assertIn(f'F {mid:08X}', ranges)
        mid_rows = ranges.split(f'F {mid:08X}', 1)[1].splitlines()[1:]
        self.assertTrue(any(row.startswith(f'R {host:08X}')
                            for row in mid_rows[:2]), ranges)


# supplemental root fragments carry the aliases of their hosts --------------
#
# Measured on Tomba 2 (bead beads-eio.3.177): the 0x80038000 capture has more
# roots than the region shard admits, so ~960 roots are compiled in strong-root
# supplement fragments. Those fragments were built from `dispatch_root` seeds
# only, so every candidate the guard absorbed into such a host (0x8003A678 in
# host 0x8003A614, captured dispatch entries) was served by neither shard: 127
# captured dispatch entries lost the native identity the unguarded build had.
class SupplementFragmentAliasTests(unittest.TestCase):
    HOST, MID = LOAD + 0x20, LOAD + 0x30

    def fallthrough_image(self, size=0x100):
        host, mid = self.HOST, self.MID
        return image({host: FRAME, host + 4: ADDIU, host + 8: ADDIU,
                      host + 12: ADDIU, mid: SUBU, mid + 4: ADDIU,
                      mid + 8: JR_RA, mid + 12: UNFRAME}, size)

    def job(self, data):
        cap = {'schema': 'psxrecomp overlay capture v2',
               'function_entry_pcs': [f'0x{self.HOST:08X}'],
               'dispatch_entry_pcs': [f'0x{self.HOST:08X}'],
               'static_discovery_entry_pcs': [f'0x{self.MID:08X}']}
        _seeds, audit = CO.classify_overlay_seeds(
            cap, data, LOAD, len(data), 0, {}, root_enrichment=True)
        return audit, CO.make_interior_fragment_job(
            LOAD & 0x1FFFFFFF, LOAD, len(data), data, audit, set(), cap)

    def test_classifier_records_final_host_of_every_alias(self):
        audit, _job = self.job(self.fallthrough_image())
        self.assertEqual(audit['included_reasons'][self.MID],
                         'DISPATCH_INTERIOR')
        self.assertEqual(audit['interior_hosts'][self.MID], {self.HOST})
        self.assertEqual(audit['guard_demoted'][self.MID],
                         ('STATIC_DISCOVERY_ROOT', self.HOST))

    def test_root_fragment_selects_its_hosts_aliases(self):
        _audit, job = self.job(self.fallthrough_image())
        self.assertIn(self.HOST, job['static_exact_demands'])
        self.assertEqual(CO.fragment_interior_aliases(job, [self.HOST]),
                         [self.MID])
        self.assertEqual(CO.fragment_interior_aliases(job, [LOAD + 0x80]), [])
        # merged byte-variant recipes union the hosts
        merged = CO.merge_fragment_jobs_by_recipe([job, job])
        self.assertEqual(merged[0]['interior_hosts'][self.MID], {self.HOST})

    def test_fragment_seed_file_carries_aliases_as_interior(self):
        data = self.fallthrough_image()
        seen = {}
        real_run = CO.subprocess.run

        def fake_run(cmd, **kw):
            seen['seeds'] = Path(cmd[cmd.index('--seeds') + 1]).read_text()
            return subprocess.CompletedProcess(cmd, 1, '', 'stop')
        args = argparse.Namespace(recompiler='recomp.exe', game_toml=__file__,
                                  project_root=None, runtime_include='.')
        CO.subprocess.run = fake_run
        try:
            CO.compile_fragment_batch(
                [self.HOST], data, LOAD, len(data), LOAD & 0x1FFFFFFF, '.',
                args, {}, {}, guard_bytes=0, interior_aliases=[self.MID])
        finally:
            CO.subprocess.run = real_run
        lines = seen['seeds'].splitlines()
        self.assertIn(f'dispatch_root 0x{self.HOST:08X}', lines)
        self.assertIn(f'interior 0x{self.MID:08X}', lines)

    def test_alias_of_interpreted_host_gets_its_root_back(self):
        # Tomba 2: host 0x800BDF10 (a walk through data, 48 unsupported
        # instructions) fails its fragment; the 8-byte stubs the guard had
        # absorbed into it were native roots in the unguarded build.
        _audit, job = self.job(self.fallthrough_image())
        self.assertEqual(job['guard_demoted_aliases'], {self.MID})
        self.assertEqual(CO.orphaned_guard_aliases(job, set()), [self.MID])
        self.assertEqual(CO.orphaned_guard_aliases(
            job, {self.HOST & 0x1FFFFFFF}), [])
        self.assertEqual(CO.orphaned_guard_aliases(
            job, {self.MID & 0x1FFFFFFF}), [])
        merged = CO.merge_fragment_jobs_by_recipe([job, dict(job)])
        self.assertEqual(merged[0]['guard_demoted_aliases'], {self.MID})

    def test_alias_failure_never_costs_the_host(self):
        calls = []

        def build(aliases):
            calls.append(list(aliases))
            return ((None, 'generated-c-audit: 1 unknown_bad') if aliases
                    else ([('ok',)], 'built'))
        ids, status = CO.compile_root_fragment_with_aliases(
            [self.HOST], [self.MID], build)
        self.assertEqual((ids, status, calls), ([('ok',)], 'built',
                                                [[self.MID], []]))
        # a multi-root batch is left to the caller's bisection
        calls.clear()
        ids, _status = CO.compile_root_fragment_with_aliases(
            [self.HOST, LOAD + 0x80], [self.MID], build)
        self.assertIsNone(ids)
        self.assertEqual(calls, [[self.MID]])
        # capacity verdicts are never retried
        calls.clear()
        CO.compile_root_fragment_with_aliases(
            [self.HOST], [self.MID],
            lambda a: calls.append(a) or (None, 'candidate-capacity: full; x'))
        self.assertEqual(len(calls), 1)

    def test_recompiler_emits_alias_in_root_fragment(self):
        if not RECOMPILER:
            self.skipTest('pass --recompiler (ctest always does)')
        data = self.fallthrough_image(0x1000)
        with tempfile.TemporaryDirectory() as tmp:
            psx = Path(tmp) / 'frag.psx'
            psx.write_bytes(CO.make_psxexe(LOAD, self.HOST, data,
                                           guard_bytes=0))
            seeds = Path(tmp) / 'seeds.txt'
            seeds.write_text(f'dispatch_root 0x{self.HOST:08X}\n'
                             f'interior 0x{self.MID:08X}\n')
            out = Path(tmp) / 'out'
            proc = subprocess.run(
                [RECOMPILER, str(psx), '--seeds', str(seeds),
                 '--out-dir', str(out), '--overlay'],
                capture_output=True, text=True, cwd=str(ROOT))
            self.assertEqual(proc.returncode, 0, proc.stderr or proc.stdout)
            ranges = next(out.glob('*_full.ranges')).read_text()
        mid_rows = ranges.split(f'F {self.MID:08X}', 1)
        self.assertEqual(len(mid_rows), 2, ranges)
        self.assertTrue(mid_rows[1].splitlines()[1].startswith(
            f'R {self.HOST:08X}'), ranges)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--cmake', default=shutil.which('cmake'))
    ap.add_argument('--recompiler')
    args, rest = ap.parse_known_args()
    if not args.cmake:
        sys.exit('cmake not found: pass --cmake')
    CMAKE = args.cmake
    RECOMPILER = args.recompiler
    unittest.main(argv=[sys.argv[0]] + rest)
