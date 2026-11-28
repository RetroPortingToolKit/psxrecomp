#!/usr/bin/env python3
"""Unit tests for tools/package_title_release.py (no guest build, no disc, no game).

Pins the release contract every title now shares: --channel is required with
no default, owned CMake flags cannot be overridden by a title, artifact names
follow the channel, forbidden content fails the payload gates, diagnostics
recording/display defaults remain independent, and a finished ZIP is re-verified by extraction.
"""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import package_title_release as ptr  # noqa: E402

DIAG = '''format_version = 5
id = "fixture.diagnostics"
version = "1.0.0"
name = "Playtest Diagnostics"
channel = "stable"

[[target]]
game_id = "SLUS-00000"

[[feature]]
id = "recording"
default_enabled = {recording}

[[feature]]
id = "osd"
default_enabled = {osd}

[[plugin]]
feature = "recording"
id = "fixture.diagnostics"

[[plugin]]
feature = "osd"
id = "fixture.diagnostics.osd"
'''

CONFIG = '''schema = 1

[product]
name = "FixtureRecomp"
display_name = "Fixture Recompiled"
exe_name = "Fixture"
version_file = "VERSION"

[build]
cmake_args = ["-DPSX_PGXP_VARIANT=OFF"]

[diagnostics]
game_id = "SLUS-00000"
package_id = "fixture.diagnostics"

[payload]
start_here = "START_HERE.txt"
files = ["README.md", {{ from = "notes/input.ini", to = "input.ini" }}]
{extra}
'''


class Fixture:
    def __init__(self, extra=''):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'VERSION').write_text('v1.2.3\n')
        (self.root / 'START_HERE.txt').write_text('Fixture @VERSION@\n')
        (self.root / 'README.md').write_text('readme\n')
        (self.root / 'game.toml').write_text('[game]\nid = "SLUS-00000"\n')
        (self.root / 'notes').mkdir()
        (self.root / 'notes/input.ini').write_text('[controller]\n')
        (self.root / 'release.toml').write_text(CONFIG.format(extra=extra))

    def config(self):
        return ptr.Config(self.root / 'release.toml', self.root)

    def close(self):
        self.tmp.cleanup()


def write_stage(stage, channel, version='v1.2.3', exe='Fixture.exe',
                package_id='fixture.diagnostics'):
    stage.mkdir(parents=True)
    (stage / exe).write_bytes(b'MZ fixture')
    (stage / 'psx_game_version.txt').write_text(version + '\n')
    (stage / 'START_HERE.txt').write_text('Fixture %s\n' % version)
    (stage / 'game.toml').write_text('[game]\nid = "SLUS-00000"\n')
    (stage / 'replay-diagnostics.py').write_text('# helper\n')
    (stage / 'bios').mkdir()
    (stage / 'bios/openbios.bin').write_bytes(b'\0' * ptr.OPENBIOS_SIZE)
    package = stage / 'mods/bundled' / package_id / '1.0.0'
    package.mkdir(parents=True)
    (package / 'manifest.toml').write_text(
        DIAG.format(recording='true', osd='false').replace('fixture.diagnostics', package_id))
    files = {p.relative_to(stage).as_posix(): ptr.sha256(p)
             for p in sorted(stage.rglob('*')) if p.is_file()}
    manifest = dict(channel=channel, version=version, product='FixtureRecomp', executable=exe,
                    mod_packages=[package_id],
                    playtest_diagnostics_default_enabled=True, files=files)
    (stage / 'RELEASE_MANIFEST.json').write_text(json.dumps(manifest))


def zip_stage(stage, artifact):
    with zipfile.ZipFile(artifact, 'w') as archive:
        for path in sorted(stage.rglob('*')):
            if path.is_file():
                archive.write(path, path.relative_to(stage).as_posix())
    Path(str(artifact) + '.sha256').write_text(
        '%s  %s\n' % (hashlib.sha256(artifact.read_bytes()).hexdigest(), artifact.name))


class BaselineProjectHook(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cmake') and shutil.which('ninja'),
                         'native CMake and Ninja are required')
    def test_baseline_removes_a_cached_project_hook(self):
        fixture = Fixture()
        self.addCleanup(fixture.close)
        root = fixture.root
        build = root / 'runtime'
        build.mkdir()
        poison = root / 'old-pgo-hook.cmake'
        poison.write_text('message(FATAL_ERROR "stale PGO hook executed")\n')
        (build / 'CMakeCache.txt').write_text(
            'CMAKE_PROJECT_INCLUDE:FILEPATH=%s\n' % poison.as_posix())
        # project() itself rejects an empty include path on CMake 3.28.
        # Keep the assertion too: newer CMake must not retain a defined hook.
        (root / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.20)\n'
            'project(PackageBaseline NONE)\n'
            'if(DEFINED CMAKE_PROJECT_INCLUDE)\n'
            '  message(FATAL_ERROR "baseline project hook is still defined")\n'
            'endif()\n'
            'set(PSX_PUBLIC_RELEASE ON CACHE BOOL "fixture public channel")\n'
            'add_custom_target(psx-runtime)\n')
        tc = SimpleNamespace(cmake=shutil.which('cmake'), generator='Ninja',
                             gcc=shutil.which('gcc') or 'gcc',
                             gxx=shutil.which('g++') or 'g++',
                             make=shutil.which('ninja'), env=None)
        config = fixture.config()
        config.selective_pgo = False
        ctx = SimpleNamespace(root=root, work=root, runtime_build=build,
                              config=config, channel='public',
                              platform=ptr.host_platform(), version='v1.2.3',
                              jobs=1, prov={'source_commit': 'fixture'}, tc=tc)
        ptr.configure_and_build(ctx)
        self.assertNotIn('CMAKE_PROJECT_INCLUDE', ptr.cmake_cache(build))


