#!/usr/bin/env python3
"""Overlay shards and BIOS modules are compiled for the RUNTIME's architecture.

THE BUG
=======
An x86_64 macOS runtime running under Rosetta compiled every overlay shard for
arm64. The chain was: the runtime spawns `sh -c <compile command>`; the shell
and the bundled Python inherit x86_64 from it, so compile_overlays.py named the
cache `macos-x64` (platform.machine() is the process's own architecture). But
it ran the compiler without `-arch`, and Xcode's clang is an arm64-only binary
whose default target is its own architecture. So arm64 shards landed in the
macos-x64 cache, the x64 runtime could not load one of them, and every overlay
ran on the interpreter. Nothing failed loudly. bios_module_build.py had the
same compile line.

THE CONTRACT PINNED HERE
========================
* The runtime exports its own "<os>-<arch>" (overlay_loader.h's
  PSX_OVERLAY_ARCH_ABI: the running slice, for a universal binary) to every
  compile it spawns, as PSX_OVERLAY_ARCH_ABI, and passes it to
  bios_module_build.py as --arch-abi.
* compile_overlays.py builds for that architecture: the cache directory names
  it, and on macOS the compiler is told it with `-arch`.
* Shards are validated by loading them into the Python that built them, so a
  Python that cannot load the runtime's architecture refuses up front instead
  of writing shards nobody loads. The Python's architecture is its process's:
  platform.machine() on macOS and Linux, but the interpreter's build on
  Windows, where platform.machine() reports the machine (an x64 Python
  emulated on Windows on ARM says ARM64; it must still build x64 shards).
* Windows and Linux compile exactly as before: no new flags, the same
  directory.

The hermetic cases run everywhere. The real-toolchain cases need a POSIX C
compiler (--cc); the per-slice cases need macOS, and the x86_64 slice needs
Rosetta on Apple silicon.

    python3 runtime/tests/test_overlay_compile_arch.py [--cc cc]
    ctest -R overlay_compile_arch_test
"""

import argparse
import contextlib
import io
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
TOOLS = os.path.join(ROOT, 'tools')
INCLUDE = os.path.join(ROOT, 'runtime', 'include')
sys.path.insert(0, TOOLS)

import compile_overlays as co  # noqa: E402
import bios_module_build as bmb  # noqa: E402

CC = 'cc'   # replaced from --cc in __main__


# What sysconfig.get_platform() says for a native Windows Python built for
# the machine it runs on (python.org spelling).
_WIN_BUILD = {'AMD64': 'win-amd64', 'ARM64': 'win-arm64', 'x86': 'win32'}


@contextlib.contextmanager
def host(system, machine, env_arch=None, build=None):
    """Pretend this interpreter runs on `system`/`machine` (what
    platform.system()/machine() report), optionally spawned by a runtime that
    exported PSX_OVERLAY_ARCH_ABI=env_arch. On Windows platform.machine() is
    the machine's, and `build` is what sysconfig.get_platform() reports for
    the interpreter (default: built for that machine)."""
    env = {k: v for k, v in os.environ.items() if k != 'PSX_OVERLAY_ARCH_ABI'}
    if env_arch is not None:
        env['PSX_OVERLAY_ARCH_ABI'] = env_arch
    saved_os, saved_arch = co._TARGET_OS, co._TARGET_ARCH
    co.set_target_os(None)
    co.set_target_arch(None)
    if build is None:
        build = _WIN_BUILD.get(machine, 'win32') if system == 'Windows' else 'posix-build'
    try:
        with mock.patch.object(co.sysconfig, 'get_platform', return_value=build), \
             mock.patch.object(co.platform, 'system', return_value=system), \
             mock.patch.object(co.platform, 'machine', return_value=machine), \
             mock.patch.object(co.os, 'name', 'nt' if system == 'Windows' else 'posix'), \
             mock.patch.dict(os.environ, env, clear=True):
            yield
    finally:
        co.set_target_os(saved_os)
        co.set_target_arch(saved_arch)


class _Ran:
    """Records the compiler command instead of running it."""
    def __init__(self):
        self.cmds = []

    def __call__(self, cmd, *a, **kw):
        self.cmds.append(list(cmd))
        return subprocess.CompletedProcess(cmd, 0, '', '')


_FIXTURE = None


