"""Reconstruct MIPS images with a trailing two-bit tagged relocation stream.

The first little-endian word is the byte offset of the stream and therefore
the retained image size. Stream words encode an aligned image offset plus:
0 = ABS32, 1 = HI16 with a following full addend, 2 = LO16, 3 = J26.
0xffffffff ends the stream. This format carries no load address: callers must
establish each destination independently from the original loader.
"""
from dataclasses import dataclass
import struct


@dataclass(frozen=True)
class Relocation:
    offset: int
    kind: int
    addend: int | None = None


def parse(data: bytes) -> tuple[bytes, tuple[Relocation, ...]]:
    """Validate the entire file and return its retained image and relocations."""
    if len(data) < 8 or len(data) % 4:
        raise ValueError("Relocation file must contain aligned header and terminator")
    end = struct.unpack_from("<I", data)[0]
    if end < 8 or end % 4 or end > len(data) - 4:
        raise ValueError("Invalid relocation stream offset")
    cursor, relocations, written = end, [], set()
    while cursor <= len(data) - 4:
        tag = struct.unpack_from("<I", data, cursor)[0]
        cursor += 4
        if tag == 0xFFFFFFFF:
            if cursor != len(data):
                raise ValueError("Unaccounted bytes after relocation terminator")
            return data[:end], tuple(relocations)
        offset, kind, addend = tag & ~3, tag & 3, None
        if offset < 4 or offset + 4 > end:
            raise ValueError("Relocation target outside retained image")
        if offset in written:
            raise ValueError("Repeated relocation target")
        written.add(offset)
        if kind == 1:
            if cursor > len(data) - 4:
                raise ValueError("Truncated HI16 addend")
            addend = struct.unpack_from("<I", data, cursor)[0]
            cursor += 4
        relocations.append(Relocation(offset, kind, addend))
    raise ValueError("Missing relocation terminator")


def relocate(data: bytes, load_addr: int) -> tuple[bytes, tuple[Relocation, ...]]:
    """Apply the loader's 32-bit and halfword writes at a verified RAM base."""
    source, relocations = parse(data)
    if load_addr % 4 or not 0x80000000 <= load_addr < load_addr + len(source) <= 0x80200000:
        raise ValueError("Relocated image must fit aligned PlayStation main RAM")
    image = bytearray(source)
    for item in relocations:
        offset = item.offset
        if item.kind == 0:
            word = struct.unpack_from("<I", image, offset)[0]
            struct.pack_into("<I", image, offset, (word + load_addr) & 0xFFFFFFFF)
        elif item.kind == 1:
            # The original loader adds 0x8000 before extracting the high word
            # because the matching low-half instruction sign-extends its imm.
            value = ((load_addr + item.addend + 0x8000) & 0xFFFFFFFF) >> 16
            struct.pack_into("<H", image, offset, value)
        elif item.kind == 2:
            value = struct.unpack_from("<H", image, offset)[0]
            struct.pack_into("<H", image, offset, (value + load_addr) & 0xFFFF)
        else:
            word = struct.unpack_from("<I", image, offset)[0]
            # This is addition to the complete instruction word, matching the
            # loader, rather than masking or replacing the encoded jump field.
            value = word + (((load_addr << 4) & 0xFFFFFFFF) >> 6)
            struct.pack_into("<I", image, offset, value & 0xFFFFFFFF)
    return bytes(image), relocations
