"""Bounded six-word archive descriptors and 16-bit, LSB-first LZ tokens.

The descriptor is (identity, decoded_size, stored_size, offset, identity,
identity). A zero stored_size denotes an uncompressed member. Offsets are
sector aligned; table order need not match physical order. Placement and code
entry evidence belong to the consuming profile, never this format reader.
"""
from dataclasses import dataclass
import struct


@dataclass(frozen=True)
class Member:
    index: int
    decoded_size: int
    stored_size: int
    source_offset: int
    payload: memoryview


def require(condition, message):
    if not condition:
        raise ValueError(message)


def members(data, *, count=None, alignment=2048):
    require(alignment > 0 and alignment & (alignment - 1) == 0,
            'Archive alignment must be a power of two')
    require(len(data) >= 28, 'Truncated archive header')
    actual, = struct.unpack_from('<I', data)
    require(0 < actual <= 65536 and (count is None or count == actual),
            'Archive member count changed')
    table_end = 4 + actual * 24
    require(table_end <= len(data), 'Truncated archive descriptors')
    first_payload = (table_end + alignment - 1) & -alignment
    require(first_payload <= len(data) and not any(data[table_end:first_payload]),
            'Unexpected archive directory padding')
    view = memoryview(data)
    result, spans = [], []
    for index in range(actual):
        _, decoded, stored, offset, _, _ = struct.unpack_from('<6I', data, 4 + index * 24)
        size = stored or decoded
        require(decoded > 0 and size > 0 and offset >= first_payload and
                offset % alignment == 0 and offset + size <= len(data),
                'Archive member outside aligned payload')
        spans.append((offset, offset + size))
        result.append(Member(index, decoded, stored, offset, view[offset:offset + size]))
    spans.sort()
    require(all(left[1] <= right[0] for left, right in zip(spans, spans[1:])),
            'Overlapping archive members')
    return result


def decode(data, decoded_size, *, max_output=64 * 1024 * 1024):
    require(0 < decoded_size <= max_output, 'Decoded member exceeds output bound')
    output = bytearray()
    cursor, flags = 0, 1
    while len(output) < decoded_size:
        if flags == 1:
            require(cursor + 2 <= len(data), 'Truncated LZ flags')
            flags = data[cursor] | data[cursor + 1] << 8 | 0x10000
            cursor += 2
        match = flags & 1
        flags >>= 1
        if not match:
            require(cursor < len(data), 'Truncated LZ literal')
            output.append(data[cursor])
            cursor += 1
        else:
            require(cursor + 2 <= len(data), 'Truncated LZ match')
            length = (data[cursor] & 15) + 1
            distance = (data[cursor] & 0xf0) << 4 | data[cursor + 1]
            cursor += 2
            require(length >= 2 and 0 < distance <= len(output),
                    'LZ match refers outside decoded history')
            require(len(output) + length <= decoded_size, 'LZ match exceeds decoded size')
            for _ in range(length):
                output.append(output[-distance])
    require(cursor == len(data), 'Unused bytes after compressed member')
    return bytes(output)
