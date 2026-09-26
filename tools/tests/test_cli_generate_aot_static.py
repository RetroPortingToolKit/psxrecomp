"""psxrecomp_cli.py generate builds the static overlay shard a title declares.

No disc, emitter, compiler or pipeline process is used: the pipeline process is
replaced by a fake that records its command line and replays scripted output,
so these cases pin the CLI's contract -- when it runs the pipeline, with what,
and how every outcome is reported -- not the pipeline's own behaviour (that is
tools/tests/test_aot_overlay_pipeline.py).
"""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT), str(ROOT / 'tools')]
import psxrecomp_cli as cli  # noqa: E402


class FakePipeline:
    """Stands in for subprocess.Popen of tools/aot_overlay_pipeline.py."""

    def __init__(self, lines, returncode, calls):
        self.lines, self.returncode, self.calls = lines, returncode, calls

    def __call__(self, cmd, **kwargs):
        self.calls.append((cmd, kwargs))
        work = Path(cmd[cmd.index('--work-dir') + 1])
        assert work.is_dir(), 'the CLI hands the pipeline an existing private work dir'
        (work / 'static-compile.log').write_text('evidence\n')
        process = mock.Mock()
        process.stdout = iter(line + '\n' for line in self.lines)
        process.wait.return_value = self.returncode
        return process


class RunAotStaticTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'disc').mkdir()
        self.cue = self.root / 'disc' / 'game.cue'
        self.cue.write_text('FILE "game.bin" BINARY\n')
        self.config = self.root / 'game.toml'
        self.config.write_text('[game]\nid = "TEST-00001"\n')
        self.marker = self.root / 'generated' / 'GAME_000.00_dispatch.c'
        self.marker.parent.mkdir()
        self.marker.write_text('static void psx_cps_mark_game(void) {}\n')
        self.progress = mock.Mock()
        self.calls = []

    def tearDown(self):
        self.tmp.cleanup()

    def profile(self, **fields):
        (self.root / 'aot').mkdir(exist_ok=True)
        (self.root / 'aot' / 'overlays.json').write_text(json.dumps(
            dict(schema='psxrecomp AOT methods v1', game_id='TEST-00001', **fields)))

    def run_step(self, lines=('RESULT_STATIC=built x',), returncode=0, *, working_disc=None,
                 game=None, matched=True, **options):
        fake = FakePipeline(list(lines), returncode, self.calls)
        with mock.patch.object(cli.subprocess, 'Popen', fake), \
             mock.patch.object(cli, 'find_psxrecomp_game', return_value=Path('/emit/psxrecomp-game')), \
             mock.patch.object(cli, 'resolve_embedded_toolchain_bin', return_value=None), \
             mock.patch.object(cli, '_which_tool', side_effect=lambda name: Path(f'/bin/{name}')), \
             mock.patch.object(cli, 'overlay_compiler', return_value=Path('/bin/clang')):
            return cli.run_aot_static(
                self.root, self.config, game=game or {}, working_disc=working_disc or self.cue,
                marker=self.marker, disc_matched_known=matched, progress=self.progress, **options)

    def command(self):
        self.assertEqual(len(self.calls), 1)
        return self.calls[0][0]

    def logged(self):
        return '\n'.join(str(c.args[0]) for c in self.progress.log.call_args_list)

    def test_no_profile_or_no_declaration_runs_nothing(self):
        self.assertEqual(self.run_step(), 'none')
        self.profile(images=[])
        self.assertEqual(self.run_step(), 'none')
        self.assertIn('declares no static_output', self.logged())
        self.assertEqual(self.calls, [])
        self.progress.error.assert_not_called()

    def test_declared_shard_runs_the_framework_pipeline_with_resolved_tools(self):
        self.profile(static_output='generated/overlays_static.c')
        self.assertEqual(self.run_step(), 'built')
        cmd = self.command()
        self.assertEqual(cmd[0], sys.executable)
        self.assertEqual(Path(cmd[1]), cli.framework_root(self.root) / 'tools' / 'aot_overlay_pipeline.py')
        self.assertEqual(cmd[2], 'static')
        value = lambda flag: cmd[cmd.index(flag) + 1]
        self.assertEqual(Path(value('--profile')), self.root / 'aot' / 'overlays.json')
        self.assertEqual(Path(value('--game-toml')), self.config)
        self.assertEqual(Path(value('--disc')), self.cue)
        self.assertEqual(Path(value('--recompiler')), Path('/emit/psxrecomp-game'))
        self.assertEqual(Path(value('--gcc')), Path('/bin/clang'))
        self.assertEqual(Path(value('--cmake')), Path('/bin/cmake'))
        self.assertGreater(int(value('--workers')), 0)
        for flag in ('--cps', '--reuse', '--clear-if-not-applicable'):
            self.assertIn(flag, cmd)
        self.assertNotIn('--out-dir', cmd, 'the profile, not the CLI, names the destination')
        self.assertEqual(self.calls[0][1]['cwd'], str(self.root))
        # A successful run leaves no evidence behind.
        self.assertFalse(Path(value('--work-dir')).exists())
        self.progress.event.assert_called_with('aot_static', status='built')

    def test_reuse_status_and_force_and_legacy_codegen(self):
        self.profile(static_output='generated/overlays_static.c')
        self.assertEqual(self.run_step(['Reused 3 ...', 'RESULT_STATIC=reused x']), 'reused')
        self.calls.clear()
        self.marker.write_text('/* legacy dispatch */\n')
        self.run_step(force=True)
        cmd = self.command()
        self.assertNotIn('--reuse', cmd)
        self.assertNotIn('--cps', cmd, 'overlay C must match the generated game C contract')

    def test_disc_cue_falls_back_to_the_configured_cue(self):
        self.profile(static_output='generated/overlays_static.c')
        chd = self.root / 'disc' / 'game.chd'
        chd.write_bytes(b'')
        self.run_step(working_disc=chd, game={'disc': 'disc/game.cue'})
        cmd = self.command()
        self.assertEqual(Path(cmd[cmd.index('--disc') + 1]), self.cue.resolve())
        self.calls.clear()
        self.assertIsNone(self.run_step(working_disc=chd, game={}))
        self.assertEqual(self.calls, [])
        self.assertIn('cue/bin', self.progress.error.call_args.args[0])

    def test_pipeline_failure_is_an_error_that_keeps_the_evidence(self):
        self.profile(static_output='generated/overlays_static.c')
        lines = ['Static AOT compilation failed; see x.log',
                 'aot_overlay_pipeline: error: Loader evidence changed: SCES 0x1']
        self.assertIsNone(self.run_step(lines, 1))
        message = self.progress.error.call_args.args[0]
        self.assertIn('exit 1', message)
        self.assertIn('Loader evidence changed', message)
        work = Path(self.command()[self.command().index('--work-dir') + 1])
        self.assertIn(str(work), message)
        self.assertTrue((work / 'static-compile.log').is_file())
        self.assertEqual(self.progress.error.call_args.kwargs['code'], cli.EXIT_ERROR)

    def test_not_applicable_disc(self):
        self.profile(static_output='generated/overlays_static.c')
        line = ['aot_overlay_pipeline: not applicable: the AOT profile for TEST-00001 '
                'describes the disc whose data track has sha256 aa, but game.cue has bb']
        # A disc that matched [prepare_disc]'s own digests: the title contradicts itself.
        self.assertIsNone(self.run_step(line, 3, matched=True))
        self.assertIn('disagree', self.progress.error.call_args.args[0])
        self.assertIn('sha256 aa', self.progress.error.call_args.args[0])
        # An unverified dump: reported loudly, Generate still succeeds.
        self.progress.reset_mock()
        self.assertEqual(self.run_step(line, 3, matched=False), 'not_applicable')
        self.progress.error.assert_not_called()
        warning = [c for c in self.progress.log.call_args_list if c.kwargs.get('level') == 'warning']
        self.assertEqual(len(warning), 1)
        self.assertIn('sha256 aa', warning[0].args[0])
        self.progress.event.assert_called_with('aot_static', status='not_applicable',
                                               detail=mock.ANY)

    def test_explicit_opt_out_is_loud(self):
        self.profile(static_output='generated/overlays_static.c')
        self.assertEqual(self.run_step(skip=True), 'skipped')
        self.assertEqual(self.calls, [])
        warning = self.progress.log.call_args_list[-1]
        self.assertEqual(warning.kwargs.get('level'), 'warning')
        self.assertIn('interpreter', warning.args[0])

    def test_unreadable_profile_is_an_error(self):
        (self.root / 'aot').mkdir()
        (self.root / 'aot' / 'overlays.json').write_text('{ not json')
        self.assertIsNone(self.run_step())
        self.assertIn('cannot read AOT profile', self.progress.error.call_args.args[0])


