import struct
import hashlib
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aligned_lzss_banks import decode, members, banks
from aot_overlay_spike.extract_generic import filter_full_discovery_seeds
from aot_overlay_pipeline import positioned_sources


def literal_stream(body):
    out = bytearray(b'\0' + (len(body) - 1).to_bytes(3, 'big'))
    for offset in range(0, len(body), 8):
        block = body[offset:offset + 8]
        out.append((1 << len(block)) - 1)
        out.extend(block)
    return bytes(out)


def archive(*payloads):
    head = bytearray(64)
    struct.pack_into('<II', head, 0, 1, len(payloads))
    result = bytearray()
    for i, body in enumerate(payloads):
        padded = (len(body) + 63) // 64 * 64
        struct.pack_into('<II', head, 8 + i * 8, len(body), padded)
        result.extend(body + bytes(padded - len(body)))
    return bytes(head + result)


class CompressedBankTests(unittest.TestCase):
    def test_flags_cross_byte(self):
        body = b'original disc code'
        stream = literal_stream(body)
        self.assertEqual(decode(stream), (body, len(stream)))

    def test_overlapping_reference_and_length_curve(self):
        # Parameter 8: seven length bits, x2 slope above index 19.
        # Literal A then distance 1 / length index 20 => 24 more As.
        self.assertEqual(decode(bytes.fromhex('0800000101410094'))[0], b'A' * 25)
        # Parameter 7: zero length bits, distance occupies all 16 bits.
        self.assertEqual(decode(bytes.fromhex('0700000101410001'))[0], b'AAAA')

    def test_external_ram_and_truncation_rejected(self):
        for stream in [b'', bytes.fromhex('000000010141'),
                       bytes.fromhex('0000000101410000'),
                       bytes.fromhex('0000000101410100')]:
            with self.subTest(stream=stream), self.assertRaises(ValueError):
                decode(stream)
        with self.assertRaises(ValueError):
            decode(literal_stream(b'ab'), max_output=1)

    def test_selected_bank_and_uninterpreted_data(self):
        meta = literal_stream(struct.pack('<III', 0x4B, 3, 0x31))
        data = archive(meta, literal_stream(b'code'), b'not a compressed code stream')
        inventory, selected = banks(data, alignment=64)
        self.assertEqual(len(inventory), 3)
        self.assertEqual([(x['index'], x['bank'], x['body']) for x in selected], [(1, 3, b'code')])

    def test_data_only_container(self):
        data = archive(literal_stream(struct.pack('<I', 0x30)), b'assets')
        self.assertEqual(banks(data, alignment=64)[1], [])

    def test_archive_and_selector_drift_rejected(self):
        data = archive(literal_stream(struct.pack('<III', 0x4B, 0, 0x31)), literal_stream(b'code'))
        for changed in [data[:-1], data + bytes(64), b'\2' + data[1:]]:
            with self.subTest(size=len(changed)), self.assertRaises(ValueError):
                members(changed, alignment=64)
        with self.assertRaises(ValueError):
            banks(archive(literal_stream(struct.pack('<I', 0x4B))), alignment=64)
        with self.assertRaises(ValueError):
            banks(archive(literal_stream(struct.pack('<I', 0x99))), alignment=64)

    def test_reserved_branch_prefix_is_not_a_discovered_entry(self):
        base = 0x80010000
        data = struct.pack('<6I', 0, 0x19097350, 0x004162B3, 0x03e00008, 0, 0)
        self.assertEqual(filter_full_discovery_seeds(data, base, [base+4, base+12], base), [base+12])
        self.assertEqual(filter_full_discovery_seeds(data, base, [base+4], base+4), [base+4])

    def test_pipeline_deduplicates_verified_banks_and_rejects_drift(self):
        raw = archive(literal_stream(struct.pack('<III', 0x4B, 0, 0x31)), literal_stream(b'code'))
        class Disc:
            def read(self, name):
                return raw
        placement = dict(load_addr='0x80010200', decoded_sha256=hashlib.sha256(b'code').hexdigest())
        spec = dict(method='aligned_lzss_banks', alignment=64, containers=[
            dict(file=name, member_count=2, bank_indices=[0], placements={'0': placement})
            for name in ['A.PAK', 'B.PAK']])
        sources = positioned_sources(Disc(), [spec])
        self.assertEqual(len(sources), 1)
        self.assertEqual(sources[0]['aliases'], ['B.PAK:BANK_0000'])
        spec['containers'][1]['bank_indices'] = []
        with self.assertRaises(ValueError):
            positioned_sources(Disc(), [spec])


if __name__ == '__main__':
    unittest.main()