def fixture():
    """One shard source and include dir for every recorded command (the tcc
    path reads both), so commands from different cases compare equal."""
    global _FIXTURE
    if _FIXTURE is None:
        root = tempfile.mkdtemp(prefix='psx_ovl_arch_fx_')
        os.makedirs(os.path.join(root, 'inc'))
        with open(os.path.join(root, 'inc', 'h.h'), 'w') as f:
            f.write('\n')
        _FIXTURE = root
    return _FIXTURE


def shard_command(compiler='gcc', gcc='gcc'):
    root = fixture()
    src = os.path.join(root, 'shard.c')
    with open(src, 'w') as f:
        f.write('int overlay_abi(void) { return 0; }\n')
    ran = _Ran()
    with mock.patch.object(co.subprocess, 'run', ran), \
         contextlib.redirect_stdout(io.StringIO()):
        co._compile_dll_direct(src, os.path.join(root, 'out.so'),
                               [os.path.join(root, 'inc')],
                               gcc=gcc, flavor=0, compiler=compiler, tcc='tcc')
    return ran.cmds[-1]


def bios_module_command(gcc='gcc'):
    ran = _Ran()
    with mock.patch.object(bmb.subprocess, 'run', ran), \
         contextlib.redirect_stdout(io.StringIO()):
        bmb.compile_module(['/x/a.c'], '/x/m.so', ['/x/inc'],
                           'SCPH1001', 0, 'gcc', gcc, 'tcc')
    return ran.cmds[-1]


def arch_args(cmd):
    return [cmd[i + 1] for i, a in enumerate(cmd[:-1]) if a == '-arch']


def run_main_until_guard(argv):
    """compile_overlays.main() as the runtime spawns it, up to the arch guard.
    Returns (exit code, stdout). The game.toml does not exist on purpose: a
    run that gets past the guard fails there instead (FileNotFoundError)."""
    out = io.StringIO()
    err = io.StringIO()
    with mock.patch.object(sys, 'argv', ['compile_overlays.py'] + argv), \
         contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        try:
            co.main()
        except SystemExit as exc:
            return exc.code, out.getvalue() + err.getvalue()
        except FileNotFoundError:
            return 'past-guard', out.getvalue() + err.getvalue()
    return 0, out.getvalue()


MAIN_ARGV = ['--game-toml', '/nonexistent/game.toml', '--recompiler', '/nonexistent/r',
             '--runtime-include', INCLUDE, '--captures', '/nonexistent/c.json']


