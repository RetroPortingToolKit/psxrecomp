#!/usr/bin/env python3
"""Recompiler codegen regression test: the dispatch file carries the page CRCs
of the executable it was generated from.

A multi-disc set can carry one program with a per-disc code byte (Star Wars:
Rebel Assault II loads the disc number with a different immediate on each
disc). A build generated from disc 1 and started from disc 2 must not run disc
1's compiled function over the differing page. The runtime finds that page by
comparing the executable it loads with a CRC-32 per 4 KiB RAM page that the
emitter writes here (runtime/include/text_source_guard.h).

This test synthesizes a PS-X EXE whose load address is NOT page-aligned, so the
first and the last page are partial, and asserts that the emitted table, load
address and length are exactly the per-page zlib CRC-32s of the loaded image.

Usage:  python test_source_page_crc_table.py [--recompiler <psxrecomp-game>]
Exit 0 = PASS.
"""
import argparse, os, re, struct, subprocess, tempfile, zlib

LOAD = 0x80017F00          # 0x100 bytes into the first RAM page
PAGE = 4096


def w(words):
    return b"".join(struct.pack("<I", x) for x in words)


def make_psxexe(entry, load, data):
    h = bytearray(2048)
    h[0:8] = b"PS-X EXE"
    struct.pack_into("<I", h, 0x10, entry)
    struct.pack_into("<I", h, 0x18, load)
    struct.pack_into("<I", h, 0x1C, len(data))
    struct.pack_into("<I", h, 0x30, 0x801FFFF0)
    # Header padding the console never loads; it must not reach the table.
    h[0x400:0x800] = bytes((i * 7 + 3) & 0xFF for i in range(0x400))
    return bytes(h) + data


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def build_image():
    # func A @ LOAD: addiu sp,-8 ; jal B ; nop ; addiu sp,8 ; jr ra ; nop
    # func B @ LOAD+0x20: jr ra ; nop
    a = [0x27BDFFF8, jal(LOAD + 0x20), 0x00000000, 0x27BD0008, 0x03E00008, 0x00000000]
    body = bytearray(w(a))
    body += b"\x00" * (0x20 - len(body))
    body += w([0x03E00008, 0x00000000])
    # Data that fills the partial first page, two full pages and a partial last one.
    total = 0x100 + 2 * PAGE + 0x800
    body += bytes(((i * 2654435761) >> 11) & 0xFF for i in range(len(body), total))
    return bytes(body)


def expected_crcs(image, load):
    out, pos, addr = [], 0, load & 0x1FFFFFFF
    while pos < len(image):
        take = min(PAGE - (addr % PAGE), len(image) - pos)
        out.append(zlib.crc32(image[pos:pos + take]) & 0xFFFFFFFF)
        pos += take
        addr += take
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--recompiler",
                    default=os.path.normpath(os.path.join(here, "..", "build", "psxrecomp-game")))
    args = ap.parse_args()
    if not os.path.isfile(args.recompiler):
        raise SystemExit("recompiler not found: %s (build it first)" % args.recompiler)

    image = build_image()
    with tempfile.TemporaryDirectory() as tmp:
        psx = os.path.join(tmp, "t.psx")
        seeds = os.path.join(tmp, "seeds.txt")
        out = os.path.join(tmp, "out")
        os.makedirs(out)
        with open(psx, "wb") as f:
            f.write(make_psxexe(LOAD, LOAD, image))
        with open(seeds, "w") as f:
            f.write("0x%08X\n0x%08X\n" % (LOAD, LOAD + 0x20))
        r = subprocess.run([args.recompiler, psx, "--seeds", seeds, "--out-dir", out],
                           capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
        if r.returncode != 0:
            raise SystemExit("recompiler failed:\n" + (r.stderr or r.stdout))
        disp = [f for f in os.listdir(out) if f.endswith("_dispatch.c")]
        if not disp:
            raise SystemExit("no _dispatch.c emitted in " + out)
        with open(os.path.join(out, disp[0]), encoding='utf-8') as f:
            src = f.read()

    table = re.search(r"static const uint32_t k_psx_game_source_page_crc32\[\] = \{(.*?)\n\};",
                      src, re.DOTALL)
    if not table:
        raise SystemExit("the dispatch file has no source page CRC table")
    emitted = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{8})u", table.group(1))]
    want = expected_crcs(image, LOAD)
    if len(want) != 4:
        raise SystemExit("fixture error: expected 4 pages, got %d" % len(want))
    if emitted != want:
        raise SystemExit("page CRCs differ:\n  emitted %s\n  wanted  %s" % (
            ["%08X" % x for x in emitted], ["%08X" % x for x in want]))

    accessor = re.search(r"const uint32_t\* psx_game_source_page_crc32\(uint32_t\* count, "
                         r"uint32_t\* phys_lo, uint32_t\* len\) \{(.*?)\n\}", src, re.DOTALL)
    if not accessor:
        raise SystemExit("psx_game_source_page_crc32 not found in emitted dispatch")
    body = accessor.group(1)
    for needle, what in (("*count = %du;" % len(want), "page count"),
                         ("*phys_lo = 0x%08Xu;" % (LOAD & 0x1FFFFFFF), "physical load address"),
                         ("*len = 0x%Xu;" % len(image), "image length")):
        if needle not in body:
            raise SystemExit("accessor reports the wrong %s (wanted `%s`):\n%s" % (what, needle, body))

    print("source page CRC table test passed (%d pages, unaligned load, partial first and last page)"
          % len(want))


if __name__ == "__main__":
    main()
