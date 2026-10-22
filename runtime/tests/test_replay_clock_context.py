"""A frozen replay clock never changes the concurrent live clock/gates."""
import argparse
import tempfile
from pathlib import Path
from fixture_link import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as temp:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(temp),
                          ['psx_cycles.c'], 'test_replay_clock_context.c',
                          defines=('PSX_NO_DEBUG_TOOLS',))
    print('PASS: independent frozen replay clock (O0/O2)')
