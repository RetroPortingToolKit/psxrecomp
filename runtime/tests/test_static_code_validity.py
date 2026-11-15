"""Compare the production validity gate with its portable uncached control."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from test_memory_debug_trace_guard import HARNESS_COMMON_H, STUBS_C, TRACE_EMPTY_C
from fixture_link import build_and_run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    runtime = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='psx-static-validity-') as tmp:
        work = Path(tmp)
        for name, content in [('harness_common.h', HARNESS_COMMON_H),
                              ('stubs.c', STUBS_C), ('trace.c', TRACE_EMPTY_C)]:
            (work / name).write_text(content, encoding='utf-8')
        for native in (0, 1):
            for opt in ('-O0', '-O2'):
                exe = work / ('validity.exe' if os.name == 'nt' else 'validity')
                subprocess.run([args.cc, '-std=c11', opt, '-w',
                                '-ffunction-sections', '-fdata-sections',
                                '-DPSX_NO_DEBUG_TOOLS', f'-DPSX_NATIVE_STATIC_CALLS={native}',
                                '-I', str(work), '-I', str(runtime / 'include'),
                                '-I', str(runtime / 'src'),
                                str(runtime / 'tests/test_static_code_validity.c'),
                                str(runtime / 'src/static_code_validity.c'),
                                str(runtime / 'src/psx_ram_geometry.c'),
                                str(runtime / 'src/kernel_patch_ranges.c'),
                                str(work / 'stubs.c'), str(work / 'trace.c'),
                                '-Wl,--gc-sections', '-o', str(exe)], check=True)
                env = {k: v for k, v in os.environ.items() if not k.upper().startswith('PSX_')}
                subprocess.run([str(exe)], env=env, check=True, timeout=30)
                env['PSX_FORCE_INTERP'] = '1'
                subprocess.run([str(exe), 'forced'], env=env, check=True, timeout=30)
        for opt in ('-O0', '-O2'):
            build_and_run(args.cc, runtime / 'tests', runtime, opt, work,
                          ['dirty_ram_interp.c', 'static_code_validity.c'],
                          'test_static_native_entry.c',
                          defines=('PSX_HAS_GAME_DISPATCH', 'PSX_NO_DEBUG_TOOLS',
                                   'PSX_NATIVE_STATIC_CALLS=1'))
    print('PASS: static validity production reference/native O0/O2')


if __name__ == '__main__':
    main()