class ChannelIsRequired(unittest.TestCase):
    def run_main(self, argv):
        err = io.StringIO()
        with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as raised:
            ptr.main(argv)
        return raised.exception.code, err.getvalue()

    def test_missing_channel_fails_loudly(self):
        code, err = self.run_main(['--platform', 'windows'])
        self.assertNotEqual(code, 0)
        self.assertIn('PACKAGING FAILED', err)
        self.assertIn('--channel', err)

    def test_unknown_channel_fails(self):
        code, err = self.run_main(['--channel', 'release'])
        self.assertNotEqual(code, 0)
        self.assertIn('PACKAGING FAILED', err)

    def test_verify_also_requires_channel(self):
        code, _ = self.run_main(['verify', 'x.zip'])
        self.assertNotEqual(code, 0)

    def test_default_subcommand_is_package(self):
        args = ptr.parse_args(['--channel', 'playtest'])
        self.assertEqual((args.command, args.channel), ('package', 'playtest'))


class ConfigTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)

    def test_loads(self):
        config = self.fixture.config()
        self.assertEqual(config.version(), 'v1.2.3')
        self.assertEqual([rel for _, rel in config.files], ['README.md', 'input.ini'])
        self.assertEqual(config.aot_mode, 'none')
        self.assertTrue(config.diagnostics['replay_helper'])
        self.assertTrue(config.selective_pgo)
        self.assertEqual(config.pgo_train_seconds, 10)
        self.assertEqual(config.pgo_train_frames, 600)

    def test_pgo_options_are_explicit_and_bounded(self):
        path = self.fixture.root / 'release.toml'
        path.write_text(CONFIG.format(extra='').replace(
            '[build]', '[build]\nselective_pgo = false\npgo_train_seconds = 3'))
        self.assertFalse(self.fixture.config().selective_pgo)
        self.assertEqual(self.fixture.config().pgo_train_frames, 180)
        for value in ('0', '301', 'true', '"10"'):
            path.write_text(CONFIG.format(extra='').replace(
                '[build]', '[build]\npgo_train_seconds = ' + value))
            with self.assertRaisesRegex(ptr.PackagingError, 'pgo_train_seconds'):
                self.fixture.config()

    def test_explicit_guest_frame_workload_overrides_legacy_seconds(self):
        path = self.fixture.root / 'release.toml'
        path.write_text(CONFIG.format(extra='').replace(
            '[build]', '[build]\npgo_train_frames = 240\npgo_train_seconds = 3'))
        self.assertEqual(self.fixture.config().pgo_train_frames, 240)
        for value in ('0', '18001', 'true', '"600"'):
            path.write_text(CONFIG.format(extra='').replace(
                '[build]', '[build]\npgo_train_frames = ' + value))
            with self.assertRaisesRegex(ptr.PackagingError, 'pgo_train_frames'):
                self.fixture.config()

    def test_diagnostics_hd_policy_is_explicit_and_forwarded(self):
        path = self.fixture.root / 'release.toml'
        for enabled in (False, True):
            path.write_text(CONFIG.format(extra='').replace(
                '[diagnostics]', '[diagnostics]\nallow_hd_textures = ' + str(enabled).lower()))
            config = self.fixture.config()
            self.assertIs(config.diagnostics['allow_hd_textures'], enabled)
            with mock.patch.object(ptr, 'run') as run:
                ptr.diagnostics_audit(SimpleNamespace(config=config, channel='public'), 'release.zip')
            self.assertEqual('--allow-hd-textures' in run.call_args.args[0], enabled)
        path.write_text(CONFIG.format(extra=''))
        self.assertFalse(self.fixture.config().diagnostics['allow_hd_textures'])
        path.write_text(CONFIG.format(extra='').replace(
            '[diagnostics]', '[diagnostics]\nallow_hd_textures = "true"'))
        with self.assertRaisesRegex(ptr.PackagingError, 'allow_hd_textures'):
            self.fixture.config()

    def test_owned_cmake_flags_are_refused(self):
        for flag in ('PSX_PUBLIC_RELEASE', 'PSX_RELEASE_CHANNEL', 'PSX_DEBUG_TOOLS',
                     'PSXRECOMP_BIOS_STEMS', 'CMAKE_BUILD_TYPE', 'PSX_PGO',
                     'PSX_RUNTIME_IPO', 'CMAKE_PROJECT_INCLUDE',
                     'PSX_RUNTIME_HOT_PGO_MODE', 'PSX_RUNTIME_HOT_PGO_TARGET'):
            text = CONFIG.format(extra='').replace(
                '"-DPSX_PGXP_VARIANT=OFF"', '"-D%s=ON"' % flag)
            (self.fixture.root / 'release.toml').write_text(text)
            with self.assertRaisesRegex(ptr.PackagingError, 'packager owns'):
                self.fixture.config()

    def test_unknown_key_is_an_error(self):
        (self.fixture.root / 'release.toml').write_text(
            CONFIG.format(extra='').replace('[build]', '[build]\nskip_audit = true'))
        with self.assertRaisesRegex(ptr.PackagingError, 'unknown key'):
            self.fixture.config()

    def test_missing_payload_file_is_an_error(self):
        (self.fixture.root / 'README.md').unlink()
        with self.assertRaisesRegex(ptr.PackagingError, 'not found'):
            self.fixture.config()

    def test_package_path_cannot_escape(self):
        (self.fixture.root / 'release.toml').write_text(
            CONFIG.format(extra='').replace('to = "input.ini"', 'to = "../input.ini"'))
        with self.assertRaisesRegex(ptr.PackagingError, 'relative path'):
            self.fixture.config()


