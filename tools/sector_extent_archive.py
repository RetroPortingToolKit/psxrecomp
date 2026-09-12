"""Strict {sector offset, byte size} archives with an in-file index.

Offsets are relative to the start of the archive, not the end of its table.
Payloads are consecutive after sector rounding; the complete file must be
accounted for. Recognition says nothing about executable code or RAM addresses.
"""
import struct


def extract_members(data, *, sector_size=2048, table_offset=0, count=None):
    if sector_size < 8 or sector_size & (sector_size - 1):
        raise ValueError('Sector size must be a power of two of at least eight')
    if table_offset < 0 or table_offset % 4 or table_offset + 8 > len(data):
        raise ValueError('Invalid archive table offset')
    if not data or len(data) % sector_size:
        raise ValueError('Archive must be sector aligned')
    first_sector = struct.unpack_from('<I', data, table_offset)[0]
    first_payload = first_sector * sector_size
    if not table_offset + 8 <= first_payload < len(data):
        raise ValueError('Archive header overlaps or exceeds payload')
    if count is not None and (count < 1 or table_offset + (count + 1) * 8 > first_payload):
        raise ValueError('Invalid archive member count')
    members, cursor, at = [], first_payload, table_offset
    while at + 8 <= first_payload:
        sector, size = struct.unpack_from('<II', data, at)
        at += 8
        if sector == size == 0:
            break
        if count is not None and len(members) >= count:
            raise ValueError('Archive member count changed')
        offset = sector * sector_size
        if not size or offset != cursor or offset + size > len(data):
            raise ValueError('Archive extent has a gap, overlap, or exceeds bounds')
        members.append(dict(index=len(members), sector=sector, source_offset=offset,
                            body=data[offset:offset + size]))
        cursor = (offset + size + sector_size - 1) // sector_size * sector_size
    else:
        raise ValueError('Archive table has no terminator')
    if not members or (count is not None and len(members) != count):
        raise ValueError('Archive member count changed')
    if cursor != len(data):
        raise ValueError('Archive extents do not cover the file')
    return members
