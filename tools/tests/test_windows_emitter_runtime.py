"""Windows emitter dependency closure can be tested on any packaging host."""

from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import release_stage


def write_pe(path, imports=(), delayed=()):
    """Write the PE headers and import data needed by the staging reader."""
    data = bytearray(0x400)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    struct.pack_into('<4sHHIIIHH', data, 0x80, b'PE\0\0', 0x8664, 1, 0, 0, 0, 240, 0)
    optional = 0x98
    struct.pack_into('<H', data, optional, 0x20b)
    struct.pack_into('<I', data, optional + 108, 16)
    directories = optional + 112
    struct.pack_into('<III', data, optional + 240 + 12, 0x1000, 0x200, 0x200)
    name_offset = 0x300
    for names, directory, start, descriptor in (
        (imports, 1, 0x200, (0, 0, 0, None, 0)),
        (delayed, 13, 0x280, (1, None, 0, 0, 0, 0, 0, 0)),
    ):
        if not names:
            continue
        stride = len(descriptor) * 4
        struct.pack_into('<II', data, directories + directory * 8,
                         start + 0xe00, stride * (len(names) + 1))
        for index, name in enumerate(names):
            fields = [name_offset + 0xe00 if value is None else value for value in descriptor]
            struct.pack_into('<%dI' % len(fields), data, start + stride * index, *fields)
            encoded = name.encode('ascii') + b'\0'
            data[name_offset:name_offset + len(encoded)] = encoded
            name_offset += len(encoded)
    path.write_bytes(data)


class WindowsEmitterRuntimeTest(unittest.TestCase):
    def test_stages_both_emitters_and_transitive_imports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sdk = root / 'sdk' / 'bin'
            sdk.mkdir(parents=True)
            (sdk.parent / 'LICENSE.TXT').write_text('LLVM notice', encoding='utf-8')
            dest = root / 'stage'
            dest.mkdir()
            game, bios = root / 'game.exe', root / 'bios.exe'
            write_pe(game, imports=('KERNEL32.dll', 'LiBc++.DLL'))
            write_pe(bios, delayed=('bios_extra.dll',))
            write_pe(sdk / 'LIBC++.dll', imports=('libunwind.dll', 'api-ms-win-crt-heap-l1-1-0.dll'))
            write_pe(sdk / 'libunwind.dll')
            write_pe(root / 'bios_extra.dll', imports=('libunwind.dll',))
            write_pe(sdk / 'unused.dll')

            release_stage._stage_windows_emitter_runtime(dest, [game, bios], sdk, lambda _: None)

            self.assertEqual({path.name for path in dest.iterdir()},
                             {'libc++.dll', 'libunwind.dll', 'bios_extra.dll', 'LLVM-LICENSE.txt'})
            self.assertEqual((dest / 'LLVM-LICENSE.txt').read_text(encoding='utf-8'), 'LLVM notice')

    def test_reports_the_importing_file_for_a_missing_dll(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / 'game.exe'
            write_pe(game, imports=('KERNEL32.dll', 'libmissing.dll'))
            with self.assertRaisesRegex(release_stage.StageError, 'libmissing.dll .*game.exe'):
                release_stage._stage_windows_emitter_runtime(root, [game], None, lambda _: None)


if __name__ == '__main__':
    unittest.main()
