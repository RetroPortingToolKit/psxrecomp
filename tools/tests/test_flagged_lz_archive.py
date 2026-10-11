"""Synthetic archive/decoder boundaries; no game data in the fixtures."""
import hashlib
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import flagged_lz_archive as archive
import aot_overlay_pipeline as pipeline


class FlaggedLzTests(unittest.TestCase):
    def test_literals_and_overlapping_matches(self):
        self.assertEqual(archive.decode(b'\x00\x00ABC', 3), b'ABC')
        self.assertEqual(archive.decode(b'\x02\x00A\x05\x01', 7), b'AAAAAAA')
        self.assertEqual(archive.decode(b'\x08\x00XYZ\x02\x03', 6), b'XYZXYZ')

    def test_incomplete_or_unbounded_streams_fail(self):
        for data, size in ((b'', 1), (b'\x00', 1), (b'\x00\x00', 1),
                           (b'\x01\x00\x00', 2), (b'\x01\x00\x01\x01', 2),
                           (b'\x02\x00A\x05\x01', 2), (b'\x00\x00AB', 1)):
            with self.subTest(data=data, size=size), self.assertRaises(ValueError):
                archive.decode(data, size)
        with self.assertRaises(ValueError):
            archive.decode(b'\x00\x00A', 100, max_output=16)

    def test_flag_groups_roll_over(self):
        literals = bytes(range(16))
        self.assertEqual(archive.decode(b'\x00\x00' + literals + b'\x00\x00Z', 17), literals+b'Z')


class ArchiveTests(unittest.TestCase):
    def fixture(self):
        data = bytearray(6144)
        struct.pack_into('<I', data, 0, 2)
        # Directory order is deliberately opposite physical order.
        struct.pack_into('<6I', data, 4, 1, 3, 0, 4096, 2, 3)
        struct.pack_into('<6I', data, 28, 4, 7, 5, 2048, 5, 6)
        data[4096:4099] = b'XYZ'
        data[2048:2053] = b'\x02\x00A\x05\x01'
        return data

    def test_unsorted_plain_and_compressed_records(self):
        members = archive.members(self.fixture(), count=2)
        self.assertEqual([m.source_offset for m in members], [4096, 2048])
        self.assertEqual(bytes(members[0].payload), b'XYZ')
        self.assertEqual(archive.decode(members[1].payload, 7), b'AAAAAAA')

    def test_directory_count_padding_overlap_and_bounds(self):
        for offset, value in ((0, 3), (16, 2048), (16, 4097), (16, 8192), (12, 9000)):
            data = self.fixture()
            struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset, value=value), self.assertRaises(ValueError):
                archive.members(data, count=2)
        data = self.fixture(); data[100] = 1
        with self.assertRaises(ValueError): archive.members(data)

    def test_profile_requires_classification_hash_and_ram_placement(self):
        data=self.fixture()
        class Disc:
            def read(self, name): return data
        spec = dict(method='flagged_lz_archive',file='PACK.DAT',count=2,
            members=[dict(index=1,load_addr='0x80010000',decoded_sha256=hashlib.sha256(b'AAAAAAA').hexdigest())],
            excluded_members=[dict(index=0,reason='Non-executable fixture')])
        sources=pipeline.spec_sources(Disc(),spec)
        self.assertEqual(sources[0]['body'],b'AAAAAAA')
        for changed in ({'excluded_members':[]}, {'members':[dict(spec['members'][0], decoded_sha256='0'*64)]},
                        {'members':[dict(spec['members'][0],load_addr='0x90000000')]}):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                pipeline.spec_sources(Disc(),dict(spec,**changed))

    def test_verified_image_slice_and_exe_header(self):
        payload=bytearray(0x820)
        payload[:8]=b'PS-X EXE'
        struct.pack_into('<II',payload,0x18,0x80010000,0x20)
        payload[0x800:]=bytes(range(32))
        data=bytearray(0x1800)
        struct.pack_into('<I',data,0,1)
        struct.pack_into('<6I',data,4,1,len(payload),0,0x800,2,3)
        data[0x800:0x800+len(payload)]=payload
        class Disc:
            def read(self,name): return data
        member=dict(index=0,load_addr='0x80010000',psx_exe=True,
                    decoded_sha256=hashlib.sha256(payload).hexdigest())
        spec=dict(method='flagged_lz_archive',file='PACK.DAT',count=1,members=[member])
        self.assertEqual(pipeline.spec_sources(Disc(),spec)[0]['body'],bytes(range(32)))
        raw=dict(member,psx_exe=False,image_offset=0x800,image_size=16)
        self.assertEqual(pipeline.spec_sources(Disc(),dict(spec,members=[raw]))[0]['body'],bytes(range(16)))
        for changed in (dict(raw,image_size=33),dict(raw,image_offset=-1),
                        dict(member,load_addr='0x80020000')):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                pipeline.spec_sources(Disc(),dict(spec,members=[changed]))


if __name__=='__main__': unittest.main()
