"""Compile the actual emitted flat owner with controlled backend callbacks."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, name):
    start = source.index('static void ' + name + '(')
    brace = source.index('{', start)
    depth = 0
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]', re.S)
    for match in tokens.finditer(source, brace):
        if match[0] == '{': depth += 1
        if match[0] == '}':
            depth -= 1
            if depth == 0: return source[start:match.end()] + '\n'
    raise RuntimeError('unterminated generated owner')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--dispatch', type=Path)
    parser.add_argument('--bios-recompiler', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='psx-static-owner-') as tmp:
        work = Path(tmp)
        dispatch = args.dispatch
        if dispatch is None:
            if args.bios_recompiler is None: parser.error('--dispatch or --bios-recompiler is required')
            subprocess.run([str(args.bios_recompiler), '--config', 'bios/OpenBIOS.toml',
                            '--out-dir', str(work / 'generated')], cwd=root, check=True,
                           stdout=subprocess.DEVNULL)
            dispatch = work / 'generated/OpenBIOS_dispatch.c'
        source = dispatch.read_text(encoding='utf-8')
        if 'PSX_NATIVE_STATIC_OWNER_V1' not in source:
            raise RuntimeError('regenerate BIOS output with the current emitter first')
        owner = function(source, 'psx_dispatch_check_return_boundary') + function(source, 'psx_dispatch_impl')
        # Substitute the table size together with the fixture's one-entry BIOS
        # table. The emitted control flow and hook/backend order stay intact.
        owner, changed = re.subn(r'int lo = 0, hi = \d+ - 1;', 'int lo = 0, hi = 1 - 1;', owner)
        assert changed == 1
        (work / 'owner.inc').write_text(owner, encoding='utf-8')
        for opt in ('-O0', '-O2'):
            for native, module in ((0, 0), (1, 0), (1, 1)):
                exe = work / ('owner.exe' if os.name == 'nt' else 'owner')
                cmd = [args.cc, '-std=c11', opt, '-DPSX_HAS_GAME_DISPATCH',
                       f'-DPSX_NATIVE_STATIC_CALLS={native}', '-I', str(work),
                       '-I', str(root / 'runtime/include')]
                if module: cmd.append('-DPSX_BIOS_MODULE_BUILD')
                subprocess.run(cmd + [str(root / 'recompiler/tests/test_static_dispatch_owner.c'),
                                      '-o', str(exe)], check=True)
                subprocess.run([str(exe)], check=True, timeout=30)
    print('PASS: actual emitted static-owner reference/native/module O0/O2')


if __name__ == '__main__': main()
