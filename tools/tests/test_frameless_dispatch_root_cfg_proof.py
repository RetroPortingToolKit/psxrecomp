#!/usr/bin/env python3
"""A prologue-less dispatch entry needs CFG proof before it becomes a walk root.

Overlay bands are shared address windows, so every observed interpreted PC is
demanded for every occupant of the band. `_callable_legacy_seed` then admits a
demanded address as a callable walk root on any of three grades of evidence:
it is the image entry, it opens a stack frame, or the word two slots back is
`jr $ra`. That third grade is the weak one, and it fires precisely where it is
least trustworthy -- the first word after a function's return is also the first
word of whatever the linker placed next, which in these images is routinely a
pointer table, a packed record array or zero fill. The walk then runs from the
table head to the image end and the generated-C audit reports a column of
UNSUPPORTED_INSTRUCTION.

Measured on Breath of Fire III (SLPS-00990), 406 overlay captures, 5,339
dispatch entries already accepted as walk roots: 2,066 have a prologue, 3,273
ride on the preceding `jr $ra` alone, and the bounded CFG probe rejects 41 of
those 3,273. All 41 are data by inspection (in-image pointer tables, packed
16-bit record arrays, signed delta pairs, zero fill), and 27 of them are
exactly the 27 shards whose all-bands compile failed the audit -- one bad root
per failing shard. The other 14 are the same bug in shards whose data happened
to decode all the way to the image end.

Demotion is not a discard: DISPATCH_INTERIOR keeps the dispatch evidence and
the isolated-fragment demand, and only declines to start a linear walk there.
"""

import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import compile_overlays  # noqa: E402

LOAD = 0x801EEC00

NOP = 0x00000000
JR_RA = 0x03E00008
ADDIU_SP_NEG = 0x27BDFFE0        # addiu sp, sp, -0x20
ADDIU_SP_POS = 0x27BD0020        # addiu sp, sp, 0x20
ORI_V0_ZERO = 0x34020001         # ori v0, zero, 1


def image(words):
    return b"".join(struct.pack("<I", w) for w in words)


class FramelessDispatchRootTests(unittest.TestCase):

    def proven(self, data, addr):
        return compile_overlays._frameless_dispatch_root_proven(
            data, LOAD, len(data), addr, LOAD + len(data))

    def test_pointer_table_after_a_return_is_not_a_walk_root(self):
        """BATL_OVR 0x801EF5F8: five in-image code pointers, then a word table.

        Every word decodes as some R3000 instruction, so opcode validity alone
        says nothing; the walk simply never reaches a return.
        """
        data = image([
            ADDIU_SP_NEG, NOP, JR_RA, ADDIU_SP_POS,   # a real leaf function
            0x801EEC40, 0x801EEC7C, 0x801EEEB8,       # the pointer table
            0x801EEF10, 0x801EF0A8,
            0x000000A0, 0x00000084, 0x00000070,       # the word table
        ])
        table = LOAD + 4 * 4
        self.assertTrue(compile_overlays._callable_legacy_seed(data, LOAD, table))
        self.assertFalse(self.proven(data, table))

    def test_packed_record_table_after_a_return_is_not_a_walk_root(self):
        """AREA050 0x801F2DD4: 12-byte script records, no pointers at all."""
        data = image([
            ADDIU_SP_NEG, NOP, JR_RA, NOP,
            0x00322F08, 0x93800500, 0x00070201,
            0x00329206, 0x2F800900, 0x00030401,
        ])
        table = LOAD + 4 * 4
        self.assertTrue(compile_overlays._callable_legacy_seed(data, LOAD, table))
        self.assertFalse(self.proven(data, table))

    def test_zero_fill_after_a_return_is_not_a_walk_root(self):
        data = image([ADDIU_SP_NEG, NOP, JR_RA, NOP] + [0] * 8)
        self.assertFalse(self.proven(data, LOAD + 4 * 4))

    def test_a_frameless_leaf_after_a_return_stays_a_walk_root(self):
        """The probe must not cost real frameless exports their root.

        3,273 of BoF3's accepted dispatch roots are admitted by the preceding
        `jr $ra` alone; only 41 fail this probe.
        """
        data = image([
            ADDIU_SP_NEG, NOP, JR_RA, NOP,
            ORI_V0_ZERO, JR_RA, NOP, NOP,
        ])
        frameless = LOAD + 4 * 4
        self.assertTrue(compile_overlays._callable_legacy_seed(data, LOAD, frameless))
        self.assertTrue(self.proven(data, frameless))

    def test_a_prologue_needs_no_probe(self):
        """A stack frame is its own evidence, even without a reachable return.

        The walk from a prologue is the recompiler's ordinary business; this
        gate exists only for the grade of evidence that has none.
        """
        data = image([
            ADDIU_SP_NEG, NOP, JR_RA, NOP,
            ADDIU_SP_NEG, 0xAFBF0018, 0x0C000000, NOP,
        ])
        entry = LOAD + 4 * 4
        self.assertFalse(
            compile_overlays.plausible_callable_target(
                data, LOAD, len(data), entry, LOAD + len(data)))
        self.assertTrue(self.proven(data, entry))

    def test_the_image_entry_needs_no_probe(self):
        data = image([0x00000070, 0x0000007A, 0x0000009C, 0x00000094])
        self.assertTrue(self.proven(data, LOAD))


if __name__ == "__main__":
    unittest.main()
