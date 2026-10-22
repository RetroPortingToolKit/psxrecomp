"""Production callback tables retain the executing thread TLS lifetime."""
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
                          ['overlay_loader.c'], 'test_overlay_callback_context.c',
                          defines=('PSX_NO_DEBUG_TOOLS',))
    print('PASS: thread-owned overlay callback context (O0/O2)')
