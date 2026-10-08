"""Checks the bytes certified by stock resident metadata, without a game image."""
import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from resident_catalog import collect_entries, executable_range_sha256


class ResidentCatalogTests(unittest.TestCase):
    def test_title_selection_and_real_sector_padding(self):
        class Disc:
            files = {'A.DAT': (20, 2049), 'MUSIC.RAW': (30, 10000), 'B.DAT': (10, 1)}
            def __init__(self):
                self.reader = self
                self.reads = []
            def read_file_bytes(self, lba, size):
                self.reads.append((lba, size))
                return bytes([lba]) * size
        disc = Disc()
        entries = collect_entries(disc, lambda name: name.endswith('.DAT'))
        self.assertEqual(disc.reads, [(10, 2048), (20, 4096)])
        self.assertEqual([e.path for e in entries], ['B.DAT', 'A.DAT'])
        self.assertEqual(entries[1].size, 2049)
        self.assertEqual(entries[1].sha256, hashlib.sha256(bytes([20]) * 4096).hexdigest())
        disc.read_file_bytes = lambda lba, size: b'\0' * (size - 1)
        with self.assertRaisesRegex(ValueError, 'short resident'):
            collect_entries(disc, lambda name: name.endswith('.DAT'))

    def test_code_guards_bound_to_declared_text(self):
        exe = bytearray(0x810)
        exe[:8] = b'PS-X EXE'
        struct.pack_into('<II', exe, 0x18, 0x80010000, 16)
        exe[0x800:] = bytes(range(16))
        self.assertEqual(executable_range_sha256(exe, [(0x80010004, 0x80010008)]),
                         hashlib.sha256(bytes(range(4, 8))).hexdigest())
        with self.assertRaisesRegex(ValueError, 'outside'):
            executable_range_sha256(exe, [(0x80010010, 0x80010011)])
        with self.assertRaisesRegex(ValueError, 'Truncated'):
            executable_range_sha256(exe[:-1], [(0x80010004, 0x80010008)])


if __name__ == '__main__':
    unittest.main()
