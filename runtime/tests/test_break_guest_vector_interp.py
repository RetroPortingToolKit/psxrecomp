"""An interpreted BREAK reaches the guest's own exception vector; a
BREAK in statically compiled code keeps the fatal exit. The run report counts
the vector entries."""
import argparse
from pathlib import Path
import re
import tempfile
from source_fixture_link import build_and_run


def static_side(root):
    """psx_break is the exit and nothing else; only the interpreter asks the vector."""
    traps = (root / 'runtime/src/traps.c').read_text(encoding='utf-8')
    body = re.search(r'^void psx_break\(CPUState\* cpu, uint32_t code, uint32_t pc\) \{\n(.*?)^\}\n',
                     traps, re.S | re.M)
    assert body, 'psx_break not found in traps.c'
    text = body.group(1)
    assert 'trap_crash(buf);' in text and 'exit(1);' in text, 'psx_break no longer ends the process'
    assert 'vector' not in text and 'return' not in text, 'psx_break can now return to its caller'
    callers = [p.relative_to(root).as_posix()
               for d in ('runtime/src', 'runtime/include', 'recompiler/src', 'recompiler/include')
               for p in sorted((root / d).rglob('*'))
               if p.is_file() and p.suffix in ('.c', '.cpp', '.h', '.inc')
               and 'psx_break_enter_guest_vector' in p.read_text(encoding='utf-8', errors='replace')]
    assert callers == ['runtime/src/dirty_ram_interp.c', 'runtime/src/traps.c'], callers
    translator = root / 'recompiler/src/strict_translator.cpp'
    assert '"psx_break(cpu, 0x{:05X}u, 0x{:08X}u); return;"' in translator.read_text(encoding='utf-8'), \
        'the static translator no longer emits the plain psx_break call'


def report_side(root):
    """The run report writes the counters that traps.c keeps (the report writer
    links into the game runtime only, so its wiring is checked in the source)."""
    report = (root / 'runtime/src/crash_trace.c').read_text(encoding='utf-8')
    call = report.index('psx_break_guest_vector_stats(bg);')
    fmt = re.search(
        r'"  \\"break_guest_vector\\": \{\\n"\s*'
        r'"    \\"count\\": %u,\\n"\s*'
        r'"    \\"first_pc\\": \\"0x%08X\\",\\n"\s*'
        r'"    \\"first_code\\": \\"0x%05X\\"\\n"\s*'
        r'"  \},\\n",\s*bg\[0\], bg\[1\], bg\[2\]\);',
        report[call:])
    assert fmt, 'the report must write "break_guest_vector": {"count", "first_pc", "first_code"} from the counters'


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    static_side(here.parent.parent)
    report_side(here.parent.parent)
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          ['dirty_ram_interp.c'], 'test_break_guest_vector_interp.c',
                          defines=('PSX_HAS_GAME_DISPATCH', 'PSX_NO_DEBUG_TOOLS'))
            build_and_run(args.cc, here, here.parent, opt, Path(root),
                          ['traps.c'], 'test_break_guest_vector_report.c')
    print('PASS: an interpreted BREAK passes through the guest vector and is counted; '
          'static code keeps the exit (O0/O2)')
