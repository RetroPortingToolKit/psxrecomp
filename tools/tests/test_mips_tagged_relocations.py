"""Synthetic exact-loader and malformed-input checks; no game bytes."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mips_tagged_relocations import parse, relocate


def packed(words):
    return struct.pack("<" + "I" * len(words), *words)


class TaggedRelocationTests(unittest.TestCase):
    def test_all_four_writes_and_hi16_carry(self):
        data = packed([24, 0x28, 0x3C080000, 0x2508F000, 0x0C00000A,
                       0xDEADBEEF, 4, 8 | 1, 0xF000, 12 | 2, 16 | 3, 0xFFFFFFFF])
        image, records = relocate(data, 0x80108000)
        self.assertEqual(struct.unpack("<6I", image),
                         (24, 0x80108028, 0x3C088011, 0x25087000, 0x0C04200A, 0xDEADBEEF))
        self.assertEqual([r.kind for r in records], [0, 1, 2, 3])
        self.assertEqual(len(data), 48)

    def test_no_relocations(self):
        image, records = relocate(packed([8, 0x12345678, 0xFFFFFFFF]), 0x80100000)
        self.assertEqual(image, packed([8, 0x12345678]))
        self.assertEqual(records, ())

    def test_rejects_malformed_streams(self):
        invalid = [b"", packed([8, 0]), packed([7, 0, 0xFFFFFFFF]),
                   packed([8, 0, 0]), packed([8, 0, 8, 0xFFFFFFFF]),
                   packed([8, 0, 5]), packed([8, 0, 4, 4, 0xFFFFFFFF]),
                   packed([8, 0, 0xFFFFFFFF, 0])]
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(ValueError):
                parse(data)

    def test_rejects_unproven_memory_layout(self):
        data = packed([8, 0, 0xFFFFFFFF])
        for base in [0, 0x80000002, 0x80200000, 0x801FFFFC]:
            with self.subTest(base=base), self.assertRaises(ValueError):
                relocate(data, base)


if __name__ == "__main__":
    unittest.main()
