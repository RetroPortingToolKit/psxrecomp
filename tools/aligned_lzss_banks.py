"""Sector-aligned size-pair containers with tagged, compressed code banks.

The compression stream has a parameter byte, a big-endian 24-bit token count
minus one, LSB-first literal flags, and 16-bit big-endian backward references.
The parameter selects distance/length bits and a piecewise length table.
No emulated RAM, captured code, or game-specific names are inputs.
"""
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def decode(data, *, max_output=2_000_000):
    require(len(data) >= 5 and max_output > 0, 'Truncated LZSS stream or invalid limit')
    parameter = data[0]
    require(parameter < 32, 'Unsupported LZSS parameter bits')
    length_bits = 7 - (parameter & 7)
    mask = 127 >> (parameter & 7)
    threshold = 19 if mask >= 31 else mask // 2
    shift = (parameter >> 3) & 3
    lengths = [(i if i <= threshold else threshold + ((i - threshold) << shift)) + 3
               for i in range(mask + 1)]
    tokens = int.from_bytes(data[1:4], 'big') + 1
    require(tokens <= max_output, 'LZSS token count exceeds output limit')
    position, flags = 4, 0
    output = bytearray()

    def byte():
        nonlocal position
        require(position < len(data), 'Truncated LZSS token')
        value = data[position]
        position += 1
        return value

    for _ in range(tokens):
        if flags <= 1:
            flags = byte() | 256
        literal = flags & 1
        flags >>= 1
        token = byte()
        if literal:
            require(len(output) < max_output, 'LZSS output exceeds limit')
            output.append(token)
        else:
            token = token * 256 + byte()
            distance, length = token >> length_bits, lengths[token & mask]
            # A zero/out-of-image reference depends on pre-existing RAM. It
            # cannot produce deterministic original-disc AOT code.
            require(0 < distance <= len(output), 'LZSS reference depends on external RAM')
            require(len(output) + length <= max_output, 'LZSS output exceeds limit')
            for _ in range(length):
                output.append(output[-distance])
    return bytes(output), position


def members(data, *, alignment=2048, version=1):
    require(alignment >= 16 and len(data) >= alignment, 'Truncated size-pair archive')
    actual_version, count = struct.unpack_from('<II', data)
    require(actual_version == version, 'Unsupported size-pair archive version')
    require(0 < count <= (alignment - 8) // 8, 'Invalid size-pair member count')
    require(not any(data[8 + count * 8:alignment]), 'Unexpected archive header tail')
    offset, result = alignment, []
    for index in range(count):
        stored, padded = struct.unpack_from('<II', data, 8 + index * 8)
        require(stored > 0 and padded == (stored + alignment - 1) // alignment * alignment,
                'Invalid aligned member size')
        require(offset + padded <= len(data), 'Truncated aligned member')
        result.append(dict(index=index, offset=offset, size=stored,
                           body=data[offset:offset + stored]))
        offset += padded
    require(offset == len(data), 'Size-pair inventory does not cover archive')
    return result


def banks(data, *, alignment=2048, version=1, bank_tag=0x4B,
          terminal_tag=0x31, data_tags=(0x30,), max_output=2_000_000):
    """Decode only metadata and its explicit leading bank members.

    Remaining members are data payloads, deliberately not decoded as code.
    The caller supplies loader-established bank-index to RAM-address mappings.
    """
    inventory = members(data, alignment=alignment, version=version)
    metadata, _ = decode(inventory[0]['body'], max_output=max_output)
    position, result = 0, []
    while True:
        require(position + 4 <= len(metadata), 'Truncated bank selector metadata')
        tag = struct.unpack_from('<I', metadata, position)[0]
        if tag != bank_tag:
            require(tag == terminal_tag if result else tag in data_tags,
                    'Unknown bank metadata tag')
            break
        require(position + 8 <= len(metadata), 'Truncated bank selector')
        bank = struct.unpack_from('<I', metadata, position + 4)[0]
        index = len(result) + 1
        require(index < len(inventory), 'Bank selector has no archive member')
        member = inventory[index]
        body, consumed = decode(member['body'], max_output=max_output)
        result.append(dict(index=index, bank=bank, source_offset=member['offset'],
                           stored_size=member['size'], consumed=consumed, body=body))
        position += 8
    return inventory, result
