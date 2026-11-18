"""Compare two save-state images section by section, after decompression.

Exit status 0 when the images are identical, 2 when they differ. A malformed
image (bad magic, a repeated or truncated section, trailing bytes) is an error.
"""
import argparse
import json
from pathlib import Path
import struct
import zlib

MAGIC = 0x50535842  # "PSXB", BOOT_STATE_MAGIC
HEADER_WORDS = 9
SECTION_ZLIB = 1

# Tag -> name, mirroring the BS_SEC_* enum in runtime/include/boot_state.h.
NAMES = {0x01: 'CPU', 0x02: 'RAM', 0x03: 'SPAD', 0x04: 'IRQ', 0x05: 'TIMER', 0x06: 'CLOCK',
         0x07: 'GPU', 0x08: 'VRAM', 0x09: 'SPU', 0x0A: 'SPURAM', 0x0B: 'CDROM', 0x0C: 'DMA',
         0x0D: 'SIO', 0x0E: 'DIRTY', 0x0F: 'MDEC', 0x10: 'ICACHE', 0x11: 'MODMEM',
         0x12: 'GAME_START', 0x13: 'GCS', 0x14: 'CPUTIM', 0x15: 'SPUCD', 0x16: 'MOD_HOST',
         0x17: 'CPU_EXEC', 0x18: 'SCHED', 0x1A: 'MEMCTRL', 0x1F: 'IRQ_TIMING'}
MAX_SECTION_BYTES = 64 * 1024 * 1024


def sections(path):
    """(header words, {tag: raw payload}) of one image."""
    data = Path(path).read_bytes()
    if len(data) < HEADER_WORDS * 4:
        raise ValueError(f'{path}: shorter than a header')
    header = struct.unpack_from(f'<{HEADER_WORDS}I', data)
    if header[0] != MAGIC:
        raise ValueError(f'{path}: not a save-state image')
    result, offset = {}, HEADER_WORDS * 4
    for _ in range(header[7]):
        if offset + 16 > len(data):
            raise ValueError(f'{path}: truncated section header')
        tag, flags, size = struct.unpack_from('<IIQ', data, offset)
        offset += 16
        payload = data[offset:offset + size]
        if tag in result or flags not in (0, SECTION_ZLIB) or len(payload) != size:
            raise ValueError(f'{path}: repeated, unsupported or truncated section {tag:#x}')
        offset += size
        if flags == SECTION_ZLIB:
            if size < 4:
                raise ValueError(f'{path}: truncated compressed section {tag:#x}')
            length, = struct.unpack_from('<I', payload)
            decoder = zlib.decompressobj()
            raw = decoder.decompress(payload[4:], min(length, MAX_SECTION_BYTES) + 1)
            if len(raw) != length or not decoder.eof or decoder.unused_data:
                raise ValueError(f'{path}: invalid compressed section {tag:#x}')
            payload = raw
        result[tag] = payload
    if offset != len(data):
        raise ValueError(f'{path}: trailing bytes after the last section')
    return header, result


def compare(left, right):
    """{'identical': bool, 'differences': [...]}; each difference names a
    section and the first differing byte offset in its uncompressed payload."""
    lh, ls = sections(left)
    rh, rs = sections(right)
    differences = []
    if lh != rh:
        differences.append({'section': 'HEADER', 'left': list(lh), 'right': list(rh)})
    for tag in sorted(ls.keys() | rs.keys()):
        a, b = ls.get(tag), rs.get(tag)
        if a == b:
            continue
        a, b = a or b'', b or b''
        offset = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
        differences.append({'section': NAMES.get(tag, f'{tag:#04x}'), 'offset': offset,
                            'left_bytes': len(a) if tag in ls else None,
                            'right_bytes': len(b) if tag in rs else None})
    return {'identical': not differences, 'differences': differences}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('left', type=Path)
    parser.add_argument('right', type=Path)
    args = parser.parse_args()
    result = compare(args.left, args.right)
    print(json.dumps(result))
    return 0 if result['identical'] else 2


if __name__ == '__main__':
    raise SystemExit(main())
