"""Synthetic structural fixtures; no retail game bytes."""
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from inspect_disc_inventory import inspect, typed_members, mips_shapes


class TypedArchiveTest(unittest.TestCase):
    def fixture(self):
        data = bytearray(3 * 32)
        struct.pack_into('<6I', data, 0, 2, len(data), 7, 5, 7, 31)
        data[32:37] = b'first'
        data[64:95] = b'z' * 31
        return data

    def test_duplicate_types_and_partial_sectors(self):
        members = typed_members(self.fixture(), 32)
        self.assertEqual([(m['kind'], m['offset'], len(m['body'])) for m in members],
                         [(7, 32, 5), (7, 64, 31)])

    def test_reject_size_count_and_extent_errors(self):
        for offset, value in [(0, 4), (4, 95), (12, 90), (20, 33)]:
            with self.subTest(offset=offset):
                data = self.fixture()
                struct.pack_into('<I', data, offset, value)
                with self.assertRaises(ValueError):
                    typed_members(data, 32)

    def test_header_padding_is_opaque(self):
        data = self.fixture()
        data[24:32] = b'metadata'
        self.assertEqual(len(typed_members(data, 32)), 2)

    def test_reject_unclaimed_payload(self):
        data = self.fixture() + bytearray(32)
        struct.pack_into('<I', data, 4, len(data))
        with self.assertRaisesRegex(ValueError, 'coverage'):
            typed_members(data, 32)

    def test_shapes_are_aligned_and_not_a_classifier(self):
        data = struct.pack('<III', 0x03e00008, 0x27bdffe0, 0x27bd0020) + b'x'
        self.assertEqual(mips_shapes(data), {'aligned_jr_ra': 1, 'negative_sp_adjust': 1})


class FileTableTest(unittest.TestCase):
    def fixture(self):
        class Disc:
            files = {'EXE': (10, 12), 'ASSET': (20, 4)}

            def read(self, name):
                return {'EXE': struct.pack('<III', 20, 4, 123),
                        'ASSET': struct.pack('<I', 123)}[name]
        profile = dict(schema='psxrecomp disc inventory v1', game_id='synthetic',
            disc_hashes={}, expected_groups={},
            file_tables=[dict(file='EXE', address=0, stride=12, lba_offset=0,
                size_offset=4, first_word_offset=8, files=['ASSET'])],
            files=[dict(file=name, method='raw_file', role='Synthetic fixture')
                   for name in Disc.files])
        return profile, Disc()

    def test_full_inventory_and_extent_match(self):
        profile, disc = self.fixture()
        self.assertEqual(len(inspect(profile, disc)['files']), 2)

    def test_mismatched_iso_extent_is_rejected(self):
        profile, disc = self.fixture()
        disc.files = {**disc.files, 'ASSET': (21, 4)}
        with self.assertRaisesRegex(ValueError, 'ISO/table mismatch'):
            inspect(profile, disc)

    def test_unclassified_file_is_rejected(self):
        profile, disc = self.fixture()
        profile['files'].pop()
        with self.assertRaisesRegex(ValueError, 'complete ISO'):
            inspect(profile, disc)

    def test_malformed_record_layout_is_rejected(self):
        for field, value in [('stride', 0), ('stride', 3), ('lba_offset', -4),
                             ('size_offset', 1), ('size_offset', 12),
                             ('first_word_offset', 12)]:
            with self.subTest(field=field, value=value):
                profile, disc = self.fixture()
                profile['file_tables'][0][field] = value
                with self.assertRaisesRegex(ValueError, 'stride|field offset'):
                    inspect(profile, disc)


if __name__ == '__main__':
    unittest.main()
