# Counted MIPS relocation containers

`tools/counted_mips_relocations.py` reads image containers with this layout:

| Area | Little-endian words |
| --- | --- |
| Header | Member count |
| Records | Image offset, image size, relocation-stream offset for each member |
| Images | Concatenated image bytes |
| Streams | Two-bit tagged relocation words, each stream ending in `0xFFFFFFFF` |

Image offsets are relative to the image area. Stream offsets are relative to
the area following the **sum** of the image sizes. Both areas must partition
exactly; overlapping members, invalid targets, duplicate writes, missing
terminators and unaccounted bytes are rejected.

The tag's low two bits select the operation; the remaining bits are the aligned
image offset. Offset zero is valid. Kind 1 consumes an additional addend word.

| Kind | Operation |
| --- | --- |
| 0 | Add the image's RAM base to an absolute 32-bit word |
| 1 | Preserve the instruction's upper half; insert `(base + addend + 0x8000) >> 16`, with 32-bit wrapping |
| 2 | Preserve the instruction's upper half; add the base to its low half |
| 3 | Preserve the opcode; relocate the masked 26-bit jump target |

```python
from counted_mips_relocations import parse, relocate

members = parse(container_bytes)
image = relocate(members[0], independently_verified_image_base)
```

This is a separate contract from `mips_tagged_relocations.py`: that format's
leading image-size word and full-instruction jump addition do not describe
these containers. Neither reader decompresses data.

The container has **no destination addresses**. `relocate` takes an aligned
KSEG0 main-RAM image base; this is not the allocation base of a container whose
headers remain in memory. Callers must establish placement and executable/data
boundaries from the original loader before constructing native AOT producers.
The decoder alone does not provide dynamically placed native module dispatch.

The implementation was compared byte for byte with the owned USA MediEvil II
relocation routine (`0x80078328`) for 24 modules at two distinct RAM bases:
48 complete-image comparisons passed. Owned bytes and diagnostic captures are
not part of this repository. Synthetic contract tests cover multiple members,
all four relocation kinds, opcode preservation and malformed inputs:

```sh
python -m unittest discover -s tools/tests -p test_counted_mips_relocations.py
```
