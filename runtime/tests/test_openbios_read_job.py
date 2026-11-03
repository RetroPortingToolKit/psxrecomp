"""Build the native request and the actual generated OpenBIOS reference.

Generated BIOS bodies stay local; no third-party generated body is committed.
Run with --generated <OpenBIOS_full.c> --output <scratch> --cc <compiler>.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--cxx', help='Also compare live callee-save precision with real pgxp.cpp')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = args.generated.read_text(encoding='utf-8')
    if 'BIOS SHA256: fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57' not in source:
        raise ValueError('Unaudited BIOS image')
    matches = list(re.finditer(r'void OpenBIOS_func_1FC03A78\(CPUState\* cpu\)\s*\{', source))
    if len(matches) != 1:
        raise ValueError('Expected one original generated read body')
    start = matches[0].start()
    end = source.find('\nvoid ', start + 1)
    if end < 0:
        raise ValueError('Missing original body boundary')
    body = source[start:end].replace('void OpenBIOS_func_1FC03A78(',
                                    'void psx_lle_OpenBIOS_func_1FC03A78(', 1)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    reference = out / 'read_reference.c'
    reference.write_text('#include "cpu_state.h"\n'
                         'extern void psx_check_interrupts_at(CPUState*, uint32_t);\n'
                         'extern uint32_t g_debug_last_store_pc;\n' + body, encoding='utf-8')
    exe = out / 'read_contract.exe'
    subprocess.run([args.cc, '-std=c11', '-O2', '-DPSX_NO_DEBUG_TOOLS=1',
                    '-I' + str(root / 'runtime/include'),
                    str(root / 'runtime/tests/test_openbios_read_job.c'),
                    str(root / 'runtime/src/openbios_read_job.c'), str(reference),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    if args.cxx:
        objects = []
        for name, path in [('fixture', root / 'runtime/tests/test_openbios_read_job.c'),
                           ('native', root / 'runtime/src/openbios_read_job.c'),
                           ('reference', reference),
                           ('geometry', root / 'runtime/src/psx_ram_geometry.c')]:
            obj = out / (name + '.o')
            subprocess.run([args.cc, '-std=c11', '-O2', '-DPSX_NO_DEBUG_TOOLS=1',
                            '-DPSX_PGXP=1', '-I' + str(root / 'runtime/include'),
                            '-c', str(path), '-o', str(obj)], check=True)
            objects.append(str(obj))
        pgxp = out / 'pgxp.o'
        subprocess.run([args.cxx, '-std=c++17', '-O2', '-I' + str(root / 'runtime/include'),
                        '-c', str(root / 'runtime/src/pgxp.cpp'), '-o', str(pgxp)], check=True)
        precision_exe = out / 'read_precision_contract.exe'
        subprocess.run([args.cxx, *objects, str(pgxp), *(['-static'] if os.name == 'nt' else []),
                        '-o', str(precision_exe)], check=True)
        subprocess.run([str(precision_exe)], check=True)
    print('Generated BIOS SHA256:', hashlib.sha256(args.generated.read_bytes()).hexdigest())


if __name__ == '__main__':
    main()
