"""Execute the production load window across real IRQ/ChangeThread delivery.

The existing exception guest runs ordinary, taken and thread-switch edges.
Extract the independent pipeline section verbatim from dirty_ram_interp.c so
the fixture's synthetic guest memory does not also link the entire interpreter.
--interrupt-root allows an executing control against the unrepaired IRQ code.
"""
import argparse
from pathlib import Path
import re
import tempfile
from fixture_link import build_and_run


def provider_source(runtime):
    source = (runtime / 'src/dirty_ram_interp.c').read_text(encoding='utf-8')
    start = source.index('enum { LD_ARMED =')
    end = source.index('void dirty_ram_irq_ambient_resync_after_restore', start)
    same_pc = re.search(r'static int dirty_ram_same_pc\(uint32_t a, uint32_t b\) \{[^}]+\}', source)
    assert same_pc
    return ('#include "cpu_state.h"\n#include "dirty_ram_interp.h"\n'
            '#include <string.h>\n' + source[start:end] + same_pc.group(0) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', default='gcc')
    ap.add_argument('--interrupt-root', type=Path)
    args = ap.parse_args()
    here = Path(__file__).resolve().parent
    runtime = here.parent
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        provider = work / 'production_load_window.c'
        provider.write_text(provider_source(runtime), encoding='utf-8')
        modules = ['interrupts.c', 'traps.c', 'psx_bios_backend.c', str(provider)]
        # Current headers/provider with the old actual delivery code is the
        # fail-before control; no simulation of IRQ retirement in the guest.
        if args.interrupt_root:
            modules[0] = str(args.interrupt_root.resolve() / 'src/interrupts.c')
        for opt in ('-O0', '-O2'):
            case = work / opt[1:]
            case.mkdir()
            print(build_and_run(args.cc, here, runtime, opt, case, modules,
                                'test_changethread_in_handler.c',
                                fixture_defines=['FX_REAL_LOAD_DELAY=1']).rstrip(), flush=True)
    print('PASS: old operand on untaken edge; actual IRQ/thread exit retires pending load')


if __name__ == '__main__':
    main()
