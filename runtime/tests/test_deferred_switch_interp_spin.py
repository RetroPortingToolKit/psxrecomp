"""A deferred thread switch is taken while an interpreted thread spins.

Links the production dirty_ram_interp.c (release build, game dispatch on) into
test_deferred_switch_interp_spin.c at -O0 and -O2; every other runtime seam is
a stub that fails the run (fixture_link.py).

--src-root builds the fixture against the runtime sources of another tree, for
a control run on a tree without the change, where it must fail.
"""
import argparse
import tempfile
from pathlib import Path

from fixture_link import build_and_run

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--src-root', default=None,
                        help='the runtime directory whose src/ and include/ are built')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    src_root = Path(args.src_root).resolve() if args.src_root else here.parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            work = Path(root) / opt.strip('-')
            work.mkdir()
            build_and_run(args.cc, here, src_root, opt, work,
                          ['dirty_ram_interp.c'], 'test_deferred_switch_interp_spin.c',
                          defines=('PSX_HAS_GAME_DISPATCH', 'PSX_NO_DEBUG_TOOLS'))
    print('PASS: a deferred thread switch is taken while an interpreted thread spins (O0/O2)')
