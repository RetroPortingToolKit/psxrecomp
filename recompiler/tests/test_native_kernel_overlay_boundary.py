#!/usr/bin/env python3
"""A local overlay jump into an intact native marker must leave the unit."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
LOAD = 0x8000A000
ANCHOR = 0x8000A108

def generate(exe, directory, marker, native, overlay=True, fallthrough=False, anchor=ANCHOR):
    directory.mkdir()
    offset = anchor - LOAD
    body = bytearray(offset + 0x18)
    if fallthrough:
        # Keep the path executable rather than a long zero-padding heuristic.
        # These writes preserve the preceding guest value all the way to A108.
        for position in range(0, offset, 4):
            struct.pack_into('<I', body, position, 0x24630000)
    struct.pack_into('<2I', body, 0, (0x24030009 if fallthrough else 0x08000000 | ((anchor >> 2) & 0x3ffffff)), 0)
    struct.pack_into('<3I', body, offset, marker, 0x03e00008, 0)
    header = bytearray(2048)
    header[:8] = b'PS-X EXE'
    for offset, value in [(0x10, LOAD), (0x18, LOAD), (0x1c, len(body))]:
        struct.pack_into('<I', header, offset, value)
    source = directory / 'prefix.psx'
    source.write_bytes(header + body)
    seeds = directory / 'seeds.txt'
    seeds.write_text('dispatch_root 0x8000A000\n')
    argv = [exe, str(source), '--seeds', str(seeds), '--out-dir', str(directory / 'out'),
            '--project-root', str(ROOT)]
    if overlay:
        argv.append('--overlay')
    if native:
        argv.append('--native-kernel-boundaries')
    env = dict(os.environ, PSX_CPS='1')
    result = subprocess.run(argv, capture_output=True, text=True, env=env)
    assert result.returncode == 0, result.stdout + result.stderr
    files = sorted((directory / 'out').glob('*_full*.c'))
    assert files
    return '\n'.join(f.read_text() for f in files)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--recompiler', required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        # Retain the exception-continuation case and cover an IRQ verifier
        # reached through a normal compiled guest wrapper.
        for anchor in (ANCHOR, 0x8000A12C):
            case = temp / f'{anchor:08X}'
            case.mkdir()
            native = generate(args.recompiler, case / 'native', 0x0000000d, True, anchor=anchor)
            original = generate(args.recompiler, case / 'ordinary', 0x0000000d, False, anchor=anchor)
            edited = generate(args.recompiler, case / 'edited', 0x24020007, True, anchor=anchor)
            static = generate(args.recompiler, case / 'static', 0x0000000d, True, False, anchor=anchor)
            fallthrough = generate(args.recompiler, case / 'fallthrough', 0x0000000d, True, fallthrough=True, anchor=anchor)
            handoff = re.compile(rf'block_{anchor:08X}:\s*cpu->pc = 0x{anchor:08X}u; return;')
            assert handoff.search(native), f'{anchor:08X}: native local jump executes marker instead of returning to dispatch'
            assert handoff.search(fallthrough), f'{anchor:08X}: fallthrough prefix must split before intact native marker'
            assert '0x24030009' in fallthrough, 'preceding guest register write was lost'
            assert not handoff.search(original), 'firmware code unexpectedly selected native ownership'
            assert not handoff.search(edited), 'edited marker must remain ordinary guest code'
            assert not handoff.search(static), 'original static game generation must remain unchanged'
            assert '0x24020007' in edited, 'edited guest body was lost'
    spec = importlib.util.spec_from_file_location('overlay_native_boundary_fixture', ROOT / 'tools/compile_overlays.py')
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    for flavor in [0, 2, 4, 6]:
        argv = module.recompiler_overlay_args(SimpleNamespace(flavor=flavor))
        assert ('--native-kernel-boundaries' in argv) == bool(flavor & 4)
    print('PASS: native marker tail, edited guest marker, ordinary/static controls and all flavor combinations')

if __name__ == '__main__':
    main()
