"""Block-wise byte-pair-expansion ("BPE") containers.

Some PlayStation engines pack their executable image and overlays with a
byte-pair coder: a table of 256 optional (left, right) pairs per block, then a
stream of symbols each expanded through that table. The container is

    u32  decoded size            (all blocks together)
    u32  magic                   (a title-declared constant, "BPE\\x1a" in the
                                  titles seen so far)
    block*:
        u32  block size          bytes this block decodes to (0 ends the container)
        table                    256 entries, run-length coded (below)
        symbols                  as many input bytes as it takes to produce
                                 `block size` output bytes, each expanded
                                 through the table

The decoder reads its input as little-endian 32-bit words, handing the words
out as bytes. Anything left unread in the last word of a table or block is
discarded, so every block header starts on a word boundary; `consumed` counts
whole words including the terminating zero count.

Table coding, entry index `c` starting at 0 until it reaches 256:
  * a byte `n` >= 0x80 first skips `n - 0x7F` entries (they stay identity
    pairs) and is then treated as `n = 0`;
  * the next `n + 1` entries are given in turn, each by a byte `b` that sets
    `left[c] = b` and, if `b != c`, a second byte that sets `right[c]`. An
    entry with `left[c] == c` is a literal.
Expanding a symbol `b`: while `left[b] != b`, the byte `right[b]` is
queued to follow, and `b` becomes `left[b]`; the final `b` is emitted, then the
queued bytes are expanded the same way, nearest first. A queued byte is
processed before any unread input. Each emitted byte counts against the block
size, whether it came from a symbol or from the queue.

This module is a pure function of its input. It rejects truncated input, a
declared size that the blocks do not produce, a different magic, and tables
that never finish expanding, instead of returning partial output.
"""
import struct


class BpeError(ValueError):
    """A container is truncated, inconsistent, or not the declared format."""


def decode(data, *, magic, offset=0, max_output=1 << 28):
    """Decode the container at `offset`. Returns (output bytes, consumed bytes).

    `magic` is the second header word the title's loader checks; it is a
    required argument because it is evidence about the title, not a default.
    """
    if offset < 0 or offset % 4 or len(data) - offset < 12:
        raise BpeError('BPE container is truncated or misaligned')
    declared, found = struct.unpack_from('<II', data, offset)
    if found != magic:
        raise BpeError(f'BPE magic {found:#010x} is not the declared {magic:#010x}')
    if declared > max_output:
        raise BpeError('BPE declared size exceeds the output limit')
    end = len(data)
    position = offset + 8
    output = bytearray()

    def word():
        nonlocal position
        if position + 4 > end:
            raise BpeError('BPE container is truncated')
        value = struct.unpack_from('<I', data, position)[0]
        position += 4
        return value

    while True:
        count = word()
        if count == 0:
            break
        if len(output) + count > declared:
            raise BpeError('BPE blocks decode past the declared size')
        left, right = list(range(256)), [0] * 256
        window = []          # next byte to hand out is window[-1]

        def take():
            if not window:
                window.extend(reversed(data[position:position + 4]))
                advance = word          # consume the word through the bounds check
                advance()
            return window.pop()

        index = 0
        while index < 256:
            run = take()
            if run >= 0x80:
                index += run - 0x7F
                run = 0
            if index >= 256:
                break
            for _ in range(run + 1):
                if index >= 256:
                    raise BpeError('BPE table run passes its last entry')
                symbol = take()
                left[index] = symbol
                if symbol != index:
                    right[index] = take()
                index += 1
        budget = (count + 1) * 256     # a table that never reaches a literal
        for _ in range(count):
            symbol = take()
            while left[symbol] != symbol:
                window.append(right[symbol])
                symbol = left[symbol]
                budget -= 1
                if budget < 0:
                    raise BpeError('BPE table does not terminate')
            output.append(symbol)
        window.clear()
    if len(output) != declared:
        raise BpeError(f'BPE decoded {len(output):#x} bytes, header declares {declared:#x}')
    return bytes(output), position - offset
