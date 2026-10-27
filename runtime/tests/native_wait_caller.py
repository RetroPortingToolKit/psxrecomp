"""Compare actual regenerated SDK callers with the shared native induction path.

Only title-owned addresses and caller metadata are committed. Retail-derived
function text is read from locally generated input and kept in temporary files.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--framework', type=Path, required=True)
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--contract', type=Path, required=True)
    parser.add_argument('--cc', required=True)
    args = parser.parse_args()
    contract = json.loads(args.contract.read_text())
    symbol = contract['symbol']
    if not re.fullmatch(r'func_[0-9A-F]{8}', symbol):
        raise ValueError('Expected an audited generated function symbol')
    bodies = []
    for path in sorted(args.generated.glob('*full*.c')):
        match = re.search(r'\nvoid '+re.escape(symbol)+r'\(.*?(?=\nvoid |\Z)',
                          path.read_text(), re.S)
        if match:
            bodies.append(match[0])
    if not bodies:
        print('SKIP: regenerate the supported original executable first')
        return 77
    if len(bodies) != 1:
        raise ValueError('Expected one actual generated caller')
    fields = {'ENTRY': contract['entry'], 'END': contract['end'],
              'COUNTER': contract['counter'],
              **{f'SITE_{i}': value for i, value in enumerate(contract['sites'])},
              **{f'CALLBACK_{i}': value for i, value in enumerate(contract['callbacks'])}}
    if len(contract['sites']) != 2 or len(contract['callbacks']) != 3:
        raise ValueError('This fixture covers two-edge, three-timeout-callback waits')
    framework = args.framework.resolve()
    with tempfile.TemporaryDirectory(prefix='psx-native-wait-caller-') as temp:
        work = Path(temp)
        (work/'wait.inc').write_text(bodies[0])
        header = '#define WAIT_FUNCTION '+symbol+'\n'
        for key, raw in fields.items():
            value = int(raw, 0) if isinstance(raw, str) else int(raw)
            if not 0 <= value <= 0xFFFFFFFF:
                raise ValueError('Address outside the guest ABI')
            header += f'#define WAIT_{key} 0x{value:08X}u\n'
        (work/'wait_contract.h').write_text(header)
        executable = work/'contract.exe'
        sources = [str(framework/'runtime/tests/native_wait_caller.c'),
                   str(framework/'runtime/src/psx_cycles.c'),
                   str(framework/'runtime/src/psx_icache.c')]
        includes = [str(work), str(framework/'runtime/include'), str(framework/'runtime/tests')]
        if Path(args.cc).name.lower() in ('cl', 'cl.exe'):
            command = [args.cc, '/nologo', '/std:c11', '/O2', '/DPSX_ENABLE_BLOCK_CYCLES=1',
                       '/DPSX_NO_DEBUG_TOOLS=1', *['/I'+p for p in includes],
                       *sources, '/Fe:'+str(executable)]
        else:
            command = [args.cc, '-std=c11', '-O2', '-DPSX_ENABLE_BLOCK_CYCLES=1',
                       '-DPSX_NO_DEBUG_TOOLS=1', *['-I'+p for p in includes],
                       *sources, '-o', str(executable)]
        subprocess.run(command, cwd=work, check=True)
        subprocess.run([str(executable)], cwd=work, check=True, timeout=30)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