class NamingTests(unittest.TestCase):
    def test_artifact_names_follow_channel(self):
        fixture = Fixture()
        self.addCleanup(fixture.close)
        config = fixture.config()
        self.assertEqual(ptr.artifact_name(config, 'v1.2.3', 'public', 'windows'),
                         'FixtureRecomp-v1.2.3-windows-x64.zip')
        self.assertEqual(ptr.artifact_name(config, '1.2.3', 'playtest', 'windows'),
                         'FixtureRecomp-v1.2.3-playtest-windows-x64.zip')
        self.assertEqual(ptr.artifact_name(config, '0.2.4-alpha', 'playtest', 'linux'),
                         'FixtureRecomp-v0.2.4-alpha-playtest-linux-x86_64.AppImage')


class GateTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.stage = Path(self.tmp.name) / 'stage'
        write_stage(self.stage, 'public')

    def test_clean_stage_passes(self):
        ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')

    def assert_refused(self, rel, data=b'x', message=None):
        path = self.stage / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        with self.assertRaises(ptr.PackagingError) as raised:
            ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')
        if message:
            self.assertIn(message, str(raised.exception))
        top = self.stage / Path(rel).parts[0]
        if top.is_dir() and top.name not in ('bios', 'mods'):
            shutil.rmtree(top)
        else:
            path.unlink()

    def test_forbidden_content(self):
        self.assert_refused('Game (USA).cue', message='disc')
        self.assert_refused('bios/SCPH1001.BIN', message='image')
        self.assert_refused('saves/card1.mcd')
        self.assert_refused('settings.toml', message='personal')
        self.assert_refused('mods/installed/x.psxmod', message='personal')
        self.assert_refused('state.pst', message='disc')
        self.assert_refused('lib/game_full_1.c', message='source')
        self.assert_refused('generated/x.h', message='build-kit')
        for name in ('memory.gcda', 'memory.gcno', 'default.profraw', 'default.profdata', 'libgcov.dll'):
            self.assert_refused(name, message='profile/training')

    def test_toolchain_sources_are_allowed(self):
        (self.stage / 'overlay_toolchain/tcc/examples').mkdir(parents=True)
        (self.stage / 'overlay_toolchain/tcc/examples/fib.c').write_text('int x;')
        ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')

    def test_hash_bound_disc_overlay_asset(self):
        package = self.stage / 'mods/bundled/fixture.patch/1.0.0'
        (package / 'assets').mkdir(parents=True)
        asset = package / 'assets/warning.bin'
        asset.write_bytes(b'authored patch fixture')
        (package / 'manifest.toml').write_text(
            '[[overlay]]\ntarget="disc_user"\noffset=16\n'
            'file="assets/warning.bin"\nsha256="%s"\nexpected_sha256="%s"\n'
            % (hashlib.sha256(asset.read_bytes()).hexdigest(), '0' * 64))
        ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')
        asset.write_bytes(b'tampered')
        with self.assertRaisesRegex(ptr.PackagingError, 'hash mismatch'):
            ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')

    def test_undeclared_mod_binary_is_refused(self):
        self.assert_refused('mods/bundled/fixture/1.0.0/assets/disc.bin', message='image')

    def test_openbios_required(self):
        (self.stage / 'bios/openbios.bin').write_bytes(b'short')
        with self.assertRaisesRegex(ptr.PackagingError, 'OpenBIOS'):
            ptr.payload_gates(self.stage, 'windows', 'Fixture.exe')

    def test_windows_binary_in_linux_package(self):
        (self.stage / 'Fixture').write_bytes(b'\x7fELF')
        (self.stage / 'extra.dll').write_bytes(b'MZ')
        with self.assertRaisesRegex(ptr.PackagingError, 'Windows binary'):
            ptr.payload_gates(self.stage, 'linux', 'Fixture')

    def test_template_tokens_must_all_be_stamped(self):
        template = Path(self.tmp.name) / 'AppRun'
        template.write_text('echo @VERSION@ @UNKNOWN_TOKEN@\n')
        with self.assertRaisesRegex(ptr.PackagingError, 'UNKNOWN_TOKEN'):
            ptr.render_template(template, {'VERSION': '1'})
        template.write_text('echo @VERSION@\n')
        self.assertEqual(ptr.render_template(template, {'VERSION': '1'}), 'echo 1\n')