class HelpersTest(unittest.TestCase):
    def test_disc_matched_known_digests(self):
        args = argparse.Namespace(skip_hash_check=False)
        known = dict(known_md5=['x'])
        self.assertTrue(cli.disc_matched_known_digests(dict(verified=True), known, args))
        self.assertFalse(cli.disc_matched_known_digests(dict(verified=True), {}, args),
                         'no declared digests is not a known disc')
        self.assertFalse(cli.disc_matched_known_digests(None, known, args))
        self.assertFalse(cli.disc_matched_known_digests(dict(verified=False), known, args))
        self.assertFalse(cli.disc_matched_known_digests(
            dict(verified=True), known, argparse.Namespace(skip_hash_check=True)))


class GenerateWiringTest(unittest.TestCase):
    """cmd_generate runs the shard step after the game C and gates on it."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        (self.root / 'disc').mkdir()
        (self.root / 'disc' / 'GAME_000.00').write_bytes(b'PS-X EXE')
        self.cue = self.root / 'disc' / 'game.cue'
        self.cue.write_text('FILE "game.bin" BINARY\n')
        (self.root / 'game.toml').write_text(
            '[game]\nid = "TEST-00001"\nexe = "disc/GAME_000.00"\ndisc = "disc/game.cue"\n'
            '[prepare_disc]\nout_dir = "disc"\nboot_exe = "GAME_000.00"\n')
        (self.root / 'generated').mkdir()
        (self.root / 'generated' / 'GAME_000.00_dispatch.c').write_text('/* dispatch */\n')

    def tearDown(self):
        self.tmp.cleanup()

    def generate(self, aot_result):
        args = argparse.Namespace(
            config=str(self.root / 'game.toml'), project_root=str(self.root), disc=str(self.cue),
            skip_hash_check=False, force_prepare=False, gen_marker='', force_emitters=False,
            no_toolchain_download=True, bios='', force_bios=False, force_aot_static=True,
            no_aot_static=False)
        progress = mock.Mock()
        events = []
        def emitter(cmd, **kwargs):
            events.append('psxrecomp-game')
            return mock.Mock(returncode=0, stdout='', stderr='')
        def aot(project_root, config, **kwargs):
            events.append(('aot', kwargs['marker'].name, kwargs['working_disc'], kwargs['force']))
            return aot_result
        replacements = dict(
            activate_embedded_toolchain=lambda *a, **k: True,
            verify_disc_path=lambda *a, **k: dict(verified=True),
            ensure_framework=lambda *a, **k: self.root / 'psxrecomp',
            ensure_emitters=lambda *a, **k: (Path('game'), Path('bios')),
            find_emitters=lambda *a, **k: (Path('game'), Path('bios')),
            bios_backend_present=lambda *a, **k: True,
            run_aot_static=aot)
        with mock.patch.multiple(cli, **replacements), \
             mock.patch.object(cli.subprocess, 'run', side_effect=emitter):
            code = cli.cmd_generate(args, progress)
        return code, progress, events

    def test_shard_step_follows_game_c_and_reports_its_status(self):
        code, progress, events = self.generate('built')
        self.assertEqual(code, cli.EXIT_OK)
        self.assertEqual(events, ['psxrecomp-game', ('aot', 'GAME_000.00_dispatch.c', self.cue, True)])
        self.assertEqual(progress.result.call_args.kwargs['aot_static'], 'built')

    def test_shard_failure_fails_generate(self):
        code, progress, events = self.generate(None)
        self.assertEqual(code, cli.EXIT_ERROR)
        progress.result.assert_not_called()


if __name__ == '__main__':
    unittest.main()
