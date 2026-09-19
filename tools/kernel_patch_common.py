#!/usr/bin/env python3
"""kernel_patch_common.py - shared plumbing for the kernel install-slot tools.

Both kernel_patch_diff.py (which kernel-RAM words does this game patch?) and
kernel_patch_ab.py (what do the declared ranges buy?) run a GAME project
headless and talk to its debug server. This module holds the three things they
would otherwise each get subtly wrong: locating the project's exe, reading the
BIOS profile, and reading the emitted tables back out of the generated C.

Run both tools from a game project root (the directory holding game.toml and
the framework checkout), not from the framework:

    python psxrecomp/tools/kernel_patch_diff.py
    python psxrecomp/tools/kernel_patch_ab.py --bios psxrecomp/bios/SCPH1001.BIN

See docs/dynamic_handler_install.md for what the ranges are and how to find
them for a new image.
"""
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import debug_client

# Kernel RAM below this is the relocated BIOS copy (dirty_ram_interp.h's
# DIRTY_RAM_KERNEL_WINDOW_END).
KERNEL_WINDOW_END = 0x10000

# The four 16-byte kernel call vectors, reported on their own line by the A/B.
#
# They are NOT discounted from its headline. An earlier version subtracted them
# on the grounds that they interpret on both sides and would mask the result.
# That is true of some titles and false in general: on Mega Man X6 with
# OpenBIOS the B0 vector burns 2,361,470 instructions over 1,180,735 entries
# with the ranges off and vanishes entirely with them on, because once the
# bless verdict is clean the vector body dispatches native. Subtracting it hid
# a 130 insn/frame win.
VECTOR_STUBS = (0x80, 0xA0, 0xB0, 0xC0)


def add_project_args(ap, default_port):
    """The arguments every kernel-patch tool takes."""
    ap.add_argument("--project-root", default=".",
                    help="game project root (holds game.toml); default cwd")
    ap.add_argument("--framework", default="psxrecomp",
                    help="framework checkout, relative to --project-root")
    ap.add_argument("--build-dir", default="build-relprof",
                    help="build tree to launch from, relative to --project-root")
    ap.add_argument("--exe", default=None,
                    help="runtime exe; default: the single .exe in --build-dir")
    ap.add_argument("--game", default="game.toml",
                    help="game config passed to --game")
    ap.add_argument("--profile", default=None,
                    help="BIOS profile toml; default <framework>/bios/OpenBIOS.toml")
    ap.add_argument("--dispatch", default=None,
                    help="generated <stem>_dispatch.c; default derived from --profile")
    ap.add_argument("--bios", default=None,
                    help="BIOS image to launch with; default = the runtime's own pick")
    ap.add_argument("--port", type=int, default=default_port)
    ap.add_argument("--host", default="127.0.0.1")


def resolve(a):
    """Fill in the derived paths on a parsed args namespace. Returns the root."""
    root = os.path.abspath(a.project_root)
    fw = os.path.join(root, a.framework)
    if not os.path.isdir(fw):
        raise SystemExit("no framework checkout at %s (pass --framework)" % fw)
    if a.profile is None:
        a.profile = os.path.join(a.framework, "bios", "OpenBIOS.toml")
    if a.dispatch is None:
        stem = read_out_stem(os.path.join(root, a.profile))
        a.dispatch = os.path.join(a.framework, "generated", stem + "_dispatch.c")
    if a.exe is None:
        a.exe = find_exe(os.path.join(root, a.build_dir))
    for key in ("profile", "dispatch", "exe"):
        p = os.path.join(root, getattr(a, key))
        if not os.path.exists(p):
            raise SystemExit("--%s not found: %s" % (key, p))
    return root


def find_exe(build_dir):
    """The game's runtime exe. Its name is derived from the window title, so it
    differs per title; glob rather than guess, and refuse to choose."""
    if not os.path.isdir(build_dir):
        raise SystemExit("no build dir at %s (pass --build-dir or --exe)" % build_dir)
    found = [p for p in glob.glob(os.path.join(build_dir, "*.exe"))
             if "_oracle" not in os.path.basename(p)]
    if not found:
        raise SystemExit("no .exe in %s - build psx-runtime first" % build_dir)
    if len(found) > 1:
        names = "\n  ".join(os.path.basename(p) for p in found)
        raise SystemExit("several exes in %s; pass --exe:\n  %s" % (build_dir, names))
    return found[0]


def read_out_stem(profile_path):
    """[recompiler] out_stem from a BIOS profile ("OpenBIOS", "SCPH1001")."""
    txt = open(profile_path, encoding="utf-8").read()
    m = re.search(r'^\s*out_stem\s*=\s*"([^"]+)"', txt, re.M)
    if not m:
        raise SystemExit("%s has no [recompiler] out_stem" % profile_path)
    return m.group(1)


def parse_profile(path):
    """(rom_path, [(name, rom_lo, ram_lo, length, kernel_bless)]) from a profile."""
    txt = open(path, encoding="utf-8").read()
    rom = re.search(r'^rom\s*=\s*"([^"]+)"', txt, re.M).group(1)
    copies = []
    for m in re.finditer(r'\[\[recompiler\.address_model\.copy\]\](.*?)(?=\n\[|\Z)',
                         txt, re.S):
        blk = m.group(1)
        g = lambda k: re.search(r'^\s*%s\s*=\s*"?([^"\n]+)"?' % k,
                                blk, re.M).group(1).strip()
        rom_lo = int(g("rom_lo"), 16)
        rom_hi = int(g("rom_hi"), 16)
        copies.append((g("name"), rom_lo, int(g("ram_lo"), 16),
                       rom_hi - rom_lo, "true" in g("kernel_bless")))
    return rom, copies


def parse_bodies(dispatch_c):
    """[(key, body_lo, body_hi)] from the emitted PsxKernelBody table."""
    txt = open(dispatch_c, encoding="utf-8", errors="replace").read()
    m = re.search(r'psx_bios_kernel_bodies\[\d+\]\s*=\s*\{(.*?)\};', txt, re.S)
    if not m:
        return []
    return [(int(a, 16), int(b, 16), int(c, 16)) for a, b, c in
            re.findall(r'\{\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u,'
                       r'\s*0x([0-9A-Fa-f]+)u\s*\}', m.group(1))]


def parse_patch_ranges(dispatch_c):
    """[(lo, hi)] from the emitted PsxKernelPatchRange table.

    The profile's [[recompiler.install_slots]], couriered into the generated C.
    A differing word INSIDE one of these is a declared patch: the bless
    verifier skips it and the body still runs native. A differing word outside
    every range is what actually unblesses a body. Empty for a backend
    generated before install-slot ranges existed.
    """
    txt = open(dispatch_c, encoding="utf-8", errors="replace").read()
    m = re.search(r'psx_bios_kernel_patch_ranges\[\d+\]\s*=\s*\{(.*?)\};', txt, re.S)
    if not m:
        return []
    pairs = [(int(a, 16), int(b, 16)) for a, b in
             re.findall(r'\{\s*0x([0-9A-Fa-f]+)u,\s*0x([0-9A-Fa-f]+)u\s*\}',
                        m.group(1))]
    # An image with no slots emits one inert { 0, 0 } entry and a count of 0.
    cnt = re.search(r'psx_bios_kernel_patch_range_count\s*=\s*(\d+)u', txt)
    return pairs[:int(cnt.group(1))] if cnt else pairs


def q(cmd, host, port, timeout=120.0, **kw):
    """One debug-server command. Bulk commands need the long default."""
    return debug_client.query(host, port, dict(cmd=cmd, **kw), timeout=timeout)
