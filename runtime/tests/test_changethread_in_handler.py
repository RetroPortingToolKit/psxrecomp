"""A ChangeThread called inside an interrupt handler switches at the call.

The guest file is built three times: as it is, with CHANGETHREAD_NO_RESTORE and
with CHANGETHREAD_NOT_RUNNABLE (see its header). The helper passes no define
to the guest file, so each variant is a copy that starts with its define.

--src-root and --control exist for the control run: they build the first
variant against the runtime sources of another tree (one from before the fix),
where it must fail on what the guest sees.
"""
import argparse
from pathlib import Path
import re
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))
from fixture_link import build_and_run

GUEST = 'test_changethread_in_handler.c'
MODULES = ['interrupts.c', 'traps.c', 'psx_bios_backend.c']


def report_side(root):
    """The run report writes the counters that traps.c keeps (the report writer
    links into the game runtime only, so its wiring is checked in the source)."""
    report = (root / 'runtime/src/crash_trace.c').read_text(encoding='utf-8')
    call = report.index('psx_changethread_in_handler_stats(ct);')
    fmt = re.search(
        r'"  \\"changethread_in_handler\\": \{\\n"\s*'
        r'"    \\"switches\\": %u,\\n"\s*'
        r'"    \\"same_thread\\": %u,\\n"\s*'
        r'"    \\"not_taken\\": %u,\\n"\s*'
        r'"    \\"first_epc\\": \\"0x%08X\\",\\n"\s*'
        r'"    \\"first_frame\\": %u,\\n"\s*'
        r'"    \\"first_target\\": \\"0x%08X\\"\\n"\s*'
        r'"  \},\\n",\s*ct\[0\], ct\[1\], ct\[2\], ct\[3\], ct\[5\], ct\[4\]\);',
        report[call:])
    assert fmt, ('the report must write "changethread_in_handler": {"switches", "same_thread", '
                 '"not_taken", "first_epc", "first_frame", "first_target"} from the counters')


def variant(here, work, name, define):
    """A copy of the guest that starts with `define` (None: the file as it is)."""
    if define is None:
        return here
    guest = work / name
    guest.mkdir()
    (guest / 'rfe_guest_fixture.h').write_bytes((here / 'rfe_guest_fixture.h').read_bytes())
    (guest / GUEST).write_bytes(b'#define ' + define.encode() + b' 1\n' + (here / GUEST).read_bytes())
    return guest


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--src-root', default=None,
                        help='the runtime directory whose src/ and include/ are built')
    parser.add_argument('--control', action='store_true',
                        help='build without the counters (sources from before the fix)')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    src_root = Path(args.src_root).resolve() if args.src_root else here.parent
    if args.control:
        variants = [('control', 'CHANGETHREAD_CONTROL')]
    else:
        report_side(src_root.parent)
        variants = [('plain', None), ('no_restore', 'CHANGETHREAD_NO_RESTORE'),
                    ('not_runnable', 'CHANGETHREAD_NOT_RUNNABLE')]
    with tempfile.TemporaryDirectory() as root:
        for name, define in variants:
            guest = variant(here, Path(root), name, define)
            for opt in ('-O0', '-O2'):
                work = Path(root) / (name + opt)
                work.mkdir()
                output = build_and_run(args.cc, guest, src_root, opt, work, MODULES, GUEST)
                print(name, opt, output.rstrip(), flush=True)
    print('PASS: a ChangeThread called inside a handler switches at the call '
          '(three guests, O0/O2)')
