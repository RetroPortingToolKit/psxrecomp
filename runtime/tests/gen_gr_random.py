#!/usr/bin/env python3
"""Generate seeded random renderer recordings and replay them.

Each recording (gpu_render_record.h format, no closing snapshot) starts from
random VRAM and a random high-resolution mirror, then mixes every recorded
renderer call: state, all primitive types, fills, copies, transfers, reads and
display decodes. gr_replay replays each twice and must find the passes
identical on every op.
"""

import argparse
from pathlib import Path
import random
import struct
import subprocess
import sys

VRAM_W, VRAM_H = 1024, 512
OPS = {name: index for index, name in enumerate(
    "END SET_SCALE SET_TEXTURE_FILTER SET_SEMI SET_MASK SET_TEXWIN SET_COLOR_MOD "
    "SET_PRECISE SET_PERSPECTIVE FILL COPY FLAT_TRI GOURAUD_TRI TEX_TRI "
    "SHADED_TEX_TRI FLAT_RECT TEX_RECT TEX_RECT_SCALED LINE SHADED_LINE "
    "RENDER_DISPLAY RENDER_DISPLAY_HIRES VRAM_WRITE VRAM_READ TRANSFER_IN "
    "TRANSFER_OUT SET_DRAW_AREA GET_DRAW_AREA SET_DRAW_OFFSET "
    "UNSUPPORTED WIDE_CONFIGURE WIDE_SET_VIEW WIDE_SET_TARGET "
    "WIDE_DISABLE_TARGET".split())}

# SwStateHeader in gpu_sw_renderer.c.
HEADER = struct.Struct("<8s5i2i4i4B3BB4ii3i3ii3f4i4i")


def state(rng, scale):
    header = HEADER.pack(
        b"PSXSWS1\0", scale, 0, 0, VRAM_W - 1, VRAM_H - 1, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0, 128, 128, 128, 0,
        0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0,
        0, 1.0, 1.0, 1.0,
        0, 0, 255, 255,
        0, 0, 0, 0)
    pixels = VRAM_W * VRAM_H * (1 + (scale * scale if scale > 1 else 0))
    return header + rng.randbytes(pixels * 2)


class Recording:
    def __init__(self, rng):
        self.rng = rng
        self.data = bytearray()

    def op(self, name, *ints):
        self.data += bytes([OPS[name]]) + struct.pack(f"<{len(ints)}i", *ints)

    def coord(self):
        return self.rng.randint(-48, 420)

    def color(self):
        return self.rng.getrandbits(16)

    def texture(self):
        rng = self.rng
        texpage = rng.getrandbits(7) | (rng.randint(0, 2) << 7)
        return rng.randrange(0, VRAM_W, 16), rng.randrange(VRAM_H), texpage


