"""Offset-indexed packs whose members are stored verbatim or as bit-stream LZSS.

Container layout (all integers little-endian):

* ``count_offset``: u32 member count. Every other byte before the end of the
  descriptor table is unknown to this reader and must be zero.
* ``table_offset``: ``count`` descriptors ``{u32 offset, u32 size, u32 stored}``.
  ``stored == size`` means the member is stored verbatim; otherwise its
  ``stored`` bytes are an LZSS stream that decodes to exactly ``size`` bytes.
* Members follow the table in descriptor order, each starting at the next
  ``alignment`` boundary. Every byte after the table is a member or zero
  padding (including any tail after the last member, such as the sector fill
  of a pack that a patch shortened), so every byte is accounted for.

The LZSS stream is read one bit at a time, most significant bit first, a byte
at a time. A 1 flag is followed by an 8-bit literal; a 0 flag by a
``position_bits`` window position and a ``length_bits`` count, copying
``count + min_match`` bytes from consecutive window positions. Position 0 ends
the stream. Every output byte is also written to the ring window, starting at
``initial_position``. When ``window_fill`` is None the decoder's window starts
uninitialized, so reading a position that was never written depends on
pre-existing RAM and fails extraction.

No emulated RAM, captured code or game-specific names are inputs.
"""
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def decode(data, offset, *, position_bits, length_bits, min_match, initial_position,
           window_fill=None, max_output=4 << 20):
    """Return (decoded bytes, stream bytes consumed from ``offset``)."""
    require(1 <= position_bits <= 16 and 1 <= length_bits <= 8 and min_match >= 1,
            'Unsupported LZSS field widths')
    size = 1 << position_bits
    require(0 < initial_position < size, 'LZSS initial position outside window')
    require(window_fill is None or 0 <= window_fill <= 255, 'Invalid LZSS window fill')
    require(0 <= offset <= len(data) and max_output > 0, 'Invalid LZSS stream offset or limit')
    window = [window_fill] * size
    cursor, position, current, mask = initial_position, offset, 0, 0
    output = bytearray()

    def bits(count):
        nonlocal position, current, mask
        value = 0
        for _ in range(count):
            if not mask:
                require(position < len(data), 'Truncated LZSS stream')
                current, mask = data[position], 0x80
                position += 1
            value = value << 1 | (1 if current & mask else 0)
            mask >>= 1
        return value

    def emit(value):
        nonlocal cursor
        require(len(output) < max_output, 'LZSS output exceeds limit')
        output.append(value)
        window[cursor] = value
        cursor = (cursor + 1) & (size - 1)

    while True:
        if bits(1):
            emit(bits(8))
            continue
        source = bits(position_bits)
        if source == 0:
            return bytes(output), position - offset
        for step in range(bits(length_bits) + min_match):
            value = window[(source + step) & (size - 1)]
            # The original decoder's window is not cleared: a slot never
            # written by this stream holds whatever RAM held before.
            require(value is not None, 'LZSS reference depends on external RAM')
            emit(value)


def members(data, *, count_offset, table_offset, alignment, lzss, count=None):
    """Decode and account for every member; return them in descriptor order."""
    require(alignment >= 1 and alignment & (alignment - 1) == 0,
            'Pack alignment must be a power of two')
    require(0 <= count_offset and count_offset + 4 <= len(data), 'Pack count outside file')
    actual = struct.unpack_from('<I', data, count_offset)[0]
    require(actual > 0 and (count is None or actual == count), 'Pack member count changed')
    table_end = table_offset + actual * 12
    require(0 <= table_offset and table_end <= len(data), 'Pack descriptor table outside file')
    require(not (count_offset < table_end and table_offset < count_offset + 4),
            'Pack count overlaps descriptor table')
    header = bytearray(data[:table_end])
    header[count_offset:count_offset + 4] = bytes(4)
    header[table_offset:table_end] = bytes(table_end - table_offset)
    require(not any(header), 'Unexpected pack header bytes')
    align = lambda value: (value + alignment - 1) & ~(alignment - 1)
    cursor, result = table_end, []
    for index in range(actual):
        offset, size, stored = struct.unpack_from('<III', data, table_offset + index * 12)
        require(offset == align(cursor) and not any(data[cursor:offset]),
                'Pack member has a gap, overlap or nonzero padding')
        require(size > 0 and stored > 0 and offset + stored <= len(data),
                'Pack member exceeds file or is empty')
        if stored == size:
            body, compressed = data[offset:offset + size], False
        else:
            body, consumed = decode(data, offset, **lzss, max_output=size)
            require(len(body) == size and consumed == stored,
                    f'Pack member {index} does not decode to its declared sizes')
            compressed = True
        result.append(dict(index=index, source_offset=offset, size=size, stored=stored,
                           compressed=compressed, body=body))
        cursor = offset + stored
    require(not any(data[cursor:]), 'Pack has nonzero bytes outside its members')
    return result
