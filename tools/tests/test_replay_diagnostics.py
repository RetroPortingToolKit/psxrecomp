"""Diagnostic replay preparation stays isolated and rejects incomplete bundles."""
import argparse, importlib.util, json, os, tempfile, unittest
from pathlib import Path

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('replay', root / 'tools/replay_diagnostics.py')
replay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(replay)

HEADER = '# psxrecomp-input-v1\n# execution e\n# plan p\n# build b\n'


class Preparation(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.session = self.root / 'session'
        self.session.mkdir()
        (self.session / 'inputs.csv').write_text(HEADER + 'N 0\nN 1\nN 2\n', newline='')
        (self.session / 'card1.mcd').write_bytes(bytes(131072))
        (self.session / 'card2.absent').touch()
        (self.session / 'mods-state.toml').write_text('format_version = 2\n')
        self.exe = self.root / 'game.exe'
        self.exe.touch()
        self.disc = self.root / 'disc.cue'
        self.disc.touch()
        self.game = self.root / 'game.toml'
        self.game.write_text('[game]\nexe = "SCUS_945.64"\ndisc = "disc.cue"\nentry_pc = "0x801B5220"\n')
        self.args = argparse.Namespace(recording=self.session, exe=self.exe, disc=self.disc,
                                       game=self.game, settings=None, output=self.root / 'replay')

    def metadata(self, value):
        (self.session / 'session.json').write_text(json.dumps(value))

    def test_restore_segment_has_private_baseline_and_original_bundle_is_untouched(self):
        self.metadata({'netplay': False, 'baseline': 'baseline.pst'})
        original = b'validated-runtime-state'
        (self.session / 'baseline.pst').write_bytes(original)
        output, _, trace = replay.prepare(self.args)
        for token in ('openbios', 'scph1001'):
            self.assertEqual((output / 'saves' / token / 'state_801B5220_slot00.pst').read_bytes(), original)
        self.assertEqual((self.session / 'baseline.pst').read_bytes(), original)
        self.assertEqual((output / 'diagnostic-baseline.pst').read_bytes(), original)
        self.assertEqual(trace.read_text().count('N '), 3)

    def test_torn_final_row_is_dropped_from_the_trace(self):
        self.metadata({'netplay': False})
        (self.session / 'inputs.csv').write_text(HEADER + 'N 0\nN 1\nN 2,12', newline='')
        _, _, trace = replay.prepare(self.args)
        self.assertEqual(trace.read_text(), HEADER + 'N 0\nN 1\n')

    def test_missing_baseline_is_rejected_before_creating_output(self):
        self.metadata({'baseline': 'baseline.pst'})
        with self.assertRaisesRegex(ValueError, 'baseline'):
            replay.prepare(self.args)
        self.assertFalse(self.args.output.exists())

    def test_baseline_path_cannot_escape_bundle(self):
        self.metadata({'baseline': '../outside.pst'})
        with self.assertRaisesRegex(ValueError, 'filename'):
            replay.prepare(self.args)
        self.assertFalse(self.args.output.exists())

    def test_rewind_segment_without_image_is_rejected(self):
        self.metadata({'baseline_unavailable': True})
        with self.assertRaisesRegex(ValueError, 'rewind'):
            replay.prepare(self.args)
        self.assertFalse(self.args.output.exists())

    def test_wrong_format_and_empty_recordings_are_rejected(self):
        self.metadata({'netplay': False})
        (self.session / 'inputs.csv').write_text('# mmx4-input-v2\n# execution e\n# plan p\n# build b\nN 0\nN 1\n')
        with self.assertRaisesRegex(ValueError, 'psxrecomp-input-v1'):
            replay.prepare(self.args)
        (self.session / 'inputs.csv').write_text(HEADER + 'N 0\n')
        with self.assertRaisesRegex(ValueError, 'interval'):
            replay.prepare(self.args)
        self.assertFalse(self.args.output.exists())

    def test_original_input_only_sessions_need_no_slot_restore(self):
        self.metadata({'netplay': False})
        output, _, _ = replay.prepare(self.args)
        self.assertFalse((output / 'diagnostic-baseline.pst').exists())
        self.assertIn('enable1 = true', (output / 'settings.toml').read_text())
        self.assertIn('enable2 = false', (output / 'settings.toml').read_text())

    def test_v2_presence_recordings_keep_their_format(self):
        text = HEADER.replace('input-v1', 'input-v2') + 'N 0\nN 1\n'
        (self.session / 'inputs.csv').write_text(text)
        _, _, trace = replay.prepare(self.args)
        self.assertEqual(trace.read_text(), text)

    def test_network_bundle_does_not_launch_as_offline_replay(self):
        self.metadata({'netplay': True, 'baseline': 'baseline.pst'})
        with self.assertRaisesRegex(ValueError, 'Netplay'):
            replay.prepare(self.args)
        self.assertFalse(self.args.output.exists())

    def test_appimage_container_is_rejected_before_any_output_is_created(self):
        self.metadata({'netplay': False})
        # Detect a renamed type-2 AppImage as well as the conventional suffix.
        self.exe.write_bytes(b'\x7fELF\x02\x01\x01\x00AI\x02')
        with self.assertRaisesRegex(ValueError, '--appimage-extract.*--exe.*--game'):
            replay.prepare(self.args)
        self.assertFalse(self.args.output.exists())
        self.exe.write_bytes(b'')
        self.args.exe = self.exe.with_suffix('.AppImage')
        self.args.exe.touch()
        with self.assertRaisesRegex(ValueError, 'AppRun'):
            replay.prepare(self.args)
        self.assertFalse(self.args.output.exists())

    def test_adjacent_overlay_cache_and_toolchain_are_independent_private_copies(self):
        cache = self.root / 'cache/game/native/module.dll'
        compiler = self.root / 'overlay_toolchain/bin/compiler'
        for path, content in ((cache, b'qualified native overlay'), (compiler, b'packaged compiler')):
            path.parent.mkdir(parents=True)
            path.write_bytes(content)
        output, _, _ = replay.prepare(self.args)
        self.assertEqual((output / 'cache/game/native/module.dll').read_bytes(), b'qualified native overlay')
        self.assertEqual((output / 'overlay_toolchain/bin/compiler').read_bytes(), b'packaged compiler')
        # Neither direction may retain a reference to the original payload.
        (output / 'cache/game/native/module.dll').write_bytes(b'private cache edit')
        self.assertEqual(cache.read_bytes(), b'qualified native overlay')
        cache.unlink()
        compiler.unlink()
        self.assertEqual((output / 'cache/game/native/module.dll').read_bytes(), b'private cache edit')
        self.assertEqual((output / 'overlay_toolchain/bin/compiler').read_bytes(), b'packaged compiler')

    def test_overlay_payload_links_are_dereferenced_into_private_files(self):
        original = self.root / 'original-payload'
        (original / 'cache').mkdir(parents=True)
        (original / 'toolchain/bin').mkdir(parents=True)
        module = original / 'cache/module.so'
        module.write_bytes(b'qualified module')
        compiler = original / 'compiler'
        compiler.write_bytes(b'packaged compiler')
        try:
            (self.root / 'cache').symlink_to(original / 'cache', target_is_directory=True)
            (self.root / 'overlay_toolchain').symlink_to(original / 'toolchain', target_is_directory=True)
            (original / 'toolchain/bin/compiler').symlink_to(compiler)
        except (OSError, NotImplementedError) as error:
            self.skipTest('symlink creation unavailable: ' + str(error))
        output, _, _ = replay.prepare(self.args)
        for name in ('cache', 'overlay_toolchain', 'overlay_toolchain/bin/compiler'):
            self.assertFalse((output / name).is_symlink())
        module.unlink()
        compiler.unlink()
        self.assertEqual((output / 'cache/module.so').read_bytes(), b'qualified module')
        self.assertEqual((output / 'overlay_toolchain/bin/compiler').read_bytes(), b'packaged compiler')

    def test_extracted_linux_runtime_uses_copied_payload_and_recorded_choices(self):
        self.metadata({'netplay': False})
        image = self.root / 'squashfs-root/usr'
        binary = image / 'bin/MediEvilIIRecomp'
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b'\x7fELF\x02\x01\x01\x00\x00\x00\x00')
        payload = image / 'share/medievil2recomp'
        payload.mkdir(parents=True)
        game = payload / 'game.toml'
        game.write_text(self.game.read_text())
        (payload / 'mods').mkdir()
        (payload / 'mods/state.toml').write_text('installation choices')
        (payload / 'mods/bundled').mkdir()
        (payload / 'mods/bundled/package').write_text('bundled mod')
        (payload / 'assets').mkdir()
        (payload / 'assets/icon.png').write_bytes(b'icon')
        (payload / 'bios').mkdir()
        (payload / 'bios/openbios.bin').write_bytes(b'openbios')
        (payload / 'cache/game/native').mkdir(parents=True)
        (payload / 'cache/game/native/module.so').write_bytes(b'qualified overlay')
        (payload / 'overlay_toolchain/bin').mkdir(parents=True)
        (payload / 'overlay_toolchain/bin/compiler').write_bytes(b'packaged compiler')
        (self.session / 'settings.toml').write_text('[video]\nvsync = false\n[memcard]\ndir = "personal"\n')
        (image / 'lib').mkdir()
        (image / 'lib/libSDL3.so.0').write_bytes(b'packaged library')
        (binary.parent / 'libadjacent.so.1').write_bytes(b'adjacent library')
        self.args.exe = binary
        self.args.game = game
        output, target, _ = replay.prepare(self.args)
        self.assertEqual(target.parent, output)
        self.assertEqual((output / 'mods/state.toml').read_bytes(), (self.session / 'mods-state.toml').read_bytes())
        self.assertEqual((payload / 'mods/state.toml').read_text(), 'installation choices')
        self.assertEqual((output / 'mods/bundled/package').read_text(), 'bundled mod')
        self.assertEqual((output / 'assets/icon.png').read_bytes(), b'icon')
        self.assertEqual((output / 'bios/openbios.bin').read_bytes(), b'openbios')
        self.assertEqual((output / 'cache/game/native/module.so').read_bytes(), b'qualified overlay')
        self.assertEqual((output / 'overlay_toolchain/bin/compiler').read_bytes(), b'packaged compiler')
        self.assertEqual((output / 'lib/libSDL3.so.0').read_bytes(), b'packaged library')
        (payload / 'cache/game/native/module.so').unlink()
        (payload / 'overlay_toolchain/bin/compiler').unlink()
        self.assertEqual((output / 'cache/game/native/module.so').read_bytes(), b'qualified overlay')
        self.assertEqual((output / 'overlay_toolchain/bin/compiler').read_bytes(), b'packaged compiler')
        self.assertEqual((output / 'lib/libadjacent.so.1').read_bytes(), b'adjacent library')
        settings = (output / 'settings.toml').read_text()
        self.assertIn('vsync = false', settings)
        self.assertNotIn('personal', settings)
        (image / 'lib/libSDL3.so.0').write_bytes(b'changed original')
        self.assertEqual((output / 'lib/libSDL3.so.0').read_bytes(), b'packaged library')

    def test_replay_environment_uses_private_roots_and_no_inherited_mount(self):
        output = self.root / 'private'
        output.mkdir()
        (output / 'diagnostic-baseline.pst').write_bytes(b'baseline')
        original = {'APPIMAGE': '/personal/game.AppImage', 'APPDIR': '/tmp/.mount-personal',
                    'RECOMP_APPIMAGE_PATH': '/personal/game.AppImage',
                    'RECOMP_APPIMAGE_PATH': '/personal/game.AppImage',
                    'RECOMP_DATA_DIR': '/personal/data', 'RECOMP_SEED_ONLY': '1',
                    'LD_LIBRARY_PATH': '/personal/lib', 'PSX_LOAD_SLOT': '7', 'PSX_SPEED': '2',
                    'PSX_DIAGNOSTIC_REPLAY': '/personal/inputs.csv', 'PATH': 'system path'}
        original['PSX_OVERLAY_CAPTURES'] = '/personal/overlay_captures.json'
        trace = output / 'replay-inputs.csv'
        env = replay.replay_environment(output, trace, original)
        self.assertEqual(env['SDL_AUDIODRIVER'], 'dummy')
        self.assertEqual(env['SDL_AUDIO_DRIVER'], 'dummy')
        self.assertEqual(env['PSX_DIAGNOSTIC_REPLAY'], str(trace))
        self.assertEqual(env['PSX_OVERLAY_CAPTURES'], str(output / 'overlay_captures.json'))
        clean = replay.replay_environment(output, trace, {'PATH': 'system path'})
        self.assertEqual(clean['PSX_OVERLAY_CAPTURES'], str(output / 'overlay_captures.json'))
        self.assertEqual(env['LD_LIBRARY_PATH'], os.pathsep.join((str(output / 'lib'), str(output))))
        self.assertEqual(env['PSX_LOAD_SLOT'], '0')
        self.assertEqual(env['PATH'], 'system path')
        for key in ('APPIMAGE', 'APPDIR', 'RECOMP_APPIMAGE_PATH', 'RECOMP_APPIMAGE_PATH',
                    'RECOMP_SEED_ONLY', 'PSX_SPEED'):
            self.assertNotIn(key, env)
        self.assertEqual(original['RECOMP_DATA_DIR'], '/personal/data')


if __name__ == '__main__':
    unittest.main()
