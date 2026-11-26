"""Word and halfword SPU CPU accesses through production memory.c (O0/O2)."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

import test_memory_debug_trace_guard as fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=fixture.default_cc())
    args = parser.parse_args()
    cc = fixture.normalize_cc(args.cc)
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='psx-spu-word-') as td:
        work = Path(td)
        fixture.write_fixture(work, fixture.MEMORY_C)
        path = work / 'stubs.c'
        source = path.read_text()
        for stub in ('uint32_t spu_read(uint32_t addr) { (void)addr; return 0; }',
                     'void spu_write(uint32_t addr, uint32_t val) { (void)addr; (void)val; }'):
            assert source.count(stub) == 1
            source = source.replace(stub, '')
        path.write_text(source)
        for opt in ('-O0', '-O2'):
            exe = work / ('spu-word' + opt + ('.exe' if os.name == 'nt' else ''))
            argv = [cc, '-std=c11', opt, '-fno-lto', '-ffunction-sections',
                    '-fdata-sections', '-I', str(work), '-I', str(root / 'runtime/include'),
                    str(root / 'runtime/tests/test_spu_word_mmio.c')]
            argv += [str(root / 'runtime/src' / name) for name in
                     ('memory.c', 'spu.c', 'psx_ram_geometry.c', 'kernel_patch_ranges.c',
                      'text_source_guard.c', 'crc32.c')]
            argv += [str(path), str(work / 'trace_empty.c'),
                     '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections',
                     '-lm', '-o', str(exe)]
            subprocess.run(argv, check=True)
            subprocess.run([str(exe)], check=True)
            print('PASS: SPU word/halfword MMIO', opt, flush=True)


if __name__ == '__main__':
    main()
