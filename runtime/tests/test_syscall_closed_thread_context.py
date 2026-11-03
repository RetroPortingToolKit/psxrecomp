"""CloseTh(current); ChangeTh(other) must still save the outgoing registers.

Links the production traps.c into test_syscall_closed_thread_context.c at -O0
and -O2; every other runtime seam is an abort() stub (fixture_link.py).
"""
import argparse
import tempfile
from pathlib import Path

from fixture_link import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          ['traps.c'], 'test_syscall_closed_thread_context.c')
    print('PASS: closed outgoing thread keeps its context without reopening (O0/O2)')
