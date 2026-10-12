"""BPE container contract on invented bytes; no game data."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import bpe_container
from bpe_container import BpeError, decode

MAGIC = 0x1A455042


def encode_table(left, right):
    """Run-length table coding with identity stretches skipped."""
    out, c = bytearray(), 0
    while c < 256:
        stretch = 0
        while c + stretch < 256 and stretch < 128 and left[c + stretch] == c + stretch:
            stretch += 1
        if stretch >= 3:
            out.append(0x7E + stretch)          # skips stretch - 1 entries, then one entry
            out.append(c + stretch - 1)         # the last identity entry of the stretch
            c += stretch
            continue
        run = 0
        while c + run < 256 and run < 128 and not (
                left[c + run] == c + run and
                all(left[c + run + k] == c + run + k for k in range(1, 3) if c + run + k < 256) and
                c + run + 3 <= 256):
            run += 1
        run = max(run, 1)
        out.append(run - 1)
        for k in range(c, c + run):
            out.append(left[k])
            if left[k] != k:
                out.append(right[k])
        c += run
    return bytes(out)


def encode_block(symbols, decoded_size, left=None, right=None):
    """A block of input `symbols` that decodes to `decoded_size` bytes."""
    left = list(range(256)) if left is None else left
    right = [0] * 256 if right is None else right
    body = encode_table(left, right) + bytes(symbols)
    body += bytes(-len(body) % 4)
    return struct.pack('<I', decoded_size) + body


def container(blocks, declared, magic=MAGIC):
    return struct.pack('<II', declared, magic) + b''.join(blocks) + struct.pack('<I', 0)


class BpeContainerTests(unittest.TestCase):
    def pairs(self):
        left, right = list(range(256)), [0] * 256
        left[0xF0], right[0xF0] = ord('A'), ord('B')       # 0xF0 -> "AB"
        left[0xF1], right[0xF1] = 0xF0, ord('C')           # 0xF1 -> "ABC" (chained)
        left[0x10], right[0x10] = 0x20, 0x30                # 0x10 -> 0x20 0x30
        return left, right

    def one(self, symbols, size, left=None, right=None):
        return container([encode_block(symbols, size, left, right)], size)

    def test_literals_pairs_chains_and_multiple_blocks(self):
        left, right = self.pairs()
        first = encode_block([0xF1, ord('x'), 0xF0, 0x10], 8, left, right)
        second = encode_block(list(b'plain'), 5)
        data = container([first, second], 13)
        body, consumed = decode(data, magic=MAGIC)
        self.assertEqual(body, b'ABCxAB' + bytes([0x20, 0x30]) + b'plain')
        self.assertEqual(consumed, len(data))

    def test_consumed_counts_words_and_offset_selects_the_container(self):
        data = self.one(list(b'abc'), 3)
        for lead in (b'', bytes(8)):
            with self.subTest(lead=len(lead)):
                body, consumed = decode(lead + data + b'trailing', magic=MAGIC, offset=len(lead))
                self.assertEqual((body, consumed), (b'abc', len(data)))

    def test_skip_coded_identity_stretches_keep_tables_short_and_decode_correctly(self):
        left, right = self.pairs()
        table = encode_table(left, right)
        self.assertLess(len(table), 40, 'identity stretches must be skip-coded')
        body, _ = decode(self.one([0xF1, 0x10, 0x41, 0xF0], 8, left, right), magic=MAGIC)
        self.assertEqual(body, b'ABC' + bytes([0x20, 0x30]) + b'A' + b'AB')

    def test_a_skip_that_reaches_the_last_entry_ends_the_table(self):
        # 128 explicit entries (entry 0 is a pair), then a 128-entry skip: index 256.
        table = bytes([127, ord('Z'), ord('Y')]) + bytes(range(1, 128)) + bytes([0xFF])
        block = struct.pack('<I', 2) + table + bytes([0])
        block += bytes(-len(block) % 4)
        body, _ = decode(container([block], 2), magic=MAGIC)
        self.assertEqual(body, b'ZY')
        # After a skip that stays inside the table, one entry still follows.
        table = bytes([127]) + bytes(range(128)) + bytes([0xFE, 0x41, 0x42])  # skip 127 -> 255, entry 255
        block = struct.pack('<I', 1) + table + bytes([5])
        block += bytes(-len(block) % 4)
        self.assertEqual(decode(container([block], 1), magic=MAGIC)[0], b'')

    def test_rejects_wrong_magic_size_truncation_and_misalignment(self):
        good = self.one(list(b'abc'), 3)
        cases = [
            (self.one(list(b'abc'), 3).replace(struct.pack('<I', MAGIC), struct.pack('<I', 0x12345678), 1),
             dict(), 'magic'),
            (container([encode_block(list(b'abc'), 3)], 4), dict(), 'declares'),
            (container([encode_block(list(b'abc'), 3)], 2), dict(), 'declared size'),
            (good[:-4], dict(), 'truncated'),
            (good[:12], dict(), 'truncated'),
            (good[:9], dict(), 'truncated'),
            (good, dict(offset=2), 'misaligned'),
            (good, dict(max_output=2), 'output limit'),
        ]
        for data, options, error in cases:
            with self.subTest(error=error, options=options), self.assertRaisesRegex(BpeError, error):
                decode(data, magic=MAGIC, **options)

    def test_rejects_cyclic_tables_and_table_runs_past_the_last_entry(self):
        left, right = list(range(256)), [0] * 256
        left[0xF0], right[0xF0] = 0xF1, ord('x')
        left[0xF1], right[0xF1] = 0xF0, ord('y')
        with self.assertRaisesRegex(BpeError, 'does not terminate'):
            decode(self.one([0xF0], 1, left, right), magic=MAGIC)
        # 128 + 101 + 41 entries = 270 > 256.
        table = (bytes([127]) + bytes(range(128)) + bytes([100]) + bytes(range(128, 229)) +
                 bytes([40]) + bytes(range(229, 256)) + bytes(16))
        block = struct.pack('<I', 1) + table + bytes([5])
        block += bytes(-len(block) % 4)
        with self.assertRaisesRegex(BpeError, 'passes its last entry'):
            decode(container([block], 1), magic=MAGIC)

    def test_magic_is_required_evidence_not_a_default(self):
        with self.assertRaises(TypeError):
            decode(self.one(list(b'a'), 1))     # no magic given
        self.assertTrue(issubclass(BpeError, ValueError))
        self.assertIs(bpe_container.BpeError, BpeError)


if __name__ == '__main__':
    unittest.main()