class ToolTargetsRuntimeArch(unittest.TestCase):
    """compile_overlays.py / bios_module_build.py, with the host mocked."""

    def test_rosetta_x64_runtime_gets_x86_64_shards(self):
        # The reported case: x86_64 runtime, x86_64 Python (Rosetta).
        with host('Darwin', 'x86_64', 'macos-x64'):
            self.assertIsNone(co.apply_runtime_arch_abi(
                os.environ['PSX_OVERLAY_ARCH_ABI'], 'PSX_OVERLAY_ARCH_ABI'))
            self.assertEqual(co.cache_arch_abi(), 'macos-x64')
            self.assertIsNone(co.interpreter_arch_mismatch())
            self.assertEqual(arch_args(shard_command()), ['x86_64'])
            self.assertEqual(arch_args(bios_module_command()), ['x86_64'])

    def test_arm64_runtime_gets_arm64_shards(self):
        with host('Darwin', 'arm64', 'macos-arm64'):
            self.assertIsNone(co.apply_runtime_arch_abi('macos-arm64', 'env'))
            self.assertEqual(co.cache_arch_abi(), 'macos-arm64')
            self.assertEqual(arch_args(shard_command()), ['arm64'])
            self.assertEqual(arch_args(bios_module_command()), ['arm64'])

    def test_manual_run_on_macos_names_the_interpreters_arch(self):
        # No runtime pin (offline/AOT use): build for this interpreter, and
        # still say so, so an x86_64 Python never gets arm64 output.
        with host('Darwin', 'x86_64'):
            self.assertEqual(co.cache_arch_abi(), 'macos-x64')
            self.assertEqual(arch_args(shard_command()), ['x86_64'])
        with host('Darwin', 'arm64'):
            self.assertEqual(co.cache_arch_abi(), 'macos-arm64')
            self.assertEqual(arch_args(shard_command()), ['arm64'])

    def test_main_adopts_the_runtime_pin(self):
        with host('Darwin', 'x86_64', 'macos-x64'):
            code, out = run_main_until_guard(MAIN_ARGV)
            self.assertEqual(code, 'past-guard', out)
            self.assertEqual(co.cache_arch_abi(), 'macos-x64')
        # The env wins over --arch-abi, like the cache dir and flavor pins.
        with host('Darwin', 'x86_64', 'macos-x64'):
            code, out = run_main_until_guard(MAIN_ARGV + ['--arch-abi', 'macos-arm64'])
            self.assertEqual(code, 'past-guard', out)
            self.assertIn('PSX_OVERLAY_ARCH_ABI overrides --arch-abi', out)
            self.assertEqual(co.cache_arch_abi(), 'macos-x64')

    def test_bios_module_env_pin_beats_flag(self):
        with host('Darwin', 'x86_64', 'macos-x64'), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            with self.assertRaises(SystemExit):  # past arch guard: no dump
                bmb.main(['--dump', '/nonexistent', '--stem', 'SCPH1001',
                          '--out', '/nonexistent/m.so', '--arch-abi', 'macos-arm64'])
            self.assertIn('PSX_OVERLAY_ARCH_ABI overrides --arch-abi: macos-x64',
                          out.getvalue())
            self.assertIn('not found', out.getvalue())
            self.assertNotIn('cannot build a BIOS module', out.getvalue())
            self.assertEqual(co.cache_arch_abi(), 'macos-x64')

    def test_python_that_cannot_load_the_runtimes_arch_refuses(self):
        # A universal runtime's arm64 slice spawning an x86_64-only Python:
        # the shards could never be validated here, so build none, loudly.
        with host('Darwin', 'x86_64', 'macos-arm64'):
            code, out = run_main_until_guard(MAIN_ARGV)
            self.assertEqual(code, 2, out)
            self.assertIn('cannot build overlay shards', out)
            self.assertIn('macos-arm64', out)
        # --static writes C only and loads nothing: no guard.
        with host('Darwin', 'x86_64', 'macos-arm64'):
            code, out = run_main_until_guard(MAIN_ARGV + ['--static'])
            self.assertEqual(code, 'past-guard', out)
        with host('Darwin', 'x86_64', 'macos-arm64'), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            with self.assertRaises(SystemExit):
                bmb.main(['--dump', '/nonexistent', '--stem', 'SCPH1001',
                          '--out', '/nonexistent/m.so'])
            self.assertIn('cannot build a BIOS module', out.getvalue())

    def test_bad_or_foreign_pins_are_rejected(self):
        with host('Darwin', 'arm64'):
            for bad in ('macos', 'macos-ppc', 'test-arch', 'darwin-arm64'):
                self.assertIsNotNone(co.apply_runtime_arch_abi(bad, 'env'), bad)
            self.assertIn('built for macos', co.apply_runtime_arch_abi('win-x64', 'env'))
            self.assertEqual(co.cache_arch_abi(), 'macos-arm64')   # unchanged
        with host('Darwin', 'arm64', 'linux-x64'):
            code, out = run_main_until_guard(MAIN_ARGV)
            self.assertEqual(code, 2, out)
            self.assertIn('PSX_OVERLAY_ARCH_ABI', out)

    def test_windows_is_unaffected(self):
        for system, machine, gcc in (('Windows', 'AMD64', r'C:\msys64\mingw64\bin\gcc.exe'),
                                     ('MSYS_NT-10.0-26100', 'x86_64', 'gcc')):
            with host(system, machine):
                before = (co.cache_arch_abi(), shard_command(gcc=gcc),
                          shard_command('tcc'), bios_module_command(gcc))
            with host(system, machine, 'win-x64'):
                self.assertIsNone(co.apply_runtime_arch_abi('win-x64', 'env'))
                after = (co.cache_arch_abi(), shard_command(gcc=gcc),
                         shard_command('tcc'), bios_module_command(gcc))
                self.assertIsNone(co.interpreter_arch_mismatch())
            self.assertEqual(before, after)
            self.assertEqual(after[0], 'win-x64')
            for cmd in after[1:]:
                self.assertEqual(arch_args(cmd), [], cmd)

    def test_windows_process_arch_comes_from_the_build(self):
        # platform.machine() on Windows is the MACHINE's (WMI since 3.12):
        # an x64 Python emulated on Windows on ARM says ARM64, a 32-bit one
        # on x64 Windows says AMD64. The interpreter's build is the process.
        for machine, build, want in (('ARM64', 'win-amd64', 'x64'),
                                     ('AMD64', 'win32', 'x86'),
                                     ('ARM64', 'win-arm64', 'arm64'),
                                     ('AMD64', 'mingw_x86_64_msvcrt_gnu', 'x64'),
                                     ('AMD64', 'mingw_x86_64_ucrt_llvm', 'x64'),
                                     ('ARM64', 'mingw_aarch64_ucrt_llvm', 'arm64'),
                                     ('AMD64', 'mingw_i686_msvcrt_gnu', 'x86'),
                                     ('AMD64', 'mingw', 'x64'),
                                     ('ARM64', 'mingw', 'arm64'),
                                     ('ARM64', 'win-arm32', 'unknown')):
            with host('Windows', machine, build=build):
                self.assertEqual(co.interpreter_arch(), want, (machine, build))

    def test_windows_on_arm_runs_the_x64_zip_unchanged(self):
        # The x64 zip on Windows on ARM: runtime, Python and tcc all x64,
        # emulated. Same commands and directory as on x64 Windows; neither
        # tool refuses.
        with host('Windows', 'AMD64', 'win-x64'):
            self.assertIsNone(co.apply_runtime_arch_abi('win-x64', 'env'))
            want = (co.cache_arch_abi(), shard_command(gcc='gcc'),
                    shard_command('tcc'), bios_module_command('gcc'))
        with host('Windows', 'ARM64', 'win-x64', build='win-amd64'):
            self.assertIsNone(co.apply_runtime_arch_abi('win-x64', 'env'))
            self.assertIsNone(co.interpreter_arch_mismatch())
            got = (co.cache_arch_abi(), shard_command(gcc='gcc'),
                   shard_command('tcc'), bios_module_command('gcc'))
        self.assertEqual(got, want)
        self.assertEqual(got[0], 'win-x64')
        with host('Windows', 'ARM64', 'win-x64', build='win-amd64'):
            code, out = run_main_until_guard(MAIN_ARGV)
            self.assertEqual(code, 'past-guard', out)
        with host('Windows', 'ARM64', 'win-x64', build='win-amd64'), \
             contextlib.redirect_stdout(io.StringIO()) as out:
            with self.assertRaises(SystemExit):   # past the guard: no dump
                bmb.main(['--dump', '/nonexistent', '--stem', 'SCPH1001',
                          '--out', '/nonexistent/m.dll', '--arch-abi', 'win-x64'])
            self.assertNotIn('cannot build a BIOS module', out.getvalue())
            self.assertIn('not found', out.getvalue())
        # A real mismatch on Windows still refuses: an arm64 runtime cannot
        # use what an x64 Python validates.
        with host('Windows', 'ARM64', 'win-arm64', build='win-amd64'):
            code, out = run_main_until_guard(MAIN_ARGV)
            self.assertEqual(code, 2, out)
            self.assertIn('runs as x64', out)

    def test_linux_is_unaffected(self):
        for machine, tag in (('x86_64', 'linux-x64'), ('aarch64', 'linux-arm64')):
            with host('Linux', machine):
                before = (co.cache_arch_abi(), shard_command(), bios_module_command())
            with host('Linux', machine, tag):
                self.assertIsNone(co.apply_runtime_arch_abi(tag, 'env'))
                after = (co.cache_arch_abi(), shard_command(), bios_module_command())
            self.assertEqual(before, after)
            self.assertEqual(after[0], tag)
            self.assertEqual(arch_args(after[1]), [])

    def test_tcc_never_gets_arch_flags(self):
        with host('Darwin', 'x86_64', 'macos-x64'):
            co.apply_runtime_arch_abi('macos-x64', 'env')
            self.assertEqual(arch_args(shard_command('tcc')), [])


