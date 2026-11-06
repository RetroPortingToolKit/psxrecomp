"""An exception return resumes the context the TCB holds (exception-context regression)."""
import argparse
from pathlib import Path
import tempfile
from source_fixture_link import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--src-root', type=Path, help='alternate runtime sources for before/after controls')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, args.src_root or here.parent, opt, Path(root),
                          ['interrupts.c', 'traps.c', 'psx_bios_backend.c'],
                          'test_rfe_irq_context.c')
    print('PASS: exception return resumes the TCB context (O0/O2)')
