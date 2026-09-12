"""Strict extraction of u32 packed (sector count, sector offset) descriptors.

Payload files form one logical byte stream, in the caller's declared order.
This is a container method; identifying executable members and their load
addresses belongs to independently verified loader evidence.
"""
import struct


def extract_members(table, payloads, indices, *, sector_size=2048, offset_bits=20,
                    table_offset=0):
    if sector_size <= 0 or sector_size & (sector_size - 1):
        raise ValueError('Sector size must be a positive power of two')
    if not 1 <= offset_bits < 32 or table_offset < 0 or table_offset % 4:
        raise ValueError('Invalid packed sector descriptor layout')
    if len({name for name, _ in payloads}) != len(payloads):
        raise ValueError('Duplicate sector payload')
    spans, cursor = [], 0
    for name, body in payloads:
        if not body or len(body) % sector_size:
            raise ValueError('Sector payload must be nonempty and sector aligned')
        spans.append((cursor, cursor + len(body), name, body))
        cursor += len(body)
    members, seen = [], set()
    for index in indices:
        if index in seen or index < 0:
            raise ValueError('Duplicate or negative sector table index')
        seen.add(index)
        at = table_offset + index * 4
        if at > len(table) - 4:
            raise ValueError('Sector descriptor outside table')
        descriptor = struct.unpack_from('<I', table, at)[0]
        start = (descriptor & ((1 << offset_bits) - 1)) * sector_size
        size = (descriptor >> offset_bits) * sector_size
        if not size:
            raise ValueError('Empty sector member')
        owner = next((span for span in spans if span[0] <= start < start + size <= span[1]), None)
        if owner is None:
            raise ValueError('Sector member crosses or exceeds payload bounds')
        lo, _, name, body = owner
        offset = start - lo
        members.append(dict(index=index, logical_offset=start, source_file=name,
                            source_offset=offset, body=body[offset:offset + size]))
    return members
