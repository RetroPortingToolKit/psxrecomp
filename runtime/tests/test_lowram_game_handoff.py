"""Reuse the real memory fixture and link the production game-entry owner."""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile
import test_memory_debug_trace_guard as fixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cc', required=True)
    parser.add_argument('--memory-source', type=Path, default=fixture.MEMORY_C)
    parser.add_argument('--fntrace-source', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    fixture.DRIVER_C = Path(__file__).with_suffix('.c').read_text(encoding='utf-8')
    original = 'int fntrace_is_game_started(void) { return 0; }'
    assert original in fixture.STUBS_C
    fixture.STUBS_C = fixture.STUBS_C.replace(original, '')
    # LTO also checks these inherited, unrelated device-double signatures.
    for old, new in (
        ('ls_read_hook(uint32_t addr, uint8_t width', 'ls_read_hook(uint32_t addr, int width'),
        ('ls_write_hook(uint32_t addr, uint8_t width', 'ls_write_hook(uint32_t addr, int width'),
        ('ds_note_read(uint32_t addr, uint8_t width', 'ds_note_read(uint32_t addr, uint32_t width'),
        ('ds_note_write(uint32_t addr, uint8_t width', 'ds_note_write(uint32_t addr, uint32_t width'),
        ('uint16_t spu_read(', 'uint32_t spu_read('),
        ('spu_write(uint32_t addr, uint16_t val', 'spu_write(uint32_t addr, uint32_t val'),
    ):
        fixture.STUBS_C = fixture.STUBS_C.replace(old, new)
        fixture.HARNESS_COMMON_H = fixture.HARNESS_COMMON_H.replace(old, new)
    fixture.STUBS_C += '''
void cdrom_notify_game_started(void) {}
void boot_state_trigger_capture(const CPUState* cpu) { (void)cpu; }
'''
    with tempfile.TemporaryDirectory(prefix='psx-lowram-') as temporary:
        work = Path(temporary)
        memory = fixture.write_fixture(work, args.memory_source)
        rom = bytearray(512*1024)
        struct.pack_into('<I', rom, 0x10a00, 0xaf5a0000)
        (work/'authored-bios.bin').write_bytes(rom)
        exe = work/'lowram.exe'
        sources = [work/'driver.c', memory,
                   args.fntrace_source or root/'runtime/src/fntrace.c',
                   root/'runtime/src/kernel_patch_ranges.c',
                   root/'runtime/src/text_source_guard.c',
                   root/'runtime/src/crc32.c', root/'runtime/src/psx_ram_geometry.c',
                   work/'stubs.c', work/'trace_empty.c']
        subprocess.run([args.cc, '-std=c11', '-O2', '-flto', '-fwhole-program',
                        '-Werror=lto-type-mismatch',
                        '-ffunction-sections', '-fdata-sections', '-DPSX_NO_DEBUG_TOOLS=1',
                        '-I', str(work), '-I', str(root/'runtime/include'),
                        *map(str, sources), '-Wl,--gc-sections', '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(work/'authored-bios.bin')], check=True)


if __name__ == '__main__':
    main()
