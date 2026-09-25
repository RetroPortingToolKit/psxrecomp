#!/usr/bin/env python3
import base64
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import enrich_overlay_captures as MOD  # noqa: E402

LOAD = 0x80010000
JR_RA = 0x03E00008
FRAME = 0x27BDFFE0
NOP = 0


def image(words):
    return b''.join(struct.pack('<I', word) for word in words)


class EnrichCaptureTests(unittest.TestCase):
    def record(self, words):
        data = image(words)
        return {'load_addr': f'0x{LOAD:08X}', 'size': len(data),
                'bytes_b64': base64.b64encode(data).decode('ascii'),
                'function_entry_pcs': [], 'dispatch_entry_pcs': []}

    def test_direct_jal_and_prologue_are_discovered(self):
        target = LOAD + 0x20
        jal = 0x0C000000 | ((target >> 2) & 0x03ffffff)
        words = [jal, NOP, JR_RA, NOP, NOP, NOP, NOP, NOP,
                 FRAME, NOP, JR_RA, NOP]
        enriched, added = MOD.enrich_record(self.record(words))
        self.assertEqual(added, 1)
        self.assertEqual(enriched['static_discovery_entry_pcs'],
                         [f'0x{target:08X}'])
        self.assertEqual(enriched['function_entry_pcs'], [])

    def test_dense_pointer_table_recovers_proven_targets_not_table_data(self):
        targets = [LOAD + 0x30, LOAD + 0x40, LOAD + 0x50]
        words = [*targets] + [NOP] * 21
        for target in targets:
            off = (target - LOAD) // 4
            words[off:off + 4] = [FRAME, NOP, JR_RA, NOP]
        enriched, _ = MOD.enrich_record(self.record(words))
        roots = {int(value, 16) for value in enriched['static_discovery_entry_pcs']}
        self.assertTrue(set(targets) <= roots)
        self.assertNotIn(LOAD, roots)

    def test_return_adjacent_pointer_data_is_not_promoted(self):
        words = [FRAME, NOP, JR_RA, NOP,
                 LOAD + 0x20, LOAD + 0x24, LOAD + 0x28,
                 1, 2, 3, 4, 5]
        enriched, added = MOD.enrich_record(self.record(words))
        self.assertEqual(added, 1)  # only the real framed entry at LOAD
        self.assertEqual(enriched['static_discovery_entry_pcs'],
                         [f'0x{LOAD:08X}'])


if __name__ == '__main__':
    unittest.main()