class SelectiveReleasePgoTests(unittest.TestCase):
    """Mock orchestration only: no compiler, profile producer or game is run."""

    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)
        root = self.fixture.root
        self.ctx = SimpleNamespace(root=root, work=root / 'work', runtime_build=root / 'work/runtime',
                                   config=self.fixture.config(), channel='public', platform='windows',
                                   version='v1.2.3', jobs=6, prov={'source_commit': 'fixture'},
                                   tc=SimpleNamespace(cmake='cmake', exe_suffix='.exe', generator='Ninja',
                                                      gcc='gcc', gxx='g++', make='ninja',
                                                      env={'PATH': 'fixture', 'PSX_LOAD_SLOT': '9'}))
        self.ctx.runtime_build.mkdir(parents=True)

    def test_detects_the_current_compiler_file(self):
        build = self.ctx.runtime_build
        (build / 'CMakeCache.txt').write_text(
            'CMAKE_CACHE_MAJOR_VERSION:INTERNAL=3\nCMAKE_CACHE_MINOR_VERSION:INTERNAL=31\n'
            'CMAKE_CACHE_PATCH_VERSION:INTERNAL=7\n')
        for version, compiler in [('3.30.0', 'Clang'), ('3.31.7', 'GNU')]:
            path = build / 'CMakeFiles' / version / 'CMakeCCompiler.cmake'
            path.parent.mkdir(parents=True)
            path.write_text('set(CMAKE_C_COMPILER_ID "%s")\n' % compiler)
        self.assertEqual(ptr.configured_cmake_value(
            build, 'CMakeCCompiler.cmake', 'CMAKE_C_COMPILER_ID'), 'GNU')

    def configure(self, enabled=True, generator='Ninja', cross=False):
        ctx = self.ctx
        ctx.config.selective_pgo = enabled
        ctx.tc.generator = generator
        cache = {}

        def command(argv, what, **kwargs):
            if what == 'runtime configure':
                cache.update(arg[2:].split('=', 1) for arg in argv
                             if str(arg).startswith('-D'))
                cache['CMAKE_GENERATOR'] = generator
                cache['PSX_PUBLIC_RELEASE'] = 'ON' if ctx.channel == 'public' else 'OFF'

        def detection(build, filename, key):
            return ('TRUE' if cross else 'FALSE') if key == 'CMAKE_CROSSCOMPILING' else 'GNU'

        def pgo(*args):
            ctx.runtime_pgo = {'status': 'selective-use'}

        with mock.patch.object(ptr, 'run', side_effect=command) as commands, \
                mock.patch.object(ptr, 'cmake_cache', side_effect=lambda _: dict(cache)), \
                mock.patch.object(ptr, 'configured_cmake_value', side_effect=detection), \
                mock.patch.object(ptr, 'build_selective_pgo', side_effect=pgo) as selected:
            ptr.configure_and_build(ctx)
        return commands, selected

    def test_native_gnu_ninja_defaults_to_qualified_workflow(self):
        commands, selected = self.configure()
        self.assertEqual(selected.call_count, 1)
        self.assertEqual([call.args[1] for call in commands.call_args_list], ['runtime configure'])

    def test_shared_emitters_still_configure_and_build_current_sources(self):
        ctx = self.ctx
        ctx.recompiler_build = ctx.root / 'shared-emitters'
        ctx.recompiler_build.mkdir()
        for name in ('psxrecomp-game.exe', 'psxrecomp-bios.exe'):
            (ctx.recompiler_build / name).write_bytes(b'unit fixture, never run')
        with mock.patch.object(ptr, 'run') as commands:
            ptr.build_emitters(ctx)
        configure = commands.call_args_list[0].args[0]
        self.assertEqual(configure[configure.index('-B') + 1], ctx.recompiler_build)
        self.assertEqual(configure[configure.index('-S') + 1], ptr.FRAMEWORK / 'recompiler')
        self.assertIn('-DCMAKE_C_COMPILER=gcc', configure)
        self.assertEqual(commands.call_args_list[1].args[1], 'emitter build')
        self.assertEqual(ctx.recompiler_dir, ctx.recompiler_build)

    def test_portable_and_explicit_off_build_without_training(self):
        for enabled, generator, cross, status in [
                (False, 'Ninja', False, 'disabled-explicit'),
                (True, 'Unix Makefiles', False, 'portable-unprofiled'),
                (True, 'Ninja', True, 'portable-unprofiled')]:
            commands, selected = self.configure(enabled, generator, cross)
            selected.assert_not_called()
            self.assertEqual(self.ctx.runtime_pgo['status'], status)
            self.assertEqual(commands.call_args_list[-1].args[1], 'runtime build')

    def test_delegates_all_phases_and_binds_the_exact_training_profiles(self):
        ctx = self.ctx
        directory = ctx.runtime_build / 'runtime-hot-pgo/psx-runtime'
        directory.mkdir(parents=True)
        profiles = {source: dict(path='private/' + source, bytes=24, sha256='a' * 64)
                    for source in ptr.runtime_hot_pgo.SOURCES}
        phases = []

        def command(argv, what, **kwargs):
            phase = argv[2]
            phases.append(phase)
            self.assertEqual(argv[-1], 2)
            (directory / (phase + '.json')).write_text(json.dumps(dict(
                status='built', profiles=profiles,
                selected_sources=list(ptr.runtime_hot_pgo.SOURCES))))

        def training(*args):
            (directory / 'training.json').write_text('{}')
            return dict(profiles=profiles, frame_completion=dict(first=1, last=601, frames=600))

        cache = dict(CMAKE_COMMAND='cmake', CMAKE_GENERATOR='Ninja', CMAKE_C_COMPILER='gcc',
                     CMAKE_CXX_COMPILER='g++', CMAKE_MAKE_PROGRAM='ninja')
        with mock.patch.object(ptr, 'run', side_effect=command), \
                mock.patch.object(ptr, 'train_release_pgo', side_effect=training):
            ptr.build_selective_pgo(ctx, ['-DPSX_DEBUG_TOOLS=OFF', '-DPSX_SOURCE_COMMIT=fixture'], cache)
        self.assertEqual(phases, ['baseline', 'generate', 'use'])
        recipe = json.loads((ctx.work / 'runtime-pgo-recipe.json').read_text())
        self.assertFalse(any('PSX_DEBUG_TOOLS' in arg for arg in recipe['configure_args']))
        self.assertTrue(any('PSX_SOURCE_COMMIT' in arg for arg in recipe['configure_args']))
        self.assertTrue(all('path' not in row for row in ctx.runtime_pgo['profiles'].values()))
        self.assertEqual(set(ctx.runtime_pgo['receipts']), {'baseline', 'generate', 'training', 'use'})

    def training_files(self):
        build = self.ctx.runtime_build
        (build / 'psxrecomp_exe_name-psx-runtime.txt').write_text('Fixture')
        (build / 'Fixture.exe').write_bytes(b'unit-test executable, never run')
        (build / 'bios').mkdir()
        (build / 'bios/openbios.bin').write_bytes(b'\0' * ptr.OPENBIOS_SIZE)
        directory = build / 'runtime-hot-pgo/psx-runtime'
        directory.mkdir(parents=True)
        (directory / 'generate.json').write_text('{}')
        return directory

    def frame_boot(self, process):
        def launch(*args, **kwargs):
            count = self.ctx.config.pgo_train_frames
            kwargs['stdout'].write(('[BENCH] window=1:%d frames=%d wall_ms=1\n'
                                    '[BENCH] self_exit=normal frame=%d\n' %
                                    (count + 1, count, count + 1)).encode())
            kwargs['stdout'].flush()
            return process
        return launch

    def test_linux_dotted_basename_keeps_the_full_execution_sidecar_name(self):
        ctx = self.ctx
        ctx.tc.exe_suffix = ''
        (ctx.runtime_build / 'psxrecomp_exe_name-psx-runtime.txt').write_text('Game.v2')
        cache = dict(CMAKE_COMMAND='cmake', CMAKE_GENERATOR='Ninja', CMAKE_C_COMPILER='gcc',
                     CMAKE_CXX_COMPILER='g++', CMAKE_MAKE_PROGRAM='ninja')
        with mock.patch.object(ptr, 'run', side_effect=ptr.PackagingError('stop before any builder')):
            with self.assertRaisesRegex(ptr.PackagingError, 'stop before any builder'):
                ptr.build_selective_pgo(ctx, [], cache)
        recipe = json.loads((ctx.work / 'runtime-pgo-recipe.json').read_text())
        self.assertEqual(recipe['binary'], 'Game.v2')
        self.assertEqual(recipe['companions'], ['Game.v2.execution.json'])

    def training_process(self, code=0):
        process = mock.Mock(pid=123, returncode=None)
        process.poll.side_effect = lambda: process.returncode
        def wait(*args, **kwargs):
            self.assertEqual((args, kwargs), ((), {}), 'normal flush has no deadline')
            process.returncode = code
            return code
        process.wait.side_effect = wait
        return process

    def test_slow_startup_waits_for_finite_guest_work_before_exit(self):
        self.training_files()
        process = self.training_process()
        launched, polls = {}, []
        def launch(*args, **kwargs):
            launched.update(kwargs)
            return process
        def poll():
            polls.append(True)
            if len(polls) == 2:
                launched['stdout'].write(b'[BENCH] window=1:')
                launched['stdout'].flush()
            if len(polls) == 3:
                launched['stdout'].write(b'601 frames=600 wall_ms=999999\n[BENCH] self_exit=normal frame=601\n')
                launched['stdout'].flush()
            return process.returncode
        process.poll.side_effect = poll
        with mock.patch.object(ptr.subprocess, 'Popen', side_effect=launch), \
                mock.patch.object(ptr.time, 'sleep'), \
                mock.patch.object(ptr.time, 'monotonic', side_effect=[0, 100000]), \
                mock.patch.object(ptr.runtime_hot_pgo, 'verify_inputs'), \
                mock.patch.object(ptr.runtime_hot_pgo, 'profiles', return_value={'validated': True}):
            result = ptr.train_release_pgo(self.ctx, {'commands': {}})
        self.assertGreaterEqual(len(polls), 3)
        self.assertEqual(result['frame_completion'], {'first': 1, 'last': 601, 'frames': 600})
        self.assertEqual(result['elapsed_seconds'], 100000)
        process.wait.assert_called_once_with()
        process.terminate.assert_not_called()
        process.kill.assert_not_called()

    def test_boot_profiles_require_guest_completion_normal_exit_and_profile_validation(self):
        directory = self.training_files()
        process = self.training_process()
        with mock.patch.object(ptr.subprocess, 'Popen', side_effect=self.frame_boot(process)) as launch, \
                mock.patch.object(ptr.runtime_hot_pgo, 'verify_inputs') as inputs, \
                mock.patch.object(ptr.runtime_hot_pgo, 'profiles', return_value={'validated': True}) as profiles:
            result = ptr.train_release_pgo(self.ctx, {'commands': 'fixture commands'})
        self.assertEqual(result['status'], 'trained-boot-only')
        self.assertEqual(result['profiles'], {'validated': True})
        self.assertEqual(result['close']['method'], 'runtime finite guest-frame self-exit')
        self.assertEqual(result['close']['exit_code'], 0)
        inputs.assert_called_once()
        profiles.assert_called_once_with(self.ctx.runtime_build, 'psx-runtime', 'fixture commands')
        env = launch.call_args.kwargs['env']
        self.assertNotIn('PSX_LOAD_SLOT', env)
        self.assertEqual(env['SDL_AUDIODRIVER'], 'dummy')
        self.assertEqual(env['PSX_BENCH_WINDOW'], '1:601')
        self.assertEqual(env['PSX_BENCH_EXIT'], '1')
        process.wait.assert_called_once_with()
        self.assertTrue((directory / 'training.json').is_file())

    def test_explicit_headless_training_uses_existing_no_window_backend(self):
        self.training_files()
        self.ctx.config.pgo_train_headless = True
        process = self.training_process()
        with mock.patch.object(ptr.subprocess, 'Popen', side_effect=self.frame_boot(process)) as launch, \
                mock.patch.object(ptr.runtime_hot_pgo, 'verify_inputs'), \
                mock.patch.object(ptr.runtime_hot_pgo, 'profiles', return_value={'validated': True}):
            result = ptr.train_release_pgo(self.ctx, {'commands': {}})
        self.assertIn('--headless', launch.call_args.args[0])
        self.assertNotIn('--hidden-window', launch.call_args.args[0])
        self.assertEqual(result['presentation'], 'headless-software')
        self.assertEqual(result['frame_completion'], {'first': 1, 'last': 601, 'frames': 600})
        self.assertEqual(result['close']['exit_code'], 0)
        self.assertEqual(launch.call_args.kwargs['env']['SDL_AUDIO_DRIVER'], 'dummy')

    def test_early_exit_is_not_training_even_with_exit_zero(self):
        directory = self.training_files()
        process = self.training_process()
        process.returncode = 0
        with mock.patch.object(ptr.subprocess, 'Popen', return_value=process), \
                mock.patch.object(ptr.runtime_hot_pgo, 'profiles') as profiles:
            with self.assertRaisesRegex(ptr.PackagingError, 'exited early'):
                ptr.train_release_pgo(self.ctx, {})
        profiles.assert_not_called()
        self.assertEqual(json.loads((directory / 'training.json').read_text())['status'], 'failed')

    def test_guest_completion_survives_transient_drvfs_read_without_stopping_child(self):
        process = self.training_process()
        stream = mock.MagicMock()
        stream.tell.return_value = 27
        stream.readline.side_effect = [OSError(ptr.errno.ENODATA, 'No data available'),
                                      b'[BENCH] window=1:601 frames=600 mean_us=1\n']
        with mock.patch.object(Path, 'open') as opened, mock.patch.object(ptr.time, 'sleep'):
            opened.return_value.__enter__.return_value = stream
            result = ptr.wait_training_guest_frames(process, Path('stdout.log'), 600)
        self.assertEqual(result, dict(first=1, last=601, frames=600))
        stream.seek.assert_called_once_with(27)
        process.terminate.assert_not_called()
        process.kill.assert_not_called()

    def test_unrelated_guest_log_read_error_is_still_reported(self):
        stream = mock.MagicMock()
        stream.readline.side_effect = OSError(ptr.errno.EIO, 'I/O failure')
        with mock.patch.object(Path, 'open') as opened:
            opened.return_value.__enter__.return_value = stream
            with self.assertRaises(OSError):
                ptr.wait_training_guest_frames(self.training_process(), Path('stdout.log'), 600)

    def test_nonzero_self_exit_still_rejects_training(self):
        directory = self.training_files()
        process = self.training_process(code=7)
        with mock.patch.object(ptr.subprocess, 'Popen', side_effect=self.frame_boot(process)), \
                mock.patch.object(ptr.runtime_hot_pgo, 'profiles') as profiles:
            with self.assertRaisesRegex(ptr.PackagingError, 'did not close normally'):
                ptr.train_release_pgo(self.ctx, {})
        profiles.assert_not_called()
        self.assertEqual(json.loads((directory / 'training.json').read_text())['close']['exit_code'], 7)
        process.terminate.assert_not_called()
        process.kill.assert_not_called()

    def test_normal_exit_without_three_valid_profiles_still_fails(self):
        directory = self.training_files()
        process = self.training_process()
        with mock.patch.object(ptr.subprocess, 'Popen', side_effect=self.frame_boot(process)), \
                mock.patch.object(ptr.runtime_hot_pgo, 'verify_inputs'), \
                mock.patch.object(ptr.runtime_hot_pgo, 'profiles', side_effect=RuntimeError('missing selected profile')):
            with self.assertRaisesRegex(RuntimeError, 'missing selected profile'):
                ptr.train_release_pgo(self.ctx, {'commands': {}})
        self.assertEqual(json.loads((directory / 'training.json').read_text())['status'], 'failed')

    def test_unmarked_exit_after_guest_work_is_not_runtime_self_exit(self):
        self.training_files()
        process = self.training_process()
        def launch(*args, **kwargs):
            kwargs['stdout'].write(b'[BENCH] window=1:601 frames=600 wall_ms=1\n')
            kwargs['stdout'].flush()
            return process
        with mock.patch.object(ptr.subprocess, 'Popen', side_effect=launch), \
                mock.patch.object(ptr.runtime_hot_pgo, 'profiles') as profiles:
            with self.assertRaisesRegex(ptr.PackagingError, 'did not report its own'):
                ptr.train_release_pgo(self.ctx, {})
        profiles.assert_not_called()

    def test_wrong_completion_retains_owned_child_without_error_cleanup_exit(self):
        directory = self.training_files()
        process = self.training_process()
        def launch(*args, **kwargs):
            kwargs['stdout'].write(b'[BENCH] window=1:301 frames=300 wall_ms=1\n')
            kwargs['stdout'].flush()
            return process
        with mock.patch.object(ptr.subprocess, 'Popen', side_effect=launch):
            with self.assertRaisesRegex(ptr.PackagingError, 'does not match'):
                ptr.train_release_pgo(self.ctx, {})
        record = json.loads((directory / 'training.json').read_text())
        self.assertEqual(record['status'], 'failed')
        self.assertEqual(record['owned_process'], {'pid': 123, 'running': True})
        process.wait.assert_not_called()
        process.terminate.assert_not_called()
        process.kill.assert_not_called()


class DiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def ctx(self, channel):
        ctx = ptr.Context()
        ctx.config, ctx.channel = self.fixture.config(), channel
        return ctx

    def test_recording_and_display_defaults_are_independent_of_channel(self):
        for staged in ptr.CHANNELS:
            for channel in ptr.CHANNELS:
                with self.subTest(staged=staged, channel=channel):
                    stage = Path(self.tmp.name) / ('%s-%s' % (staged, channel))
                    write_stage(stage, staged)
                    ptr.check_diagnostics(self.ctx(channel), stage)
                    self.assertEqual(ptr.check_diagnostics_dir(SimpleNamespace(
                        bundled=stage / 'mods/bundled', channel=channel)), 0)

    def test_disabled_recording_or_enabled_display_is_rejected_by_both_gates(self):
        for channel in ptr.CHANNELS:
            for recording, osd in (('false', 'false'), ('true', 'true')):
                with self.subTest(channel=channel, recording=recording, osd=osd):
                    stage = Path(self.tmp.name) / ('%s-%s-%s' % (channel, recording, osd))
                    write_stage(stage, channel)
                    manifest = stage / 'mods/bundled/fixture.diagnostics/1.0.0/manifest.toml'
                    manifest.write_text(DIAG.format(recording=recording, osd=osd))
                    with self.assertRaisesRegex(ptr.PackagingError, 'default_enabled'):
                        ptr.check_diagnostics(self.ctx(channel), stage)
                    with self.assertRaisesRegex(ptr.PackagingError, 'default_enabled'):
                        ptr.check_diagnostics_dir(SimpleNamespace(
                            bundled=stage / 'mods/bundled', channel=channel))
                    refresh_manifest(stage)
                    with self.assertRaisesRegex(ptr.PackagingError, 'default_enabled'):
                        ptr.verify_payload(stage, None, self.fixture.config(),
                                           channel, 'windows', 'v1.2.3')

    def test_recording_only_catalog_preserves_legacy_control_and_required_default(self):
        for channel in ptr.CHANNELS:
            for name in ('recording', 'input-recording'):
                with self.subTest(channel=channel, feature=name):
                    stage = Path(self.tmp.name) / (channel + '-' + name)
                    write_stage(stage, channel)
                    manifest = stage / 'mods/bundled/fixture.diagnostics/1.0.0/manifest.toml'
                    text = DIAG.format(recording='true', osd='false')
                    text = text.replace('[[feature]]\nid = "osd"\ndefault_enabled = false\n', '')
                    text = text.replace('[[plugin]]\nfeature = "osd"\nid = "fixture.diagnostics.osd"\n', '')
                    text = text.replace('"recording"', '"' + name + '"')
                    manifest.write_text(text)
                    ptr.check_diagnostics(self.ctx(channel), stage)
                    refresh_manifest(stage)
                    ptr.verify_payload(stage, None, self.fixture.config(), channel, 'windows', 'v1.2.3')
                    manifest.write_text(text.replace('default_enabled = true', 'default_enabled = false'))
                    with self.assertRaisesRegex(ptr.PackagingError, 'default_enabled'):
                        ptr.check_diagnostics(self.ctx(channel), stage)

    def test_undeclared_diagnostics_package_fails(self):
        (self.fixture.root / 'release.toml').write_text(
            CONFIG.format(extra='').replace(
                '[diagnostics]\ngame_id = "SLUS-00000"\npackage_id = "fixture.diagnostics"\n', ''))
        stage = Path(self.tmp.name) / 'stage'
        write_stage(stage, 'public')
        with self.assertRaisesRegex(ptr.PackagingError, 'no \\[diagnostics\\]'):
            ptr.check_diagnostics(self.ctx('public'), stage)

    def test_legacy_inputs_control_uses_all_recording_gates(self):
        for package in ('mmx4.diagnostics', 'mmx5.diagnostics'):
            for channel in ptr.CHANNELS:
                with self.subTest(package=package, channel=channel):
                    stage = Path(self.tmp.name) / (package + '-' + channel)
                    write_stage(stage, channel, package_id=package)
                    manifest = stage / 'mods/bundled' / package / '1.0.0/manifest.toml'
                    text = manifest.read_text().replace('"recording"', '"inputs"')
                    manifest.write_text(text)
                    ctx = self.ctx(channel)
                    ctx.config.diagnostics['package_id'] = package
                    ptr.check_diagnostics(ctx, stage)
                    ptr.check_diagnostics_dir(SimpleNamespace(
                        bundled=stage / 'mods/bundled', channel=channel))
                    refresh_manifest(stage)
                    ptr.verify_payload(stage, None, ctx.config, channel, 'windows', 'v1.2.3')
                    self.assertEqual(manifest.read_text(), text)
                    manifest.write_text(text.replace('default_enabled = true', 'default_enabled = false'))
                    with self.assertRaisesRegex(ptr.PackagingError, 'default_enabled'):
                        ptr.check_diagnostics(ctx, stage)
                    with self.assertRaisesRegex(ptr.PackagingError, 'default_enabled'):
                        ptr.check_diagnostics_dir(SimpleNamespace(
                            bundled=stage / 'mods/bundled', channel=channel))
                    refresh_manifest(stage)
                    with self.assertRaisesRegex(ptr.PackagingError, 'default_enabled'):
                        ptr.verify_payload(stage, None, ctx.config, channel, 'windows', 'v1.2.3')

    def test_legacy_inputs_alias_does_not_mask_other_recording_controls(self):
        inputs = {'id': 'inputs', 'default_enabled': True}
        recording = {'id': 'recording', 'default_enabled': False}
        self.assertNotIn('recording', ptr.diagnostics_features({
            'id': 'fixture.diagnostics', 'feature': [inputs]}))
        for package in ('mmx4.diagnostics', 'mmx5.diagnostics'):
            self.assertIs(ptr.diagnostics_features({
                'id': package, 'feature': [inputs, recording]})['recording'], recording)


class VerifyZipTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)
        self.config = self.fixture.config()
        self.out = Path(self.fixture.root) / 'dist'
        self.out.mkdir()

    def build(self, channel, staged_channel=None):
        stage = Path(self.fixture.root) / ('stage-' + channel)
        write_stage(stage, staged_channel or channel)
        artifact = self.out / ptr.artifact_name(self.config, 'v1.2.3', channel, 'windows')
        zip_stage(stage, artifact)
        return artifact

    def test_public_and_playtest_verify(self):
        for channel in ptr.CHANNELS:
            manifest = ptr.verify_artifact(self.build(channel), self.config, channel, 'v1.2.3')
            self.assertEqual(manifest['channel'], channel)

    def test_wrong_channel_fails(self):
        artifact = self.build('public')
        with self.assertRaises(ptr.PackagingError):
            ptr.verify_artifact(artifact, self.config, 'playtest', 'v1.2.3')

    def test_tampered_payload_fails(self):
        artifact = self.build('public')
        with zipfile.ZipFile(artifact, 'a') as archive:
            archive.writestr('saves/card.mcd', b'card')
        Path(str(artifact) + '.sha256').write_text(
            '%s  %s\n' % (ptr.sha256(artifact), artifact.name))
        with self.assertRaises(ptr.PackagingError):
            ptr.verify_artifact(artifact, self.config, 'public', 'v1.2.3')

    def test_stale_checksum_fails(self):
        artifact = self.build('public')
        Path(str(artifact) + '.sha256').write_text('0' * 64 + '  x\n')
        with self.assertRaisesRegex(ptr.PackagingError, 'sha256'):
            ptr.verify_artifact(artifact, self.config, 'public', 'v1.2.3')

    def test_self_consistent_forbidden_package_zip_is_rejected(self):
        artifact = self.build('public')
        stage = self.fixture.root / 'stage-public'
        package_id = 'psx.enhancement.fast-loading'
        package = stage / 'mods/bundled' / package_id / '1.0.0'
        package.mkdir(parents=True)
        (package / 'manifest.toml').write_text('id = "%s"\n' % package_id)
        # An empty consumer exclusion list and truthful release manifest used
        # to let this artifact pass. Neither can override the release policy.
        (stage / 'psx_mod_catalog_psx-runtime.excluded.txt').write_text('')
        refresh_manifest(stage)
        zip_stage(stage, artifact)
        with self.assertRaisesRegex(ptr.PackagingError, 'forbidden loading mod'):
            ptr.verify_artifact(artifact, self.config, 'public', 'v1.2.3')


