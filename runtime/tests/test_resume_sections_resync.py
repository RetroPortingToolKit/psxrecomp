"""Load resyncs keep checkpoint-restored state once, and only then.

Links the production interrupts.c, dirty_ram_interp.c and psx_cycles.c into
test_resume_sections_resync.c at -O0 and -O2; every other runtime seam is an
abort() stub (fixture_link.py).
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
                          ['interrupts.c', 'dirty_ram_interp.c', 'psx_cycles.c'],
                          'test_resume_sections_resync.c',
                          defines=('PSX_NO_DEBUG_TOOLS',))
    print('PASS: load resyncs keep checkpoint state once (O0/O2)')
