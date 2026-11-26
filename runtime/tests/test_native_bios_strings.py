"""Compare the native kernel's A0 string services with the generated OpenBIOS.

Extracts the audited image's recompiled string routines and links them beside
runtime/src/native_bios_services.c, so both implementations run on the same
inputs. Run with --generated <OpenBIOS_full.c> --output <scratch> --cc <compiler>.
"""
import argparse
from pathlib import Path
import re
import subprocess

AUDITED = 'BIOS SHA256: fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57'
# The audited image's A0 table entries for the services the fixture compares
# (index and strchr share one entry). The fixture declares the same names.
BODIES = ['1FC058C8',  # A0:15 strcat
          '1FC05A44',  # A0:17 strcmp
          '1FC05A68',  # A0:18 strncmp
          '1FC0590C',  # A0:19 strcpy
          '1FC05930',  # A0:1A strncpy
          '1FC059CC',  # A0:1B strlen
          '1FC0596C',  # A0:1C index, A0:1E strchr
          '1FC05858',  # A0:25 toupper
          '1FC05878',  # A0:26 tolower
          '1FC060C4',  # A0:29 bcmp, A0:2D memcmp
          '1FC08674',  # their shared compare loop (tail jump target)
          '1FC05894']  # A0:2E memchr


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = args.generated.read_text(encoding='utf-8')
    if AUDITED not in source:
        raise ValueError('Unaudited BIOS image')
    parts = ['#include "cpu_state.h"\n',
             'extern void psx_check_interrupts_at(CPUState*, uint32_t);\n',
             'extern uint32_t g_debug_last_store_pc;\n']
    for address in BODIES:
        matches = list(re.finditer(r'void OpenBIOS_func_%s\(CPUState\* cpu\)\s*\{' % address, source))
        if len(matches) != 1:
            raise ValueError('Expected one generated body at ' + address)
        start = matches[0].start()
        end = source.find('\nvoid ', start + 1)
        if end < 0:
            raise ValueError('Missing body boundary at ' + address)
        parts.append(source[start:end].replace('void OpenBIOS_func_', 'void reference_', 1) + '\n')
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    reference = out / 'string_reference.c'
    reference.write_text(''.join(parts), encoding='utf-8')
    exe = out / 'native_bios_strings.exe'
    subprocess.run([args.cc, '-std=c11', '-O2', '-DPSX_NO_DEBUG_TOOLS=1',
                    '-I' + str(root / 'runtime/include'),
                    str(root / 'runtime/tests/test_native_bios_strings.c'),
                    str(root / 'runtime/src/native_bios_services.c'), str(reference),
                    '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
