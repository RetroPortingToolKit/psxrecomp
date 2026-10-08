"""Stock-disc metadata for title-owned resident loaders; never stores asset bytes.

Use aot_overlay_pipeline.Disc (or its small reader/files interface). Titles own
their selection policy, loader ABI and emitted C/C++ layout. Hash the complete
sector payload, including the disc's actual final-sector padding, exactly as
the runtime resident store does.
"""
import hashlib
import struct
from typing import NamedTuple


class ResidentCatalogEntry(NamedTuple):
    path: str
    lba: int
    size: int
    sha256: str

    @property
    def padded_size(self):
        return (self.size + 2047) // 2048 * 2048


def collect_entries(disc, include):
    """Return selected metadata in stable LBA/path order.

    `include(path)` is title-owned: audio, movie and executable exclusions are
    not a universal policy. Reads are bounded to each selected file's sectors.
    Invalid extents and short reads fail generation rather than certify a hash.
    """
    entries = []
    for path, (lba, size) in sorted(disc.files.items(), key=lambda item: (item[1][0], item[0])):
        if not include(path):
            continue
        if lba < 0 or size <= 0 or size > 0xFFFFFFFF:
            raise ValueError(f'{path}: invalid resident extent')
        padded = (size + 2047) // 2048 * 2048
        raw = disc.reader.read_file_bytes(lba, padded)
        if len(raw) != padded:
            raise ValueError(f'{path}: short resident sector read')
        entries.append(ResidentCatalogEntry(path, lba, size, hashlib.sha256(raw).hexdigest()))
    return entries


def executable_range_sha256(exe, ranges):
    """Hash declared guest-code ranges in order, bounded to a PS-X EXE image."""
    if len(exe) < 0x800 or exe[:8] != b'PS-X EXE':
        raise ValueError('Expected an original PS-X EXE')
    base, text_size = struct.unpack_from('<II', exe, 0x18)
    if text_size > len(exe) - 0x800:
        raise ValueError('Truncated PS-X EXE text')
    digest = hashlib.sha256()
    for lo, hi in ranges:
        if lo < base or hi <= lo or hi - base > text_size:
            raise ValueError(f'Code guard outside PS-X EXE text: {lo:#x}..{hi:#x}')
        digest.update(exe[0x800 + lo - base:0x800 + hi - base])
    return digest.hexdigest()