def _runnable_macos_arches():
    """Mach-O arches this Mac can execute: its own, plus x86_64 via Rosetta."""
    if sys.platform != 'darwin':
        return []
    arches = []
    for name in ('arm64', 'x86_64'):
        if subprocess.run(['arch', '-' + name, '/usr/bin/true'],
                          capture_output=True).returncode == 0:
            arches.append(name)
    return arches


def _lipo_archs(path):
    return subprocess.run(['lipo', '-archs', path], capture_output=True,
                          text=True, check=True).stdout.split()


@unittest.skipIf(os.name == 'nt', 'POSIX toolchain cases')
class RealToolchain(unittest.TestCase):
    """The real C compiler and the real runtime sources."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix='psx_ovl_arch_')

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _cc(self, *args):
        r = subprocess.run([CC, *args], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr or r.stdout)

    def _write(self, name, text):
        path = os.path.join(self.tmp, name)
        with open(path, 'w') as f:
            f.write(text)
        return path

    def test_runtime_exports_its_own_arch_to_spawned_compiles(self):
        # autocompile.c + the four loader entry points it calls, nothing else;
        # on macOS built universal and run as each slice the Mac can execute.
        probe = self._write('probe.c', '''
#include "autocompile.h"
#include "overlay_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct OverlayPreparedImage { unsigned id; };
OverlayPreparedImage *overlay_loader_prepare_published(const char *p) { (void)p; return NULL; }
int  overlay_loader_commit_published(OverlayPreparedImage *i) { (void)i; return 0; }
void overlay_loader_discard_prepared(OverlayPreparedImage *i) { (void)i; }
void overlay_loader_rescan(void) { }
int main(void) {
    const char *v;
    autocompile_set_cache_paths("cache", "captures.json");
    autocompile_configure("true", ".");
    v = getenv("PSX_OVERLAY_ARCH_ABI");
    printf("%s\\n", v ? v : "(unset)");
    return !(v && strcmp(v, PSX_OVERLAY_ARCH_ABI) == 0);
}
''')
        exe = os.path.join(self.tmp, 'probe')
        arches = _runnable_macos_arches()
        flags = [a for name in arches for a in ('-arch', name)]
        self._cc(*flags, '-I' + INCLUDE, probe,
                 os.path.join(ROOT, 'runtime', 'src', 'autocompile.c'),
                 '-o', exe, '-lpthread')
        runs = [(['arch', '-' + name, exe], name) for name in arches] or [([exe], None)]
        for argv, name in runs:
            r = subprocess.run(argv, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            got = r.stdout.split()[-1] if r.stdout.split() else ''
            if name is None:
                with host(platform.system(), platform.machine()):
                    want = co.cache_arch_abi()   # same OS, same arch as this host
                self.assertEqual(got, want)
            else:
                self.assertEqual(got, {'arm64': 'macos-arm64',
                                       'x86_64': 'macos-x64'}[name])

    @unittest.skipUnless(sys.platform == 'darwin', 'macOS -arch')
    def test_shards_are_the_target_arch_and_load_in_it(self):
        # compile_overlays' real compile line, for each arch this Mac runs,
        # whatever this Python is; then load the shard in a process of that
        # architecture the way the runtime does (dlopen + overlay_abi).
        shard = self._write('shard.c', 'int overlay_abi(void) { return 0x5A5A; }\n')
        loader = self._write('loader.c', '''
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW);
    int (*abi)(void);
    if (!h) { printf("dlopen: %s\\n", dlerror()); return 1; }
    abi = (int (*)(void))dlsym(h, "overlay_abi");
    return !(abi && abi() == 0x5A5A);
}
''')
        loader_exe = os.path.join(self.tmp, 'loader')
        arches = _runnable_macos_arches()
        self._cc(*[a for n in arches for a in ('-arch', n)], loader, '-o', loader_exe)
        for name in arches:
            tag = {'arm64': 'macos-arm64', 'x86_64': 'macos-x64'}[name]
            out = os.path.join(self.tmp, f'{name}.so')
            with host('Darwin', platform.machine(), tag):
                co.apply_runtime_arch_abi(tag, 'env')
                with contextlib.redirect_stdout(io.StringIO()):
                    ok = co._compile_dll_direct(shard, out, [INCLUDE], gcc=CC)
            self.assertTrue(ok, name)
            self.assertEqual(_lipo_archs(out), [name])
            r = subprocess.run(['arch', '-' + name, loader_exe, out],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, f'{name}: {r.stdout}{r.stderr}')


def tearDownModule():
    if _FIXTURE:
        shutil.rmtree(_FIXTURE, ignore_errors=True)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    opts, rest = ap.parse_known_args()
    CC = opts.cc
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