def recording(seed):
    rng = random.Random(seed)
    scale = rng.choice((1, 2, 4))
    out = Recording(rng)
    out.data += b"PSXGRR1\0" + struct.pack("<I", 1)
    opening = state(rng, scale)
    out.data += struct.pack("<I", len(opening)) + opening
    for _ in range(rng.randint(150, 400)):
        kind = rng.randrange(26)
        c = out.coord
        if kind == 0:
            out.op("SET_SEMI", rng.getrandbits(1), rng.randrange(4))
        elif kind == 1:
            out.op("SET_MASK", rng.getrandbits(1), rng.getrandbits(1))
        elif kind == 2:
            out.op("SET_TEXWIN", rng.choice((0, rng.getrandbits(20))))
        elif kind == 3:
            out.op("SET_COLOR_MOD", rng.randrange(256), rng.randrange(256),
                   rng.randrange(256), rng.getrandbits(1))
        elif kind == 4:
            x1, x2 = sorted(rng.randrange(VRAM_W) for _ in range(2))
            y1, y2 = sorted(rng.randrange(VRAM_H) for _ in range(2))
            out.op("SET_DRAW_AREA", x1, y1, x2, y2)
        elif kind == 5:
            out.op("SET_DRAW_OFFSET", rng.randint(-64, 64), rng.randint(-64, 64))
        elif kind == 6:
            out.op("SET_TEXTURE_FILTER", int(rng.random() < 0.2))
        elif kind == 7:
            out.op("SET_PRECISE", rng.getrandbits(1),
                   *(rng.randint(-48 << 16, 420 << 16) for _ in range(6)))
        elif kind == 8:
            out.data += bytes([OPS["SET_PERSPECTIVE"]]) + struct.pack(
                "<i3f", rng.getrandbits(1), *(rng.uniform(0.05, 2.0) for _ in range(3)))
        elif kind == 9:
            out.op("FILL", rng.randrange(VRAM_W), rng.randrange(VRAM_H),
                   rng.randint(0, 96), rng.randint(0, 96), out.color())
        elif kind == 10:
            out.op("COPY", rng.randrange(VRAM_W), rng.randrange(VRAM_H),
                   rng.randrange(VRAM_W), rng.randrange(VRAM_H),
                   rng.randint(0, 64), rng.randint(0, 64))
        elif kind in (11, 12, 13):
            out.op("FLAT_TRI", c(), c(), c(), c(), c(), c(), out.color())
        elif kind in (14, 15):
            out.op("GOURAUD_TRI", c(), c(), out.color(), c(), c(), out.color(),
                   c(), c(), out.color())
        elif kind == 16:
            uv = [rng.randrange(256) for _ in range(6)]
            out.op("TEX_TRI", c(), c(), uv[0], uv[1], c(), c(), uv[2], uv[3],
                   c(), c(), uv[4], uv[5], *out.texture())
        elif kind == 17:
            vertices = []
            for _ in range(3):
                vertices += [c(), c(), rng.randrange(256), rng.randrange(256),
                             rng.getrandbits(24)]
            out.op("SHADED_TEX_TRI", *vertices, *out.texture(), rng.getrandbits(1))
        elif kind in (18, 19, 20):
            out.op("FLAT_RECT", c(), c(), rng.randint(0, 128), rng.randint(0, 128),
                   out.color())
        elif kind == 21:
            size = rng.choice((1, 8, 16, rng.randint(1, 64)))
            out.op("TEX_RECT", c(), c(), size, size, rng.randrange(256),
                   rng.randrange(256), *out.texture())
        elif kind == 22:
            out.op("TEX_RECT_SCALED", c(), c(), rng.randint(1, 96), rng.randint(1, 96),
                   rng.randrange(256), rng.randrange(256), rng.randrange(256),
                   rng.randrange(256), *out.texture())
        elif kind == 23:
            if rng.getrandbits(1):
                out.op("LINE", c(), c(), c(), c(), out.color())
            else:
                out.op("SHADED_LINE", c(), c(), out.color(), c(), c(), out.color())
        elif kind == 24:
            w, h = rng.randint(1, 48), rng.randint(1, 48)
            x, y = rng.randrange(VRAM_W - w), rng.randrange(VRAM_H - h)
            out.op("TRANSFER_IN", x, y, w, h)
            out.data += rng.randbytes(w * h * 2)
        elif kind == 25:
            # Reads compare the two rasterizers, so the recorded values only
            # need the right size; CPU replay reports them as differences.
            choice = rng.randrange(6)
            if choice == 0:
                width, height = rng.choice(((320, 240), (256, 240), (640, 480)))
                hires = rng.getrandbits(1)
                pitch = width * 4 * (scale if hires else 1)
                out.op("RENDER_DISPLAY_HIRES" if hires else "RENDER_DISPLAY", pitch,
                       rng.randrange(0, VRAM_W - width + 1, 8),
                       rng.randrange(0, VRAM_H - height + 1), width, height, -1)
            elif choice in (1, 2):
                out.op("VRAM_WRITE", rng.randrange(VRAM_W), rng.randrange(VRAM_H),
                       out.color())
            elif choice == 3:
                out.op("VRAM_READ", rng.randrange(VRAM_W), rng.randrange(VRAM_H), 0)
            elif choice == 4:
                w, h = rng.randint(1, 32), rng.randint(1, 32)
                out.op("TRANSFER_OUT", rng.randrange(VRAM_W - w), rng.randrange(VRAM_H - h),
                       w, h)
                out.data += bytes(w * h * 2)
            else:
                out.op("GET_DRAW_AREA", 0, 0, 0, 0)
    out.data += bytes([OPS["END"]]) + struct.pack("<I", 0)
    return bytes(out.data)


def check_closing_snapshot(replay, directory):
    opening = state(random.Random(0), 1)
    prefix = b"PSXGRR1\0" + struct.pack("<II", 1, len(opening)) + opening
    empty = prefix + struct.pack("<BI", OPS["END"], 0)
    complete = prefix + struct.pack("<BI", OPS["END"], len(opening)) + opening
    cases = (
        ("complete", complete, [], 0),
        ("missing", empty, [], 1),
        ("synthetic", empty, ["--compare-only"], 0),
        ("trailing", complete + b"extra", [], 1),
    )
    for label, data, options, expected in cases:
        path = directory / f"closing-{label}.grr"
        path.write_bytes(data)
        result = subprocess.run([replay, *options, str(path)], capture_output=True, text=True)
        if result.returncode != expected:
            print(result.stdout + result.stderr, file=sys.stderr, end="")
            raise RuntimeError(f"{path.name}: expected exit {expected}, got {result.returncode}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replay", required=True, help="gr_replay executable")
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--count", type=int, default=48)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    check_closing_snapshot(args.replay, args.out)
    paths = []
    for index in range(args.count):
        path = args.out / f"random-{args.seed + index:04}.grr"
        path.write_bytes(recording(args.seed + index))
        paths.append(str(path))
    return subprocess.run([args.replay, "--compare-only", *paths]).returncode


if __name__ == "__main__":
    sys.exit(main())
