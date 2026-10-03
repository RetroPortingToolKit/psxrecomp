"""Read counted image containers with separate two-bit MIPS relocation streams.

A count precedes 12-byte records (image offset, image size, stream offset).
Images follow the records; streams follow the sum of image sizes. Offsets are
relative to their respective areas. The format has no destination addresses.
Callers must establish placement from the original loader before using AOT.
"""
from dataclasses import dataclass
import struct

from mips_tagged_relocations import Relocation


@dataclass(frozen=True)
class Member:
    image_offset: int
    image: bytes
    relocations: tuple[Relocation, ...]
    stream_offset: int
    stream_end: int


def parse(data: bytes) -> tuple[Member, ...]:
    if len(data) < 20 or len(data) % 4:
        raise ValueError('Container must contain aligned records, images and streams')
    count, = struct.unpack_from('<I', data)
    if not 1 <= count <= 4096 or 4 + 12 * count > len(data):
        raise ValueError('Invalid image count')
    records = [struct.unpack_from('<III', data, 4 + 12 * i) for i in range(count)]
    image_start = 4 + 12 * count
    image_bytes = sum(size for _, size, _ in records)
    stream_start = image_start + image_bytes
    if stream_start > len(data) - 4:
        raise ValueError('Images exceed container bounds')
    images, streams, result = [], [], []
    for offset, size, stream in records:
        if offset % 4 or size < 4 or size % 4 or offset + size > image_bytes:
            raise ValueError('Invalid image interval')
        images.append((offset, offset + size))
        cursor = stream_start + stream
        if stream % 4 or cursor > len(data) - 4:
            raise ValueError('Invalid relocation stream offset')
        start, written, relocations = cursor, set(), []
        while True:
            if cursor > len(data) - 4:
                raise ValueError('Missing relocation terminator')
            tag, = struct.unpack_from('<I', data, cursor)
            cursor += 4
            if tag == 0xffffffff:
                break
            target, kind, addend = tag & ~3, tag & 3, None
            if target + 4 > size or target in written:
                raise ValueError('Invalid or repeated relocation target')
            written.add(target)
            if kind == 1:
                if cursor > len(data) - 4:
                    raise ValueError('Truncated HI16 addend')
                addend, = struct.unpack_from('<I', data, cursor)
                cursor += 4
            relocations.append(Relocation(target, kind, addend))
        streams.append((start, cursor))
        result.append(Member(image_start + offset,
                             data[image_start + offset:image_start + offset + size],
                             tuple(relocations), start, cursor))
    for intervals, message in ((images, 'Image intervals do not partition their area'),
                               (streams, 'Relocation streams do not partition their area')):
        intervals.sort()
        expected_start = 0 if intervals is images else stream_start
        expected_end = image_bytes if intervals is images else len(data)
        if intervals[0][0] != expected_start or intervals[-1][1] != expected_end or any(
                left[1] != right[0] for left, right in zip(intervals, intervals[1:])):
            raise ValueError(message)
    return tuple(result)


def relocate(member: Member, load_addr: int) -> bytes:
    if load_addr % 4 or not 0x80000000 <= load_addr < load_addr + len(member.image) <= 0x80800000:
        raise ValueError('Relocated image must fit aligned main RAM')
    image = bytearray(member.image)
    for item in member.relocations:
        word, = struct.unpack_from('<I', image, item.offset)
        if item.kind == 0:
            value = (word + load_addr) & 0xffffffff
        elif item.kind == 1:
            high = ((load_addr + item.addend + 0x8000) & 0xffffffff) >> 16
            value = (word & 0xffff0000) | high
        elif item.kind == 2:
            value = (word & 0xffff0000) | ((word + load_addr) & 0xffff)
        else:
            target = (((word & 0x03ffffff) << 2) + load_addr) & 0xffffffff
            value = (word & 0xfc000000) | ((target >> 2) & 0x03ffffff)
        struct.pack_into('<I', image, item.offset, value)
    return bytes(image)