def refresh_manifest(stage):
    manifest = json.loads((stage / 'RELEASE_MANIFEST.json').read_text())
    manifest['mod_packages'] = ptr.staged_mod_ids(stage)
    manifest['files'] = {p.relative_to(stage).as_posix(): ptr.sha256(p)
                         for p in sorted(stage.rglob('*'))
                         if p.is_file() and p.name != 'RELEASE_MANIFEST.json'}
    (stage / 'RELEASE_MANIFEST.json').write_text(json.dumps(manifest))


class PublicModPolicyTests(unittest.TestCase):
    def setUp(self):
        self.fixture = Fixture()
        self.addCleanup(self.fixture.close)
        self.config = self.fixture.config()
        self.stage = self.fixture.root / 'stage'
        write_stage(self.stage, 'public')

    def verify(self, channel='public'):
        return ptr.verify_payload(self.stage, None, self.config, channel, 'windows', 'v1.2.3')

    def test_both_ids_are_rejected_even_under_renamed_package_directories(self):
        for package_id in sorted(ptr.PUBLIC_FORBIDDEN_MODS):
            with self.subTest(package_id=package_id):
                package = self.stage / 'mods/bundled/renamed/1.0.0'
                package.mkdir(parents=True)
                (package / 'manifest.toml').write_text('id = "%s"\n' % package_id)
                refresh_manifest(self.stage)
                with self.assertRaisesRegex(ptr.PackagingError, 'forbidden loading mod'):
                    self.verify()
                shutil.rmtree(package.parent)

    def test_physical_paths_without_manifests_are_rejected(self):
        for package_id in sorted(ptr.PUBLIC_FORBIDDEN_MODS):
            with self.subTest(package_id=package_id):
                package = self.stage / 'mods/bundled' / package_id.upper()
                package.mkdir()
                # No catalog id or file hash claims this empty leftover path.
                with self.assertRaisesRegex(ptr.PackagingError, 'forbidden loading mod package path'):
                    self.verify()
                package.rmdir()

    def test_manifest_only_claim_is_rejected(self):
        manifest_path = self.stage / 'RELEASE_MANIFEST.json'
        manifest = json.loads(manifest_path.read_text())
        manifest['mod_packages'].append('psx.enhancement.cd-speed')
        manifest_path.write_text(json.dumps(manifest))
        with self.assertRaisesRegex(ptr.PackagingError, 'forbidden loading mod'):
            self.verify()

    def test_authored_title_loading_package_is_allowed(self):
        package = self.stage / 'mods/bundled/fixture.fast-loading/1.0.0'
        package.mkdir(parents=True)
        (package / 'manifest.toml').write_text('id = "fixture.fast-loading"\n')
        refresh_manifest(self.stage)
        self.verify()

    def test_playtest_release_also_rejects_loading_packages(self):
        self.stage = self.fixture.root / 'stage-playtest'
        write_stage(self.stage, 'playtest')
        package = self.stage / 'mods/bundled/psx.enhancement.fast-loading/1.0.0'
        package.mkdir(parents=True)
        (package / 'manifest.toml').write_text('id = "psx.enhancement.fast-loading"\n')
        refresh_manifest(self.stage)
        with self.assertRaisesRegex(ptr.PackagingError, 'forbidden loading mod'):
            self.verify('playtest')


if __name__ == '__main__':
    unittest.main()
