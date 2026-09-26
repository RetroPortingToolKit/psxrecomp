"""runtime/overlay_static_sources.cmake and hash_codegen.cmake's source-list mode.

Configures throw-away CMake projects (LANGUAGES NONE: no compiler is involved)
that call psxrecomp_overlay_static_sources() the way runtime.cmake does, and
checks what a title's configure reports and links for each state of its static
overlay shard. Also pins that `hash_codegen.cmake -DPSXRECOMP_CODEGEN_HASH_ROOT`
(what aot_overlay_pipeline.py static runs before the runtime is ever built)
computes the same hash psxrecomp-game bakes in.

Run: ctest -R overlay_static_sources_cmake
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
# ctest passes the tools explicitly; under pytest the cmake on PATH is used and
# the recompiler comes from PSXRECOMP_GAME (the CLI's override variable).
ARGS = argparse.Namespace(cmake=shutil.which('cmake') or '', generator='', make_program='',
                          recompiler=os.environ.get('PSXRECOMP_GAME', ''))


def cmake_list(path):
    return Path(path).as_posix()


class OverlayStaticSourcesTest(unittest.TestCase):
    def setUp(self):
        if not ARGS.cmake:
            self.skipTest('no cmake: pass --cmake or put cmake on PATH')
        self.tmp = tempfile.TemporaryDirectory()
        self.project = Path(self.tmp.name) / 'title'
        (self.project / 'generated').mkdir(parents=True)
        self.static_c = self.project / 'generated' / 'overlays_static.c'

    def tearDown(self):
        self.tmp.cleanup()

    def profile(self, **fields):
        (self.project / 'aot').mkdir(exist_ok=True)
        (self.project / 'aot' / 'overlays.json').write_text(json.dumps(
            dict(schema='psxrecomp AOT methods v1', game_id='TEST-00001', **fields)))

    def configure(self, static_c, game_linked=True):
        (self.project / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(overlay_static_probe NONE)
include("{cmake_list(ROOT / 'runtime' / 'overlay_static_sources.cmake')}")
psxrecomp_overlay_static_sources(srcs present
    TARGET      probe
    STATIC_C    "{cmake_list(static_c) if static_c else ''}"
    PROFILE     "${{CMAKE_CURRENT_SOURCE_DIR}}/aot/overlays.json"
    GAME_LINKED {"TRUE" if game_linked else "FALSE"})
message(STATUS "PROBE_PRESENT=${{present}}")
message(STATUS "PROBE_SOURCES=${{srcs}}")
''')
        build = self.project / 'build'
        cmd = [ARGS.cmake, '-S', str(self.project), '-B', str(build)]
        if ARGS.generator:
            cmd += ['-G', ARGS.generator]
        if ARGS.make_program:
            cmd += [f'-DCMAKE_MAKE_PROGRAM={ARGS.make_program}']
        result = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
        return result.returncode, result.stdout + result.stderr, build

    @staticmethod
    def probe(output, key):
        for line in output.splitlines():
            if f'{key}=' in line:
                return line.split(f'{key}=', 1)[1].strip()
        raise AssertionError(f'{key} not reported:\n{output}')

    def test_declared_but_absent_warns_only_when_game_c_is_linked(self):
        self.profile(static_output='generated/overlays_static.c')
        code, out, build = self.configure(self.static_c)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.probe(out, 'PROBE_PRESENT'), 'FALSE')
        self.assertIn('links NO static', out)
        self.assertIn('psxrecomp_cli.py generate', out)
        # The dispatcher is globbed with CONFIGURE_DEPENDS, so a later Generate
        # re-runs configure at build time rather than being ignored.
        verify = (build / 'CMakeFiles' / 'VerifyGlobs.cmake').read_text()
        self.assertIn(cmake_list(self.static_c), verify)
        code, out, _ = self.configure(self.static_c, game_linked=False)
        self.assertEqual(code, 0, out)
        self.assertNotIn('links NO static', out, 'a setup host has no game C to accelerate')

    def test_present_dispatcher_links_with_its_sorted_units(self):
        self.profile(static_output='generated/overlays_static.c')
        for name in ('overlays_static.c', 'overlays_static_0001.c', 'overlays_static_0000.c',
                     'overlays_static_extra.c'):
            (self.project / 'generated' / name).write_text('\n')
        code, out, _ = self.configure(self.static_c)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.probe(out, 'PROBE_PRESENT'), 'TRUE')
        names = [Path(p).name for p in self.probe(out, 'PROBE_SOURCES').split(';')]
        self.assertEqual(names, ['overlays_static.c', 'overlays_static_0000.c',
                                 'overlays_static_0001.c'])
        self.assertNotIn('links NO static', out)

    def test_profile_and_cmake_must_name_the_same_file(self):
        self.profile(static_output='generated/overlays_static.c')
        code, out, _ = self.configure(None)
        self.assertNotEqual(code, 0)
        self.assertIn('passes no GAME_OVERLAY_STATIC_C', out)
        other = self.project / 'elsewhere' / 'overlays_static.c'
        code, out, _ = self.configure(other)
        self.assertNotEqual(code, 0)
        self.assertIn('Make them name the same file', out)

    def test_undeclared_profiles_and_capture_built_shards_are_unaffected(self):
        # A release/DLL-cache title: profile without static_output, no static C.
        self.profile(images=[])
        code, out, _ = self.configure(None)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.probe(out, 'PROBE_PRESENT'), 'FALSE')
        # A capture-built shard (compile_overlays --static) with no profile.
        (self.project / 'aot' / 'overlays.json').unlink()
        (self.project / 'generated' / 'overlays_static.c').write_text('\n')
        code, out, _ = self.configure(self.static_c)
        self.assertEqual(code, 0, out)
        self.assertEqual(self.probe(out, 'PROBE_PRESENT'), 'TRUE')

    def test_malformed_profile_is_a_configure_error(self):
        (self.project / 'aot').mkdir()
        (self.project / 'aot' / 'overlays.json').write_text('{ "static_output": ')
        code, out, _ = self.configure(self.static_c)
        self.assertNotEqual(code, 0)
        self.assertIn('cannot read static_output', out)


class CodegenHashHeaderTest(unittest.TestCase):
    def test_source_list_mode_matches_the_hash_psxrecomp_game_bakes(self):
        if not ARGS.cmake or not ARGS.recompiler:
            self.skipTest('pass --cmake and --recompiler <psxrecomp-game> (or set PSXRECOMP_GAME)')
        sys.path.insert(0, str(ROOT / 'tools'))
        import aot_overlay_pipeline as pipeline
        with tempfile.TemporaryDirectory() as tmp:
            header = pipeline.write_codegen_hash_header(ARGS.cmake, Path(tmp) / 'overlay_codegen_hash.h')
            written = pipeline.compiler.codegen_hash(tmp)
        baked = subprocess.run([ARGS.recompiler, '--codegen-hash'], capture_output=True,
                               text=True).stdout.strip()
        self.assertEqual(f'{written:08x}', baked.lower(), header)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmake', default=ARGS.cmake)
    parser.add_argument('--generator', default='')
    parser.add_argument('--make-program', default='')
    parser.add_argument('--recompiler', default=ARGS.recompiler)
    ARGS, rest = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + rest)
