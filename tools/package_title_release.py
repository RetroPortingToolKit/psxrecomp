#!/usr/bin/env python3
"""Cut a PSXRecomp title release: one command, every platform, both channels.

    psxrecomp/tools/package_title_release.py --channel public|playtest
        [--config packaging/release.toml] [--platform windows|linux|all]

Run from the title repository root (or pass --root). The title keeps only its
release config (packaging/release.toml, see docs/RELEASING_A_TITLE.md); every
step below is the framework's, so a release fix lands once for every title.

--channel is REQUIRED and has no default:
  public    public production release. -DPSX_RELEASE_CHANNEL=public, so
            PSX_PUBLIC_RELEASE=ON and Playtest Diagnostics ship default off.
  playtest  non-public playtest package. Diagnostics ship default on and every
            artifact name carries "-playtest".
Every audit runs in the matching mode.

What one run owns, in order (any failure ends with one line
"PACKAGING FAILED: <reason>" and a non-zero exit; nothing warns and continues):
  1. source provenance: title/framework/recomp-ui commits; refuses a dirty
     tracked tree unless --allow-dirty (recorded in the manifest).
  2. emitters: psxrecomp-game AND psxrecomp-bios, always, up front.
  3. source preparation: recompiled OpenBIOS present (regen if missing),
     game C regenerated from the disc, static AOT units when the title's AOT
     mode is "static". On Windows, --platform linux/all prepares once here and
     the Linux build compiles the same audited sources.
  4. configure + build: Release, PSX_DEBUG_TOOLS=OFF, OpenBIOS-only backend
     (PSXRECOMP_BIOS_STEMS=OpenBIOS), PSX_RELEASE_CHANNEL=<channel>,
     PSX_GAME_VERSION from the title's version file; every owned value is
     read back from CMakeCache.txt.
  5. stage: executable, assets, OpenBIOS + notice, configs, mod catalog
     (verified against the build's own catalog list; developer-channel
     features pruned from public packages), overlay cache via the original-
     disc AOT pipeline (AOT mode "cache") and/or overlay toolchain, notices,
     title docs, START_HERE.txt with @VERSION@ stamped, execution contract.
  6. gates: no disc/BIOS dumps/saves/personal state/generated C/build kit;
     OpenBIOS only; diagnostics defaults per channel; Windows executable
     imports only system DLLs; static AOT receipt matches the linked sources.
  7. artifact: Windows ZIP (portable POSIX names) or Linux AppImage
     (tools/package_appimage.sh, fixed linuxdeploy/appimagetool pins), its
     .sha256, RELEASE_MANIFEST.json inside, then the artifact is re-opened /
     extracted and verified (`verify` below) before success is printed.

Linux from Windows: --platform linux (or all) runs the Linux half inside WSL
(default distribution) on the same checkout, from /mnt/<drive>/...

    package_title_release.py verify --channel C [--config ...] ARTIFACT
re-checks a finished ZIP or AppImage by extracting it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform as _platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import zipfile

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    import tomli as tomllib  # type: ignore

TOOLS = Path(__file__).resolve().parent
FRAMEWORK = TOOLS.parent
sys.path.insert(0, str(TOOLS))
import runtime_hot_pgo

CHANNELS = ('public', 'playtest')
PLATFORMS = ('windows', 'linux')
ARTIFACT_SUFFIX = {'windows': 'windows-x64.zip', 'linux': 'linux-x86_64.AppImage'}
TOOLCHAIN_PLATFORM = {'windows': 'win', 'linux': 'linux'}
AOT_MODES = ('cache', 'static', 'none')
OPENBIOS_SIZE = 524288
CREATE_NO_WINDOW = 0x08000000

# Flags the packager owns. A title's [build].cmake_args may not set them.
OWNED_CMAKE = ('CMAKE_BUILD_TYPE', 'PSX_DEBUG_TOOLS', 'PSX_RELEASE_CHANNEL',
               'PSX_PUBLIC_RELEASE', 'PSXRECOMP_BIOS_STEMS', 'PSX_GAME_VERSION',
               'PSX_SOURCE_COMMIT', 'PSXRECOMP_ROOT', 'PSX_RUNTIME_IPO', 'PSX_PGO',
               'CMAKE_EXPORT_COMPILE_COMMANDS', 'CMAKE_PROJECT_INCLUDE',
               'PSX_RUNTIME_HOT_PGO_MODE', 'PSX_RUNTIME_HOT_PGO_TARGET')

# Windows system DLLs a self-contained release executable may import.
SYSTEM_DLLS = frozenset('''
advapi32 bcrypt cfgmgr32 comdlg32 crypt32 d2d1 d3d11 d3d12 dbghelp dinput8 dwmapi
dwrite dxgi gdi32 hid imm32 iphlpapi kernel32 msvcrt ntdll ole32 oleaut32
opengl32 rpcrt4 setupapi shell32 shlwapi ucrtbase user32 userenv uxtheme
version winhttp winmm wintrust ws2_32
'''.split())

# Never in a player package (beyond bios/openbios.bin).
DISC_SUFFIXES = {'.cue', '.iso', '.img', '.chd', '.ccd', '.sub', '.mds', '.mdf',
                 '.pbp', '.ecm', '.mcd', '.mcr', '.pst', '.pack', '.srm', '.gme'}
PERSONAL_NAMES = {'settings.toml', 'disc.cfg', 'bios.cfg', 'rom.cfg', 'state.toml',
                  'state.toml.tmp', 'keybinds.ini', 'overlay_captures.json',
                  'diagnostics-location.txt', 'psx_last_run_report.json',
                  'psx_crash.txt', 'psx_freeze_heartbeat.json'}
BUILD_KIT = ('psxrecomp', 'recomp-ui', 'generated', 'seeds', 'src', 'disc', 'iso',
             'saves', 'toolchain', 'CMakeLists.txt', 'codegen_setup.c',
             'codegen_setup.h', 'psxrecomp_cli.py', 'psxrecomp-game',
             'psxrecomp-game.exe', 'psxrecomp-bios', 'psxrecomp-bios.exe')


class PackagingError(Exception):
    pass


def fail(message):
    raise PackagingError(message)


def log(message=''):
    print(message, flush=True)


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def host_platform():
    if os.name == 'nt':
        return 'windows'
    if sys.platform.startswith('linux'):
        return 'linux'
    fail('unsupported host %s: packaging runs on Windows or Linux' % sys.platform)


def run(command, what, cwd=None, env=None, capture=False):
    """Run a child, stream its output, fail loudly on a non-zero exit.

    Output is piped through this process so CREATE_NO_WINDOW never loses it:
    no child ever opens a console window on the desktop."""
    command = [str(c) for c in command]
    log('+ ' + ' '.join(shlex.quote(c) for c in command))
    kwargs = dict(cwd=str(cwd) if cwd else None, env=env, stdin=subprocess.DEVNULL,
                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if os.name == 'nt':
        kwargs['creationflags'] = CREATE_NO_WINDOW
    try:
        process = subprocess.Popen(command, **kwargs)
    except OSError as error:
        fail('%s: cannot start %s (%s)' % (what, command[0], error))
    captured = []
    for raw in process.stdout:
        line = raw.decode('utf-8', 'replace')
        if capture:
            captured.append(line)
        else:
            sys.stdout.write(line)
            sys.stdout.flush()
    code = process.wait()
    if code != 0:
        if capture:
            sys.stdout.write(''.join(captured))
        fail('%s failed (exit %d)' % (what, code))
    return ''.join(captured)


def read_text(path):
    return Path(path).read_text(encoding='utf-8-sig')


# --------------------------------------------------------------------------
# Config
# --------------------------------------------------------------------------
class Config:
    """packaging/release.toml, validated. Unknown keys are errors (a typo in a
    release config must not silently drop a step)."""

    SCHEMA = {
        '': {'schema', 'product', 'build', 'aot', 'overlay_toolchain', 'diagnostics',
             'payload', 'linux', 'hooks'},
        'product': {'name', 'display_name', 'exe_name', 'version_file', 'game_toml',
                    'player_game_toml', 'runtime_target', 'zip_folder', 'launcher_boxart'},
        'build': {'cmake_args', 'linux_cmake_args', 'windows_cmake_args', 'regenerate',
                  'binary_identity', 'selective_pgo', 'pgo_train_seconds'},
        'aot': {'mode', 'profile', 'cps'},
        'overlay_toolchain': {'ship'},
        'diagnostics': {'game_id', 'package_id', 'replay_helper'},
        'payload': {'start_here', 'files', 'build_files'},
        'linux': {'app_run', 'desktop_file', 'icon', 'payload_dir', 'env_prefix',
                  'prune_unreferenced_libs'},
        'hooks': {'prepare', 'post_build', 'post_stage'},
    }

    def __init__(self, path, root):
        self.path = Path(path)
        self.root = Path(root)
        if not self.path.is_file():
            fail('release config not found: %s' % self.path)
        try:
            data = tomllib.loads(read_text(self.path))
        except (tomllib.TOMLDecodeError, UnicodeError) as error:
            fail('%s: %s' % (self.path, error))
        self._check_keys(data)
        if data.get('schema') != 1:
            fail('%s: schema must be 1' % self.path)
        product = self._table(data, 'product', required=True)
        self.name = self._name(product, 'name')
        self.display_name = self._str(product, 'display_name', self.name)
        self.exe_name = product.get('exe_name')
        if self.exe_name is not None:
            self._name(product, 'exe_name')
        self.version_file = self._file(product, 'version_file', required=True)
        self.game_toml = self._file(product, 'game_toml', default='game.toml')
        self.player_game_toml = self._file(product, 'player_game_toml',
                                           default=product.get('game_toml', 'game.toml'))
        self.runtime_target = self._str(product, 'runtime_target', 'psx-runtime')
        self.zip_folder = self._str(product, 'zip_folder', '')
        if self.zip_folder:
            self._name(product, 'zip_folder')
        self.launcher_boxart = self._file(product, 'launcher_boxart')

        build = self._table(data, 'build')
        self.cmake_args = self._args(build, 'cmake_args')
        self.platform_cmake_args = {'linux': self._args(build, 'linux_cmake_args'),
                                    'windows': self._args(build, 'windows_cmake_args')}
        self.regenerate = self._bool(build, 'regenerate', True)
        self.binary_identity = self._str(build, 'binary_identity', '')
        self.selective_pgo = self._bool(build, 'selective_pgo', True)
        self.pgo_train_seconds = build.get('pgo_train_seconds', 10)
        if (type(self.pgo_train_seconds) is not int or
                not 1 <= self.pgo_train_seconds <= 300):
            fail('%s: pgo_train_seconds must be an integer from 1 to 300' % self.path)

        aot = self._table(data, 'aot')
        self.aot_mode = self._str(aot, 'mode', 'none')
        if self.aot_mode not in AOT_MODES:
            fail('%s: [aot] mode must be one of %s' % (self.path, ', '.join(AOT_MODES)))
        self.aot_profile = self._file(aot, 'profile', required=self.aot_mode != 'none')
        self.aot_cps = self._bool(aot, 'cps', self.aot_mode == 'static')

        self.ship_toolchain = self._bool(self._table(data, 'overlay_toolchain'), 'ship', False)

        diag = self._table(data, 'diagnostics')
        self.diagnostics = None
        if diag:
            self.diagnostics = dict(game_id=self._str(diag, 'game_id', required=True),
                                    package_id=self._str(diag, 'package_id', required=True),
                                    replay_helper=self._bool(diag, 'replay_helper', True))

        payload = self._table(data, 'payload', required=True)
        self.start_here = self._file(payload, 'start_here', required=True)
        self.files = []
        for item in payload.get('files', []):
            if isinstance(item, str):
                item = {'from': item, 'to': item}
            if not isinstance(item, dict) or set(item) - {'from', 'to'} or 'from' not in item:
                fail('%s: [payload] files entries are "path" or {from=, to=}' % self.path)
            source = self.root / item['from']
            if not source.is_file():
                fail('%s: payload file not found: %s' % (self.path, item['from']))
            self.files.append((source, self._relpath(item.get('to', item['from']))))
        self.build_files = [self._relpath(p) for p in payload.get('build_files', [])]

        linux = self._table(data, 'linux')
        self.linux = None
        if linux:
            self.linux = dict(app_run=self._file(linux, 'app_run', required=True),
                              desktop_file=self._file(linux, 'desktop_file', required=True),
                              icon=self._file(linux, 'icon', required=True),
                              payload_dir=self._name(linux, 'payload_dir'),
                              env_prefix=self._str(linux, 'env_prefix', ''),
                              prune_libs=self._bool(linux, 'prune_unreferenced_libs', False))

        hooks = self._table(data, 'hooks')
        self.hooks = {}
        for phase in ('prepare', 'post_build', 'post_stage'):
            entries = hooks.get(phase, [])
            if not isinstance(entries, list):
                fail('%s: [[hooks.%s]] must be an array of tables' % (self.path, phase))
            self.hooks[phase] = []
            for entry in entries:
                if not isinstance(entry, dict) or set(entry) - {'command', 'platforms'}:
                    fail('%s: hooks.%s entries take command = [...] and optional platforms'
                         % (self.path, phase))
                command = entry.get('command')
                if (not isinstance(command, list) or not command or
                        not all(isinstance(c, str) for c in command)):
                    fail('%s: hooks.%s command must be a non-empty list of strings'
                         % (self.path, phase))
                platforms = entry.get('platforms', list(PLATFORMS))
                if not set(platforms) <= set(PLATFORMS):
                    fail('%s: hooks.%s platforms must be within %s' % (self.path, phase, PLATFORMS))
                self.hooks[phase].append((command, platforms))

        for args in [self.cmake_args] + list(self.platform_cmake_args.values()):
            for arg in args:
                match = re.match(r'^-D([A-Za-z0-9_]+)', arg)
                if match and match.group(1) in OWNED_CMAKE:
                    fail('%s: [build] cmake args may not set %s; the packager owns it '
                         '(channel via --channel)' % (self.path, match.group(1)))

    def _check_keys(self, data):
        unknown = set(data) - self.SCHEMA['']
        for table, allowed in self.SCHEMA.items():
            if table and isinstance(data.get(table), dict):
                unknown |= {'%s.%s' % (table, k) for k in set(data[table]) - allowed}
        if unknown:
            fail('%s: unknown key(s): %s' % (self.path, ', '.join(sorted(unknown))))

    def _table(self, data, key, required=False):
        value = data.get(key)
        if value is None:
            if required:
                fail('%s: missing [%s]' % (self.path, key))
            return {}
        if not isinstance(value, dict):
            fail('%s: %s must be a table' % (self.path, key))
        return value

    def _str(self, table, key, default=None, required=False):
        value = table.get(key, default)
        if value is None or (required and value == ''):
            fail('%s: missing %s' % (self.path, key))
        if not isinstance(value, str):
            fail('%s: %s must be a string' % (self.path, key))
        return value

    def _bool(self, table, key, default):
        value = table.get(key, default)
        if not isinstance(value, bool):
            fail('%s: %s must be true or false' % (self.path, key))
        return value

    def _name(self, table, key):
        value = self._str(table, key, required=True)
        if not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*', value):
            fail('%s: %s %r must be a plain file name' % (self.path, key, value))
        return value

    def _args(self, table, key):
        value = table.get(key, [])
        if not isinstance(value, list) or not all(isinstance(a, str) for a in value):
            fail('%s: %s must be a list of strings' % (self.path, key))
        return value

    def _relpath(self, value):
        path = PurePosixPath(value)
        if path.is_absolute() or '..' in path.parts or not path.parts or ':' in value:
            fail('%s: %r must be a relative path inside the package' % (self.path, value))
        return path.as_posix()

    def _file(self, table, key, default=None, required=False):
        value = table.get(key, default)
        if value is None:
            if required:
                fail('%s: missing %s' % (self.path, key))
            return None
        if not isinstance(value, str):
            fail('%s: %s must be a string' % (self.path, key))
        path = self.root / value
        if not path.is_file():
            fail('%s: %s not found: %s' % (self.path, key, value))
        return path

    def version(self):
        raw = read_text(self.version_file).strip()
        if not raw or not re.fullmatch(r'v?[0-9][0-9A-Za-z.+-]*', raw):
            fail('%s: invalid version %r' % (self.version_file, raw))
        return raw


def artifact_name(config, version, channel, plat):
    bare = version[1:] if version.startswith('v') else version
    suffix = '-playtest' if channel == 'playtest' else ''
    return '%s-v%s%s-%s' % (config.name, bare, suffix, ARTIFACT_SUFFIX[plat])


# --------------------------------------------------------------------------
# Provenance
# --------------------------------------------------------------------------
def git_executable():
    explicit = os.environ.get('PSX_RELEASE_GIT')
    if explicit:
        return explicit
    if os.name == 'nt':
        # PowerShell's `git` is often devkitPro MSYS git, which cannot read a
        # worktree whose .git names an F:/ path.
        for candidate in (r'C:\Program Files\Git\cmd\git.exe',):
            if os.path.isfile(candidate):
                return candidate
    found = shutil.which('git')
    if not found:
        fail('git not found (set PSX_RELEASE_GIT)')
    return found


def git(repo, *args):
    return run([git_executable(), '-C', repo] + list(args), 'git %s' % ' '.join(args),
               capture=True).strip()


def provenance(root, ui_root):
    """Commits and cleanliness, from git or (inside WSL) from the Windows parent."""
    env = os.environ
    if env.get('PSX_RELEASE_SOURCE_COMMIT'):
        return dict(source_commit=env['PSX_RELEASE_SOURCE_COMMIT'],
                    source_date_epoch=int(env.get('PSX_RELEASE_SOURCE_DATE_EPOCH') or 0),
                    source_dirty=env.get('PSX_RELEASE_SOURCE_DIRTY') == '1',
                    dirty_paths=json.loads(env.get('PSX_RELEASE_DIRTY_PATHS', '[]')),
                    framework_commit=env['PSX_RELEASE_FRAMEWORK_COMMIT'],
                    recomp_ui_commit=env.get('PSX_RELEASE_UI_COMMIT') or None)
    status = git(root, 'status', '--porcelain', '--untracked-files=no', '--ignore-submodules=none')
    dirty = [line[3:] for line in status.splitlines() if line.strip()]
    fw_status = git(FRAMEWORK, 'status', '--porcelain', '--untracked-files=no')
    dirty += ['psxrecomp/' + line[3:] for line in fw_status.splitlines() if line.strip()]
    ui_commit = None
    if ui_root and (Path(ui_root) / '.git').exists():
        ui_commit = git(ui_root, 'rev-parse', 'HEAD')
    return dict(source_commit=git(root, 'rev-parse', 'HEAD'),
                source_date_epoch=int(git(root, 'log', '-1', '--format=%ct')),
                source_dirty=bool(dirty),
                dirty_paths=dirty, framework_commit=git(FRAMEWORK, 'rev-parse', 'HEAD'),
                recomp_ui_commit=ui_commit)


# --------------------------------------------------------------------------
# Toolchain
# --------------------------------------------------------------------------
class Toolchain:
    def __init__(self, plat, toolchain_bin):
        self.platform = plat
        self.env = dict(os.environ)
        if plat == 'windows':
            self.bin = Path(toolchain_bin).resolve()
            for tool in ('cmake.exe', 'ninja.exe', 'gcc.exe', 'g++.exe', 'objdump.exe'):
                if not (self.bin / tool).is_file():
                    fail('%s missing from %s (pass --toolchain-bin)' % (tool, self.bin))
            self.env['PATH'] = str(self.bin) + os.pathsep + self.env.get('PATH', '')
            self.cmake = str(self.bin / 'cmake.exe')
            self.gcc = str(self.bin / 'gcc.exe')
            self.gxx = str(self.bin / 'g++.exe')
            self.make = str(self.bin / 'ninja.exe')
            self.objdump = str(self.bin / 'objdump.exe')
            self.exe_suffix = '.exe'
        else:
            for tool in ('cmake', 'gcc', 'g++'):
                if not shutil.which(tool):
                    fail('%s not found on PATH' % tool)
            self.cmake, self.gcc, self.gxx = [shutil.which(tool) for tool in ('cmake', 'gcc', 'g++')]
            self.make = shutil.which('ninja') or shutil.which('make')
            if not self.make:
                fail('ninja or make not found on PATH')
            self.objdump = None
            self.exe_suffix = ''
        self.generator = 'Ninja' if (plat == 'windows' or shutil.which('ninja')) else 'Unix Makefiles'


def hook_tokens(ctx, **extra):
    tokens = dict(python=sys.executable, root=str(ctx.root), framework=str(FRAMEWORK),
                  channel=ctx.channel, platform=ctx.platform, version=ctx.version,
                  build_dir=str(ctx.runtime_build), work_dir=str(ctx.work))
    tokens.update(extra)
    return tokens


def run_hooks(ctx, phase, **extra):
    for command, platforms in ctx.config.hooks[phase]:
        if ctx.platform not in platforms:
            continue
        tokens = hook_tokens(ctx, **extra)
        try:
            argv = [part.format(**tokens) for part in command]
        except KeyError as error:
            fail('hooks.%s: unknown token %s' % (phase, error))
        run(argv, 'hooks.%s %s' % (phase, ' '.join(command[:2])), cwd=ctx.root, env=ctx.tc.env)


# --------------------------------------------------------------------------
# Build steps
# --------------------------------------------------------------------------
class Context:
    pass


def build_emitters(ctx):
    build = getattr(ctx, 'recompiler_build', None) or ctx.work / 'recompiler'
    run([ctx.tc.cmake, '-S', FRAMEWORK / 'recompiler', '-B', build, '-G', ctx.tc.generator,
         '-DCMAKE_BUILD_TYPE=Release'] + native_toolchain_args(ctx.tc),
        'emitter configure', env=ctx.tc.env)
    run([ctx.tc.cmake, '--build', build, '--target', 'psxrecomp-game', 'psxrecomp-bios',
         '-j', ctx.jobs], 'emitter build', env=ctx.tc.env)
    for name in ('psxrecomp-game', 'psxrecomp-bios'):
        if not (build / (name + ctx.tc.exe_suffix)).is_file():
            fail('emitter build did not produce %s' % (build / (name + ctx.tc.exe_suffix)))
    ctx.recompiler_dir = build
    ctx.recompiler = build / ('psxrecomp-game' + ctx.tc.exe_suffix)


def ensure_openbios(ctx):
    if (FRAMEWORK / 'generated/OpenBIOS_dispatch.c').is_file():
        return
    log('Recompiled OpenBIOS backend missing; generating it')
    if not getattr(ctx, 'recompiler_dir', None):
        build_emitters(ctx)
    rel_build = os.path.relpath(ctx.recompiler_dir, FRAMEWORK).replace(os.sep, '/')
    if ctx.platform == 'windows':
        bash = Path(r'C:\msys64\usr\bin\bash.exe')
        if not bash.is_file():
            fail('OpenBIOS C is missing and MSYS2 bash is not installed to regenerate it')
        posix = lambda p: run([str(bash.parent / 'cygpath.exe'), '-u', str(p)], 'cygpath',
                              capture=True).strip()
        script = ("export PATH='%s':/usr/bin:$PATH; cd '%s' && PSXRECOMP_BIOS_BUILD='%s' "
                  "tools/regen_bios.sh --config bios/OpenBIOS.toml"
                  % (posix(ctx.tc.bin), posix(FRAMEWORK), rel_build))
        run([bash, '-c', script], 'regen_bios OpenBIOS', env=ctx.tc.env)
    else:
        env = dict(ctx.tc.env, PSXRECOMP_BIOS_BUILD=rel_build)
        run(['bash', 'tools/regen_bios.sh', '--config', 'bios/OpenBIOS.toml'],
            'regen_bios OpenBIOS', cwd=FRAMEWORK, env=env)
    # regen_bios.sh has exited 0 on failure before; check the product.
    if not (FRAMEWORK / 'generated/OpenBIOS_dispatch.c').is_file():
        fail('regen_bios produced no generated/OpenBIOS_dispatch.c')


def prepare_sources(ctx):
    """Platform-independent source generation: game C and static AOT units."""
    config = ctx.config
    if config.regenerate:
        run([ctx.recompiler, '--config', config.game_toml], 'game C regeneration',
            cwd=ctx.root, env=ctx.tc.env)
    if config.aot_mode == 'static':
        run([sys.executable, FRAMEWORK / 'tools/aot_overlay_pipeline.py', 'static',
             '--profile', config.aot_profile, '--game-toml', config.game_toml,
             '--recompiler', ctx.recompiler, '--work-dir', ctx.work / 'aot-static',
             '--gcc', ctx.tc.gcc, '--cmake', ctx.tc.cmake, '--workers', ctx.jobs, '--reuse']
            + (['--cps'] if config.aot_cps else []),
            'static AOT generation', cwd=ctx.root, env=ctx.tc.env)
    run_hooks(ctx, 'prepare')


def cmake_cache(build):
    path = Path(build) / 'CMakeCache.txt'
    if not path.is_file():
        fail('no CMakeCache.txt in %s' % build)
    values = {}
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        match = re.match(r'^([A-Za-z0-9_.+-]+):[A-Z_]+=(.*)$', line)
        if match:
            values[match.group(1)] = match.group(2)
    return values


def cmake_bool(value):
    return str(value).upper() in ('ON', '1', 'TRUE', 'YES', 'Y')


def configured_cmake_value(build, filename, key):
    """Read the current CMake detection file, ignoring any older cache versions."""
    cache = cmake_cache(build)
    version = '.'.join(cache.get('CMAKE_CACHE_%s_VERSION' % part, '')
                       for part in ('MAJOR', 'MINOR', 'PATCH'))
    path = Path(build) / 'CMakeFiles' / version / filename
    if not path.is_file():
        fail('CMake compiler identity is missing: %s' % path)
    match = re.search(r'set\(%s "([^"]+)"\)' % re.escape(key), read_text(path))
    if not match:
        fail('CMake compiler identity is unreadable: %s' % path)
    return match.group(1)


def native_toolchain_args(toolchain):
    return ['-DCMAKE_C_COMPILER=' + toolchain.gcc,
            '-DCMAKE_CXX_COMPILER=' + toolchain.gxx,
            '-DCMAKE_MAKE_PROGRAM=' + toolchain.make]


def close_training_game(process, timeout=15):
    """Request the exact owned SDL process to quit normally so gcov flushes."""
    if process.poll() is not None:
        fail('PGO training exited before its close request (%s)' % process.returncode)
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes as w
        user = ctypes.WinDLL('user32', use_last_error=True)
        callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        user.EnumWindows.argtypes = [callback_type, w.LPARAM]
        user.EnumWindows.restype = w.BOOL
        user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
        user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
        user.PostMessageW.restype = w.BOOL
        windows = []

        @callback_type
        def inspect(handle, unused):
            owner = w.DWORD()
            user.GetWindowThreadProcessId(handle, ctypes.byref(owner))
            if owner.value == process.pid:
                name = ctypes.create_unicode_buffer(256)
                user.GetClassNameW(handle, name, len(name))
                if name.value.startswith('SDL'):
                    windows.append(handle)
            return True

        if not user.EnumWindows(inspect, 0) or len(windows) != 1:
            fail('PGO training needs one exact owned SDL window; found %d' % len(windows))
        if not user.PostMessageW(windows[0], 0x0010, 0, 0):
            fail('Cannot request normal PGO training window close')
        method = 'WM_CLOSE to exact owned SDL window, no focus'
    else:
        # SDL's enabled POSIX signal handler converts SIGTERM to SDL_EVENT_QUIT.
        # A signal termination/nonzero exit is a failure, never a flushed-profile pass.
        process.send_signal(signal.SIGTERM)
        method = 'SIGTERM converted by SDL signal handler to quit'
    code = process.wait(timeout=timeout)
    if code != 0:
        fail('PGO training did not close normally (exit %s)' % code)
    return dict(method=method, pid=process.pid, exit_code=code)


def train_release_pgo(ctx, generated):
    """One isolated real boot for build profiles, not a gameplay QA/gain claim."""
    directory = ctx.runtime_build / 'runtime-hot-pgo' / ctx.config.runtime_target
    receipt = directory / 'training.json'
    private = ctx.work / 'pgo-training'
    if receipt.exists() or private.exists():
        fail('Preserve existing PGO training; choose a fresh --work-dir')
    private.mkdir()
    for name in ('state', 'resident-cache', 'temp', 'userdata'):
        (private / name).mkdir()
    exe = locate_executable(ctx)
    bios = exe.parent / 'bios/openbios.bin'
    if not bios.is_file() or bios.stat().st_size != OPENBIOS_SIZE:
        fail('PGO boot training needs the build-staged OpenBIOS')
    env = {key: value for key, value in ctx.tc.env.items()
           if not key.startswith(('PSX_', 'SDL_', 'GCOV_', 'LLVM_PROFILE_'))}
    env.update(SDL_AUDIODRIVER='dummy', SDL_NO_SIGNAL_HANDLERS='0',
               PSX_RESIDENT_CACHE=str(private / 'resident-cache'),
               PSX_OVERLAY_CACHE_DIR=str(exe.parent / 'cache'),
               TEMP=str(private / 'temp'), TMP=str(private / 'temp'),
               TMPDIR=str(private / 'temp'), APPDATA=str(private / 'userdata'),
               LOCALAPPDATA=str(private / 'userdata'),
               XDG_DATA_HOME=str(private / 'userdata'),
               XDG_CONFIG_HOME=str(private / 'userdata'))
    argv = [str(exe), '--game', str(ctx.config.game_toml), '--bios', str(bios),
            '--memcard-dir', str(private / 'state'), '--no-launcher', '--hidden-window']
    record = dict(status='training', purpose='Build-stage boot profiles only; no gameplay QA or performance qualification',
                  binary_sha256=sha256(exe), config_sha256=sha256(ctx.config.game_toml),
                  generate_receipt_sha256=sha256(directory / 'generate.json'),
                  seconds=ctx.config.pgo_train_seconds, argv=argv, private=str(private))
    log('Build-stage PGO boot profiles: %ds, hidden and muted; no gameplay QA claim'
        % ctx.config.pgo_train_seconds)
    process = None
    try:
        with (private / 'stdout.log').open('wb') as out, (private / 'stderr.log').open('wb') as err:
            process = subprocess.Popen(argv, cwd=exe.parent, env=env, stdin=subprocess.DEVNULL,
                                       stdout=out, stderr=err)
            started = time.monotonic()
            try:
                code = process.wait(timeout=ctx.config.pgo_train_seconds)
            except subprocess.TimeoutExpired:
                pass
            else:
                fail('PGO boot exited early (exit %s); no training qualification' % code)
            record['close'] = close_training_game(process)
            record['elapsed_seconds'] = time.monotonic() - started
        runtime_hot_pgo.verify_inputs(generated)
        record['profiles'] = runtime_hot_pgo.profiles(
            ctx.runtime_build, ctx.config.runtime_target, generated['commands'])
        if sha256(exe) != record['binary_sha256'] or sha256(ctx.config.game_toml) != record['config_sha256']:
            fail('PGO training binary/configuration changed')
        record['status'] = 'trained-boot-only'
    except BaseException as error:
        record.update(status='failed', failure=str(error))
        raise
    finally:
        try:
            if process is not None and process.poll() is None:
                # Cleanup is never credited as a successful profile flush.
                try:
                    process.terminate()
                    process.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    process.kill()
                    process.wait(timeout=5)
                record['cleanup_exit_code'] = process.returncode
        except BaseException as cleanup_error:
            record['cleanup_error'] = str(cleanup_error)
        finally:
            receipt.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8', newline='\n')
    return record


def build_selective_pgo(ctx, args, cache):
    """Delegate strict three-TU phases to the qualified shared workflow."""
    marker = ctx.runtime_build / ('psxrecomp_exe_name-%s.txt' % ctx.config.runtime_target)
    basename = read_text(marker).strip() if marker.is_file() else ctx.config.runtime_target
    binary = basename + ctx.tc.exe_suffix
    controlled = runtime_hot_pgo.CONTROLLED
    configure_args = [arg for arg in args if not any(
        re.match(r'-[DU]' + re.escape(key) + r'(?::|=|$)', arg) for key in controlled)]
    for key in ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER', 'CMAKE_MAKE_PROGRAM'):
        configure_args.append('-D%s=%s' % (key, cache[key]))
    recipe = dict(source=str(ctx.root), build=str(ctx.runtime_build), framework=str(FRAMEWORK),
                  cmake=cache['CMAKE_COMMAND'], target=ctx.config.runtime_target, binary=binary,
                  companions=[basename + '.execution.json'],
                  configure_args=['-G', cache['CMAKE_GENERATOR']] + configure_args)
    recipe_path = ctx.work / 'runtime-pgo-recipe.json'
    if recipe_path.exists():
        fail('Preserve existing PGO recipe; choose a fresh --work-dir')
    recipe_path.write_text(json.dumps(recipe, indent=2) + '\n', encoding='utf-8', newline='\n')
    directory = ctx.runtime_build / 'runtime-hot-pgo' / ctx.config.runtime_target
    for phase in ('baseline', 'generate'):
        run([sys.executable, TOOLS / 'runtime_hot_pgo.py', phase, '--config', recipe_path,
             '--jobs', min(2, ctx.jobs)], 'selective PGO ' + phase, cwd=ctx.root, env=ctx.tc.env)
    generated = json.loads(read_text(directory / 'generate.json'))
    training = train_release_pgo(ctx, generated)
    run([sys.executable, TOOLS / 'runtime_hot_pgo.py', 'use', '--config', recipe_path,
         '--jobs', min(2, ctx.jobs)], 'strict selective PGO use', cwd=ctx.root, env=ctx.tc.env)
    used = json.loads(read_text(directory / 'use.json'))
    if used['status'] != 'built' or used['profiles'] != training['profiles']:
        fail('Strict PGO use did not retain the actual boot training profiles')
    ctx.runtime_pgo = dict(status='selective-use', training_scope='boot-only; not gameplay QA or a per-title gain claim',
                           selected_sources=used['selected_sources'], profiles=used['profiles'],
                           receipts={phase: sha256(directory / (phase + '.json'))
                                     for phase in ('baseline', 'generate', 'training', 'use')})
    # The player manifest needs identities, not private build paths or gcda blobs.
    ctx.runtime_pgo['profiles'] = {name: {key: row[key] for key in ('sha256', 'bytes')}
                                   for name, row in used['profiles'].items()}


def configure_and_build(ctx):
    owned = {
        'CMAKE_BUILD_TYPE': 'Release',
        'PSX_DEBUG_TOOLS': 'OFF',
        'PSX_RELEASE_CHANNEL': ctx.channel,
        'PSXRECOMP_BIOS_STEMS': 'OpenBIOS',
        'PSX_GAME_VERSION': ctx.version,
        'PSX_SOURCE_COMMIT': ctx.prov['source_commit'],
        # The framework running this script is the framework that gets built.
        'PSXRECOMP_ROOT': FRAMEWORK.as_posix(),
        'PSX_RUNTIME_IPO': 'OFF', 'PSX_PGO': '',
        'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON', 'CMAKE_PROJECT_INCLUDE': '',
    }
    args = ['-D%s=%s' % kv for kv in owned.items()] + native_toolchain_args(ctx.tc)
    args += ctx.config.cmake_args + ctx.config.platform_cmake_args[ctx.platform]
    if ctx.platform == 'linux':
        # The interpreter running this script; WSL otherwise finds the Windows
        # pyenv shim first (beads-eio.3.299). --build-id=none keeps the ELF a
        # function of its sources.
        args += ['-DPSX_PYTHON=%s' % sys.executable,
                 '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--build-id=none',
                 '-DCMAKE_C_COMPILER_LAUNCHER=', '-DCMAKE_CXX_COMPILER_LAUNCHER=']
    run([ctx.tc.cmake, '-S', ctx.root, '-B', ctx.runtime_build, '-G', ctx.tc.generator] + args,
        'runtime configure', env=ctx.tc.env)
    cache = cmake_cache(ctx.runtime_build)
    expected = dict(owned, PSX_PUBLIC_RELEASE='ON' if ctx.channel == 'public' else 'OFF')
    for arg in ctx.config.cmake_args + ctx.config.platform_cmake_args[ctx.platform]:
        match = re.match(r'^-D([A-Za-z0-9_]+)(?::[A-Z]+)?=(.*)$', arg)
        if match:
            expected.setdefault(match.group(1), match.group(2))
    for key, want in expected.items():
        got = cache.get(key)
        if key == 'PSXRECOMP_ROOT':
            same = got is not None and Path(got).resolve() == FRAMEWORK
        else:
            same = (got == want or
                    (want.upper() in ('ON', 'OFF') and got is not None and
                     cmake_bool(got) == cmake_bool(want)))
        if not same:
            fail('configured %s=%r but CMakeCache.txt has %r (the title CMake overrode it?)'
                 % (key, want, got))
    if cache.get('PSX_RECOMP_UI', 'ON').upper() == 'OFF':
        fail('PSX_RECOMP_UI=OFF: a player release needs the recomp-ui launcher')
    if not ctx.config.selective_pgo:
        ctx.runtime_pgo = dict(status='disabled-explicit', requested=False)
    elif (cache.get('CMAKE_GENERATOR') == 'Ninja' and
          configured_cmake_value(ctx.runtime_build, 'CMakeCCompiler.cmake', 'CMAKE_C_COMPILER_ID') == 'GNU' and
          configured_cmake_value(ctx.runtime_build, 'CMakeCXXCompiler.cmake', 'CMAKE_CXX_COMPILER_ID') == 'GNU' and
          not cmake_bool(configured_cmake_value(ctx.runtime_build, 'CMakeSystem.cmake', 'CMAKE_CROSSCOMPILING'))):
        build_selective_pgo(ctx, args, cache)
    else:
        ctx.runtime_pgo = dict(status='portable-unprofiled', requested=True,
                               reason='Selective PGO requires native GNU C/C++ and Ninja')
        log('Selective PGO unavailable for this toolchain; building the portable unprofiled runtime')
    if ctx.runtime_pgo['status'] != 'selective-use':
        run([ctx.tc.cmake, '--build', ctx.runtime_build, '--target', ctx.config.runtime_target,
             '-j', ctx.jobs], 'runtime build', env=ctx.tc.env)
    ctx.cache = cmake_cache(ctx.runtime_build)


def locate_executable(ctx):
    marker = ctx.runtime_build / ('psxrecomp_exe_name-%s.txt' % ctx.config.runtime_target)
    candidates = []
    if marker.is_file():
        candidates.append(read_text(marker).strip())
    candidates.append(ctx.config.runtime_target)
    for name in candidates:
        for directory in (ctx.runtime_build, ctx.runtime_build / 'Release'):
            path = directory / (name + ctx.tc.exe_suffix)
            if path.is_file():
                return path
    fail('built executable not found under %s (marker %s)' % (ctx.runtime_build, marker))


def check_build_outputs(ctx):
    exe = locate_executable(ctx)
    ctx.built_exe = exe
    ctx.exe_dir = exe.parent
    stamp = ctx.exe_dir / 'psx_game_version.txt'
    if not stamp.is_file():
        fail('missing %s (rebuild with a current runtime.cmake)' % stamp)
    built = read_text(stamp).strip()
    if built != ctx.version:
        fail('binary version stamp %r != %s %r' % (built, ctx.config.version_file.name, ctx.version))
    if ctx.config.binary_identity:
        token = ctx.config.binary_identity.format(
            version=ctx.version, commit=ctx.prov['source_commit'],
            commit12=ctx.prov['source_commit'][:12])
        data = exe.read_bytes()
        if token.encode('utf-8') not in data:
            fail('executable lacks build identity %r: stale binary or wrong source commit' % token)
        log('build identity present: %s' % token)
    if ctx.config.aot_mode == 'static':
        ctx.static_audit = check_static_audit(ctx)
    run_hooks(ctx, 'post_build', exe=str(exe))


def check_static_audit(ctx):
    """The linked static AOT units are exactly the audited original-disc output."""
    config = ctx.config
    profile = json.loads(read_text(config.aot_profile))
    output = profile.get('static_output')
    if not output:
        fail('%s: AOT mode "static" needs a profile static_output' % config.aot_profile)
    out_dir = (config.game_toml.parent / output).parent
    receipt_path = out_dir / 'AOT_STATIC_AUDIT.json'
    if not receipt_path.is_file():
        fail('missing %s; the static AOT step did not publish' % receipt_path)
    audit = json.loads(read_text(receipt_path))
    if not audit.get('all_pairs_valid') or not audit.get('all_guards_match_known_input_bytes'):
        fail('original-disc static AOT audit failed (%s)' % receipt_path)
    expected = profile.get('expected_records')
    if expected is not None and audit.get('recipe_count') != expected:
        fail('static AOT recipe_count %s != profile expected_records %s'
             % (audit.get('recipe_count'), expected))
    inputs = audit.get('generation_inputs', {})
    if inputs.get('game_toml_sha256') != sha256(config.game_toml):
        fail('%s changed since static AOT generation' % config.game_toml.name)
    if inputs.get('profile_sha256') != sha256(config.aot_profile):
        fail('AOT profile changed since static AOT generation')
    files = audit.get('files') or {}
    if not files:
        fail('static AOT receipt lists no files')
    for name, digest in files.items():
        if sha256(out_dir / name) != digest:
            fail('static AOT unit %s differs from its audit receipt' % name)
    ninja = ctx.runtime_build / 'build.ninja'
    if ninja.is_file():
        exe = ctx.built_exe.name
        links = [line for line in read_text(ninja).splitlines()
                 if line.startswith('build ') and 'EXECUTABLE_LINKER' in line and exe in line.split(':')[0]]
        if not links:
            fail('no link rule for %s in build.ninja' % exe)
        for name in files:
            if name.endswith('.c') and not any(name in line for line in links):
                fail('static AOT unit %s is not linked into %s' % (name, exe))
    log('static AOT receipt verified: %d recipes, %d files' % (audit.get('recipe_count', 0), len(files)))
    return dict(recipe_count=audit.get('recipe_count'),
                published_variants=audit.get('published_variants'),
                full_static_coverage_proven=audit.get('full_static_coverage_proven'),
                profile_sha256=audit.get('profile_sha256'),
                generation_inputs_sha256=audit.get('generation_inputs_sha256'))


# --------------------------------------------------------------------------
# Staging
# --------------------------------------------------------------------------
def copy(src, dst):
    dst = Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def stage_payload(ctx):
    config, stage, exe_dir = ctx.config, ctx.stage, ctx.exe_dir
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    exe_name = (config.exe_name or ctx.built_exe.stem) + ctx.tc.exe_suffix
    ctx.exe_name = exe_name
    copy(ctx.built_exe, stage / exe_name)
    if ctx.platform == 'linux':
        os.chmod(stage / exe_name, 0o755)
    copy(ctx.exe_dir / 'psx_game_version.txt', stage / 'psx_game_version.txt')

    for tree in ('assets/fonts', 'assets/img'):
        if not (exe_dir / tree).is_dir():
            fail('%s missing: the recomp-ui launcher assets were not staged by the build'
                 % (exe_dir / tree))
    shutil.copytree(exe_dir / 'assets', stage / 'assets')
    if config.launcher_boxart is not None:
        staged = stage / 'assets/img/boxart.tga'
        if not staged.is_file() or sha256(staged) != sha256(config.launcher_boxart):
            fail('staged launcher boxart does not match %s' % config.launcher_boxart)

    bios, notice = exe_dir / 'bios/openbios.bin', exe_dir / 'bios/OpenBIOS.LICENSE'
    if not bios.is_file() or bios.stat().st_size != OPENBIOS_SIZE or not notice.is_file():
        fail('the build did not stage OpenBIOS (%d bytes) and its MIT notice' % OPENBIOS_SIZE)
    copy(bios, stage / 'bios/openbios.bin')
    copy(notice, stage / 'bios/OpenBIOS.LICENSE')

    copy(config.player_game_toml, stage / 'game.toml')
    options = config.root / 'game_options.toml'
    if options.is_file():
        copy(options, stage / 'game_options.toml')
    game = tomllib.loads(read_text(stage / 'game.toml'))
    ctx.game_id = game.get('game', {}).get('id')
    if not ctx.game_id:
        fail('[game] id missing from %s' % config.player_game_toml)

    # Mod catalog: the build's own catalog list is the invariant.
    run([sys.executable, TOOLS / 'release_stage.py', 'stage-mods', '--build-path', exe_dir,
         '--stage', stage, '--runtime-target', config.runtime_target], 'mod catalog staging')
    if ctx.channel == 'public':
        run([sys.executable, TOOLS / 'mod_channel_filter.py', stage / 'mods/bundled'],
            'developer-channel pruning')
    ctx.mod_packages = staged_mod_ids(stage)
    if not ctx.mod_packages:
        fail('staged mod catalog is empty')
    check_diagnostics(ctx, stage)

    for rel in config.build_files:
        if not (exe_dir / rel).is_file():
            fail('build file %s missing beside the executable' % rel)
        copy(exe_dir / rel, stage / rel)
    if config.diagnostics and config.diagnostics['replay_helper']:
        helper = exe_dir / 'replay-diagnostics.py'
        if not helper.is_file():
            fail('diagnostics replay helper missing beside the executable (%s)' % helper)
        copy(helper, stage / 'replay-diagnostics.py')

    if config.aot_mode == 'cache':
        run([sys.executable, TOOLS / 'aot_overlay_pipeline.py', 'release',
             '--profile', config.aot_profile, '--game-toml', config.game_toml,
             '--runtime-config', stage / 'game.toml', '--runtime-build-dir', ctx.runtime_build,
             '--runtime-target', config.runtime_target, '--recompiler', ctx.recompiler,
             '--work-dir', ctx.work / 'aot-cache', '--stage', stage, '--gcc', ctx.tc.gcc,
             '--workers', ctx.jobs] + (['--cps'] if config.aot_cps else []),
            'original-disc AOT cache', cwd=ctx.root, env=ctx.tc.env)
        if not (stage / 'AOT_CACHE_AUDIT.json').is_file():
            fail('AOT release staged no AOT_CACHE_AUDIT.json')
    if config.ship_toolchain:
        command = [sys.executable, TOOLS / 'release_stage.py', 'stage-toolchain', '--stage', stage,
                   '--recomp-dir', ctx.recompiler_dir, '--recomp-tools', TOOLS,
                   '--recomp-include', FRAMEWORK / 'runtime/include',
                   '--dl-cache', ctx.dl_cache, '--platform', TOOLCHAIN_PLATFORM[ctx.platform]]
        if ctx.platform == 'windows':
            command += ['--mingw-bin', ctx.tc.bin]
        run(command, 'overlay toolchain staging', env=ctx.tc.env)

    licenses = stage / 'licenses'
    licenses.mkdir(exist_ok=True)
    for notice_file in sorted((FRAMEWORK / 'runtime/licenses').iterdir()):
        if notice_file.is_file():
            copy(notice_file, licenses / notice_file.name)
    copy(FRAMEWORK / 'LICENSE', licenses / 'psxrecomp-LICENSE')
    if (FRAMEWORK / 'THIRD_PARTY_ATTRIBUTION.md').is_file():
        copy(FRAMEWORK / 'THIRD_PARTY_ATTRIBUTION.md', licenses / 'psxrecomp-THIRD_PARTY_ATTRIBUTION.md')
    ui_root = Path(ctx.cache.get('RECOMP_UI_ROOT') or (config.root / 'recomp-ui'))
    if not (ui_root / 'LICENSE').is_file():
        fail('recomp-ui LICENSE not found under %s' % ui_root)
    copy(ui_root / 'LICENSE', licenses / 'recomp-ui-LICENSE')
    for sub in ('fonts', 'img'):
        notice_md = ui_root / 'assets/common' / sub / 'NOTICE.md'
        if notice_md.is_file():
            copy(notice_md, stage / 'assets' / sub / 'NOTICE.md')

    for source, rel in config.files:
        copy(source, stage / rel)
    start_here = read_text(config.start_here)
    if '@VERSION@' not in start_here:
        fail('%s has no @VERSION@ token; the shipped header must be stamped, not hand-edited'
             % config.start_here)
    start_here = start_here.replace('@VERSION@', ctx.version)
    (stage / 'START_HERE.txt').write_text(start_here, encoding='utf-8', newline='\n')
    run_hooks(ctx, 'post_stage', stage=str(stage))


def staged_mod_ids(stage):
    bundled = Path(stage) / 'mods/bundled'
    ids = []
    for manifest in sorted(bundled.glob('*/*/manifest.toml')):
        ids.append(tomllib.loads(read_text(manifest)).get('id', manifest.parent.parent.name))
    return sorted(set(ids))


def diagnostics_manifests(mods_root):
    found = {}
    for manifest in sorted(Path(mods_root).glob('*/*/manifest.toml')):
        data = tomllib.loads(read_text(manifest))
        features = {f.get('id') for f in data.get('feature', [])}
        if {'recording', 'osd'} <= features and str(data.get('id', '')).endswith('.diagnostics'):
            found.setdefault(data['id'], []).append((manifest, data))
    return found


def check_diagnostics(ctx, stage):
    """Diagnostics defaults follow the channel; a diagnostics package the
    config does not declare is a config error, not something to skip."""
    found = diagnostics_manifests(Path(stage) / 'mods/bundled')
    declared = ctx.config.diagnostics
    if not declared:
        if found:
            fail('staged catalog carries %s but the release config has no [diagnostics] table'
                 % ', '.join(sorted(found)))
        return
    entries = found.get(declared['package_id'])
    if not entries:
        fail('staged catalog is missing %s' % declared['package_id'])
    want = ctx.channel == 'playtest'
    for manifest, data in entries:
        if data.get('channel') == 'developer':
            fail('%s must stay player-selectable' % manifest)
        features = {f.get('id'): f for f in data.get('feature', [])}
        for feature in ('recording', 'osd'):
            got = features[feature].get('default_enabled')
            if got is not want or features[feature].get('channel') == 'developer':
                fail('%s: %s default_enabled=%r, a %s package needs %r'
                     % (manifest, feature, got, ctx.channel, want))
    log('Playtest Diagnostics default %s (%s)' % ('on' if want else 'off', ctx.channel))


def declared_overlay_assets(stage):
    """Hash-bound mod patches are payloads, not standalone disc/BIOS images."""
    allowed = set()
    for manifest in sorted((Path(stage) / 'mods/bundled').glob('*/*/manifest.toml')):
        for overlay in tomllib.loads(read_text(manifest)).get('overlay', []):
            rel = PurePosixPath(overlay.get('file', ''))
            if (rel.is_absolute() or '..' in rel.parts or not rel.parts or
                    rel.parts[0] != 'assets' or rel.suffix.lower() != '.bin' or
                    overlay.get('target') != 'disc_user'):
                continue
            digest = overlay.get('sha256', '')
            expected = overlay.get('expected_sha256', '')
            if not all(re.fullmatch(r'[0-9a-fA-F]{64}', h) for h in (digest, expected)):
                continue
            asset = manifest.parent.joinpath(*rel.parts).resolve()
            if not asset.is_relative_to(manifest.parent.resolve()) or not asset.is_file():
                fail('declared overlay asset missing or outside package: %s' % rel)
            if sha256(asset).lower() != digest.lower():
                fail('declared overlay asset hash mismatch: %s' % rel)
            allowed.add(asset)
    return allowed


def payload_gates(stage, plat, exe_name):
    """Content that must never ship, whatever the title config says."""
    stage = Path(stage)
    overlay_assets = declared_overlay_assets(stage)
    present = [name for name in BUILD_KIT if (stage / name).exists()]
    if present:
        fail('package contains build-kit/user content: %s' % ', '.join(present))
    for path in sorted(stage.rglob('*')):
        if path.is_symlink():
            target = (path.parent / os.readlink(path)).resolve()
            if stage.resolve() not in target.parents:
                fail('symlink leaving the package: %s' % path.relative_to(stage))
            continue
        if not path.is_file():
            continue
        rel = path.relative_to(stage).as_posix()
        lower, leaf = rel.lower(), path.name.lower()
        suffix = path.suffix.lower()
        if (suffix in ('.gcda', '.gcno', '.profraw', '.profdata') or
                (leaf.startswith('libgcov') and not lower.startswith('overlay_toolchain/'))):
            fail('compiler profile/training data in package: %s' % rel)
        if suffix in DISC_SUFFIXES:
            fail('disc/save data in package: %s' % rel)
        if (suffix in ('.bin', '.rom') and lower != 'bios/openbios.bin' and
                path.resolve() not in overlay_assets):
            fail('binary image in package (only bios/openbios.bin may ship): %s' % rel)
        if re.match(r'scph.*\.(bin|rom)', leaf):
            fail('retail BIOS image in package: %s' % rel)
        if leaf in PERSONAL_NAMES or lower.startswith(('mods/installed/', 'saves/', 'diagnostics/')):
            fail('personal/user state in package: %s' % rel)
        if suffix in ('.c', '.cpp', '.cc') and not lower.startswith('overlay_toolchain/'):
            fail('source code in package: %s' % rel)
        if plat == 'linux' and suffix in ('.dll', '.exe'):
            fail('Windows binary in a Linux package: %s' % rel)
    bios = stage / 'bios/openbios.bin'
    if not bios.is_file() or bios.stat().st_size != OPENBIOS_SIZE:
        fail('package lacks the bundled OpenBIOS')
    if not (stage / exe_name).is_file():
        fail('package lacks its executable %s' % exe_name)


def windows_imports(exe, objdump):
    text = run([objdump, '-p', exe], 'objdump imports', capture=True)
    imports = re.findall(r'DLL Name:\s*(\S+)', text)
    if not imports:
        fail('objdump found no imports in %s' % exe)
    foreign = [d for d in imports if d.lower().removesuffix('.dll') not in SYSTEM_DLLS
               and not re.match(r'api-ms-win-[a-z0-9-]+\.dll$', d.lower())]
    if foreign:
        fail('%s is not self-contained: imports %s' % (Path(exe).name, ', '.join(foreign)))
    return imports


def sign_windows(ctx):
    if not (os.environ.get('WINDOWS_SIGN_PFX') or os.environ.get('WINDOWS_SIGN_PFX_BASE64')):
        log('Windows binaries ship unsigned (no WINDOWS_SIGN_PFX certificate configured)')
        ctx.signed = False
        return
    bash = shutil.which('bash')
    if not bash:
        fail('a signing certificate is configured but bash is not available to run sign_windows.sh')
    run([bash, TOOLS / 'ci/sign_windows.sh', ctx.stage], 'Authenticode signing')
    ctx.signed = True


def bind_execution(ctx, binary, output):
    sidecar = ctx.built_exe.with_name(ctx.built_exe.stem + '.execution.json')
    if not sidecar.is_file():
        fail('build published no execution contract %s' % sidecar)
    run([sys.executable, TOOLS / 'release_stage.py', 'stage-execution', '--binary', binary,
         '--manifest', sidecar, '--output', output], 'execution contract binding')


def write_manifest(ctx, extra):
    stage = ctx.stage
    manifest = dict(
        schema=1, product=ctx.config.name, display_name=ctx.config.display_name,
        version=ctx.version, channel=ctx.channel, public_release=ctx.channel == 'public',
        platform=ctx.platform, game_id=ctx.game_id, executable=ctx.exe_name,
        source_commit=ctx.prov['source_commit'], source_dirty=ctx.prov['source_dirty'],
        dirty_paths=ctx.prov['dirty_paths'], framework_commit=ctx.prov['framework_commit'],
        recomp_ui_commit=ctx.prov['recomp_ui_commit'], debug_tools=False,
        bios_backends=['OpenBIOS'], setup_wizard=cmake_bool(ctx.cache.get('PSX_SETUP_WIZARD', 'OFF')),
        playtest_diagnostics_default_enabled=(ctx.channel == 'playtest') if ctx.config.diagnostics else None,
        mod_packages=ctx.mod_packages, aot_mode=ctx.config.aot_mode,
        overlay_toolchain=ctx.config.ship_toolchain, signed=getattr(ctx, 'signed', False),
        packager='psxrecomp/tools/package_title_release.py')
    if hasattr(ctx, 'runtime_pgo'):
        manifest['runtime_pgo'] = ctx.runtime_pgo
    if getattr(ctx, 'static_audit', None):
        manifest['static_aot'] = ctx.static_audit
    manifest.update(extra)
    files = {}
    for path in sorted(stage.rglob('*')):
        if path.is_file() and path.name != 'RELEASE_MANIFEST.json':
            files[path.relative_to(stage).as_posix()] = sha256(path)
    manifest['files'] = files
    (stage / 'RELEASE_MANIFEST.json').write_text(json.dumps(manifest, indent=2) + '\n',
                                                 encoding='utf-8', newline='\n')
    return manifest


def write_sha256(artifact):
    sidecar = Path(str(artifact) + '.sha256')
    sidecar.write_text('%s  %s\n' % (sha256(artifact), Path(artifact).name), encoding='ascii',
                       newline='\n')
    return sidecar


def diagnostics_audit(ctx, target):
    diag = ctx.config.diagnostics
    if not diag:
        return
    command = [sys.executable, TOOLS / 'ci/audit_playtest_diagnostics_package.py', target,
               '--game-id', diag['game_id'], '--diagnostics-id', diag['package_id'],
               '--mode', ctx.channel]
    if not diag['replay_helper']:
        command.append('--no-replay-helper')
    run(command, 'Playtest Diagnostics package audit (%s)' % ctx.channel)


def package_windows(ctx):
    payload_gates(ctx.stage, 'windows', ctx.exe_name)
    imports = windows_imports(ctx.stage / ctx.exe_name, ctx.tc.objdump)
    log('Verified self-contained: %d imports, all system DLLs' % len(imports))
    sign_windows(ctx)
    bind_execution(ctx, ctx.stage / ctx.exe_name,
                   ctx.stage / (Path(ctx.exe_name).stem + '.execution.json'))
    write_manifest(ctx, dict(system_dll_imports=imports))
    artifact = ctx.out_dir / artifact_name(ctx.config, ctx.version, ctx.channel, 'windows')
    for stale in (artifact, Path(str(artifact) + '.sha256')):
        if stale.exists():
            stale.unlink()
    zip_source = ctx.stage
    if ctx.config.zip_folder:
        zip_source = ctx.work / 'zip-root'
        if zip_source.exists():
            shutil.rmtree(zip_source)
        zip_source.mkdir(parents=True)
        shutil.copytree(ctx.stage, zip_source / ctx.config.zip_folder)
    run([sys.executable, TOOLS / 'create_release_zip.py', '--source', zip_source,
         '--output', artifact], 'ZIP creation')
    diagnostics_audit(ctx, artifact)
    write_sha256(artifact)
    return artifact


def render_template(path, tokens):
    text = read_text(path)
    for key, value in tokens.items():
        text = text.replace('@%s@' % key, value)
    left = sorted(set(re.findall(r'@[A-Z][A-Z0-9_]*@', text)))
    if left:
        fail('%s has unstamped token(s) %s' % (path, ', '.join(left)))
    return text


def package_linux(ctx):
    linux = ctx.config.linux
    if not linux:
        fail('%s has no [linux] table (AppRun, desktop file, icon, payload_dir)' % ctx.config.path)
    payload_gates(ctx.stage, 'linux', ctx.exe_name)
    diagnostics_audit(ctx, ctx.stage)
    bind_execution(ctx, ctx.stage / ctx.exe_name, ctx.stage / (ctx.exe_name + '.execution.json'))
    libc = run(['getconf', 'GNU_LIBC_VERSION'], 'glibc version', capture=True).strip()
    write_manifest(ctx, dict(linux_build_libc=libc))
    meta = ctx.work / 'appimage-meta'
    if meta.exists():
        shutil.rmtree(meta)
    meta.mkdir(parents=True)
    tokens = dict(VERSION=ctx.version, APP_NAME=ctx.config.display_name, EXE_NAME=ctx.exe_name,
                  PAYLOAD_DIR=linux['payload_dir'], ENV_PREFIX=linux['env_prefix'],
                  ARTIFACT_NAME=ctx.config.name)
    app_run = meta / 'AppRun'
    app_run.write_text(render_template(linux['app_run'], tokens), encoding='utf-8', newline='\n')
    desktop = meta / linux['desktop_file'].name
    desktop.write_text(render_template(linux['desktop_file'], tokens), encoding='utf-8', newline='\n')
    icon = meta / (desktop.stem + '.png')
    convert = shutil.which('magick') or shutil.which('convert')
    if linux['icon'].suffix.lower() == '.png':
        copy(linux['icon'], icon)
    elif convert:
        run([convert, linux['icon'], '-resize', '240x240', '-background', 'transparent',
             '-gravity', 'center', '-extent', '256x256', icon], 'icon conversion')
    else:
        fail('ImageMagick is required to convert %s to the AppImage PNG icon' % linux['icon'])
    artifact = ctx.out_dir / artifact_name(ctx.config, ctx.version, ctx.channel, 'linux')
    for stale in (artifact, Path(str(artifact) + '.sha256')):
        if stale.exists() or stale.is_symlink():
            stale.unlink()
    env = dict(ctx.tc.env, SOURCE_DATE_EPOCH=str(ctx.source_date_epoch),
               PSX_RELEASE_STAGE_PYTHON=sys.executable)
    command = ['bash', TOOLS / 'package_appimage.sh', '--payload', ctx.stage,
               '--exe-name', ctx.exe_name, '--payload-name', linux['payload_dir'],
               '--app-run', app_run, '--desktop-file', desktop, '--icon', icon,
               '--output', artifact, '--jobs', ctx.jobs]
    if linux['prune_libs']:
        command.append('--prune-unreferenced-libs')
    run(command, 'AppImage assembly', env=env)
    write_sha256(artifact)
    return artifact


# --------------------------------------------------------------------------
# Verification of a finished artifact (also the `verify` subcommand)
# --------------------------------------------------------------------------
def verify_payload(payload, exe_path, config, channel, plat, version):
    payload = Path(payload)
    manifest_path = payload / 'RELEASE_MANIFEST.json'
    if not manifest_path.is_file():
        fail('artifact has no RELEASE_MANIFEST.json')
    manifest = json.loads(read_text(manifest_path))
    problems = []
    if manifest.get('channel') != channel:
        problems.append('manifest channel %r != %s' % (manifest.get('channel'), channel))
    if manifest.get('version') != version:
        problems.append('manifest version %r != %s' % (manifest.get('version'), version))
    if read_text(payload / 'psx_game_version.txt').strip() != version:
        problems.append('psx_game_version.txt does not say %s' % version)
    start = read_text(payload / 'START_HERE.txt')
    if version not in start or '@VERSION@' in start:
        problems.append('START_HERE.txt is not stamped with %s' % version)
    staged = staged_mod_ids(payload)
    if staged != manifest.get('mod_packages'):
        problems.append('mod catalog %s != manifest %s' % (staged, manifest.get('mod_packages')))
    found = diagnostics_manifests(payload / 'mods/bundled')
    if config.diagnostics:
        want = channel == 'playtest'
        entries = found.get(config.diagnostics['package_id'], [])
        if not entries:
            problems.append('diagnostics package missing')
        for manifest_file, data in entries:
            features = {f.get('id'): f for f in data.get('feature', [])}
            for feature in ('recording', 'osd'):
                if features[feature].get('default_enabled') is not want:
                    problems.append('%s %s default_enabled is not %r' % (manifest_file, feature, want))
    elif found:
        problems.append('undeclared diagnostics package(s) %s' % sorted(found))
    files = manifest.get('files', {})
    for rel, digest in files.items():
        path = payload / rel
        if plat == 'linux' and rel in (manifest.get('executable'),
                                       '%s.execution.json' % manifest.get('executable')):
            continue  # moved to usr/bin, possibly re-linked; re-bound by its execution contract
        if not path.is_file():
            problems.append('manifest file missing: %s' % rel)
        elif sha256(path) != digest:
            problems.append('manifest hash mismatch: %s' % rel)
    if problems:
        fail('verification failed:\n  ' + '\n  '.join(problems))
    exe_name = manifest['executable']
    # Re-run the content gates over exactly what a player receives (the
    # payload is a private extraction, so the AppImage's ELF can join it).
    if plat == 'linux':
        copy(exe_path, payload / exe_name)
    payload_gates(payload, plat, exe_name)
    return manifest


def verify_artifact(artifact, config, channel, version, objdump=None, scratch=None):
    """Extract into a private directory under `scratch` (default: beside the
    artifact; never the system temp drive on Windows) and check everything."""
    artifact = Path(artifact)
    plat = 'windows' if artifact.name.endswith('.zip') else 'linux'
    expected = artifact_name(config, version, channel, plat)
    if artifact.name != expected:
        fail('artifact name %s != expected %s' % (artifact.name, expected))
    sidecar = Path(str(artifact) + '.sha256')
    if not sidecar.is_file() or read_text(sidecar).split()[0] != sha256(artifact):
        fail('%s missing or does not match' % sidecar.name)
    if scratch is None:
        scratch = tempfile.gettempdir() if host_platform() == 'linux' else artifact.parent
    work = Path(tempfile.mkdtemp(prefix='.psx-release-verify-', dir=str(scratch)))
    try:
        if plat == 'windows':
            with zipfile.ZipFile(artifact) as archive:
                for info in archive.infolist():
                    name = info.filename
                    if '\\' in name or name.startswith('/') or '..' in PurePosixPath(name).parts:
                        fail('unsafe/non-portable ZIP entry %r' % name)
                archive.extractall(work)
            payload = work / config.zip_folder if config.zip_folder else work
            if config.zip_folder and sorted(p.name for p in work.iterdir()) != [config.zip_folder]:
                fail('ZIP must hold exactly one top-level folder %s' % config.zip_folder)
            manifest = verify_payload(payload, None, config, channel, 'windows', version)
            exe = payload / manifest['executable']
            if objdump:
                windows_imports(exe, objdump)
            layout = 'zip'
        else:
            if host_platform() != 'linux':
                fail('verify an AppImage on Linux (or through WSL)')
            os.chmod(artifact, 0o755)
            run([artifact, '--appimage-extract'], 'AppImage extraction', cwd=work, capture=True)
            root = work / 'squashfs-root'
            linux = config.linux
            payload = root / 'usr/share' / linux['payload_dir']
            manifest = json.loads(read_text(payload / 'RELEASE_MANIFEST.json'))
            exe = root / 'usr/bin' / manifest['executable']
            for need, what in ((root / 'AppRun', 'AppRun'), (exe, 'usr/bin executable'),
                               (payload / 'game.toml', 'payload game.toml')):
                if not need.is_file():
                    fail('AppImage layout: %s missing' % what)
            if not os.access(root / 'AppRun', os.X_OK) or not os.access(exe, os.X_OK):
                fail('AppImage layout: AppRun/executable not executable')
            if exe.read_bytes()[:4] != b'\x7fELF':
                fail('AppImage executable is not ELF')
            if not list(root.glob('*.desktop')) or not (root / '.DirIcon').exists():
                fail('AppImage layout: desktop entry or .DirIcon missing')
            if not (root / 'usr/bin/assets').exists():
                fail('AppImage layout: usr/bin/assets link missing')
            missing = run(['env', '-u', 'LD_LIBRARY_PATH', 'ldd', exe], 'ldd', capture=True)
            if 'not found' in missing:
                fail('AppImage executable has unresolved libraries:\n' + missing)
            verify_payload(payload, exe, config, channel, 'linux', version)
            layout = 'appimage'
        log('VERIFIED %s: %s %s, %d mod packages, diagnostics %s, OpenBIOS only (%s)'
            % (artifact.name, manifest['product'], manifest['version'], len(manifest['mod_packages']),
               {True: 'on', False: 'off', None: 'n/a'}[manifest['playtest_diagnostics_default_enabled']],
               layout))
        return manifest
    finally:
        shutil.rmtree(work, ignore_errors=True)


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
def to_wsl_path(path):
    path = str(Path(path).resolve())
    match = re.match(r'^([A-Za-z]):[\\/](.*)$', path)
    if not match:
        fail('cannot map %s into WSL (only drive paths are supported)' % path)
    return '/mnt/%s/%s' % (match.group(1).lower(), match.group(2).replace('\\', '/'))


def run_in_wsl(args, ctx):
    """Run this script's Linux half inside WSL on the same checkout."""
    wsl = shutil.which('wsl.exe') or shutil.which('wsl')
    if not wsl:
        fail('--platform linux on Windows needs WSL (wsl.exe not found)')
    argv = ['python3', to_wsl_path(Path(__file__)), 'package', '--channel', args.channel,
            '--platform', 'linux', '--root', to_wsl_path(ctx.root),
            '--config', to_wsl_path(ctx.config.path), '--jobs', str(ctx.jobs),
            '--out-dir', to_wsl_path(ctx.out_dir), '--sources-prepared']
    if args.work_dir:
        argv += ['--work-dir', to_wsl_path(Path(args.work_dir))]
    if args.allow_dirty:
        argv.append('--allow-dirty')
    env = {'PSX_RELEASE_SOURCE_COMMIT': ctx.prov['source_commit'],
           'PSX_RELEASE_SOURCE_DIRTY': '1' if ctx.prov['source_dirty'] else '0',
           'PSX_RELEASE_DIRTY_PATHS': json.dumps(ctx.prov['dirty_paths']),
           'PSX_RELEASE_FRAMEWORK_COMMIT': ctx.prov['framework_commit'],
           'PSX_RELEASE_UI_COMMIT': ctx.prov['recomp_ui_commit'] or '',
           'PSX_RELEASE_SOURCE_DATE_EPOCH': str(ctx.prov['source_date_epoch'])}
    script = 'set -e; ' + ' '.join('export %s=%s;' % (k, shlex.quote(v)) for k, v in env.items())
    script += ' exec ' + ' '.join(shlex.quote(a) for a in argv)
    run([wsl, '-e', 'bash', '-c', script], 'Linux packaging in WSL')


def make_context(args, plat):
    ctx = Context()
    ctx.root = Path(args.root).resolve()
    ctx.config = Config(Path(args.config) if Path(args.config).is_absolute()
                        else ctx.root / args.config, ctx.root)
    ctx.channel = args.channel
    ctx.platform = plat
    ctx.version = ctx.config.version()
    ctx.jobs = args.jobs
    ctx.out_dir = Path(args.out_dir).resolve() if args.out_dir else ctx.root / 'dist'
    ctx.out_dir.mkdir(parents=True, exist_ok=True)
    base = Path(args.work_dir).resolve() if args.work_dir else ctx.root / 'build-package'
    ctx.work = base / plat
    ctx.work.mkdir(parents=True, exist_ok=True)
    ctx.runtime_build = ctx.work / 'runtime'
    shared_emitters = getattr(args, 'recompiler_build_dir', None)
    ctx.recompiler_build = Path(shared_emitters).resolve() if shared_emitters else None
    ctx.stage = ctx.work / 'stage'
    ctx.dl_cache = base / 'dl-cache'
    return ctx


def package(args):
    host = host_platform()
    platforms = list(PLATFORMS) if args.platform == 'all' else [args.platform or host]
    if host == 'linux' and 'windows' in platforms:
        fail('Windows packages are built on a Windows host')
    artifacts = []
    for plat in platforms:
        ctx = make_context(args, plat)
        ui_root = ctx.root / 'recomp-ui'
        ctx.prov = provenance(ctx.root, ui_root)
        if ctx.prov['source_dirty'] and not args.allow_dirty:
            fail('tracked changes in the release source (%s); commit them, or pass --allow-dirty '
                 'for a non-publishable test package' % ', '.join(ctx.prov['dirty_paths'][:8]))
        log('== %s %s | channel %s | platform %s | source %s%s'
            % (ctx.config.name, ctx.version, ctx.channel, plat, ctx.prov['source_commit'][:12],
               ' (DIRTY)' if ctx.prov['source_dirty'] else ''))
        if plat == 'linux' and host == 'windows':
            # Prepare once with this host's emitters; Linux compiles the same sources.
            ctx.tc = Toolchain('windows', args.toolchain_bin)
            ctx.work = ctx.work.parent / 'windows'
            ctx.runtime_build = ctx.work / 'runtime'
            build_emitters(ctx)
            ensure_openbios(ctx)
            prepare_sources(ctx)
            run_in_wsl(args, ctx)
            artifacts.append(ctx.out_dir / artifact_name(ctx.config, ctx.version, ctx.channel, 'linux'))
            continue
        ctx.tc = Toolchain(plat, args.toolchain_bin)
        scratch = None
        if plat == 'linux':
            if hasattr(os, 'nice'):
                try:
                    os.nice(10)  # packaging must not make the machine unusable
                except OSError:
                    pass
            # The payload, AppDir and verification extraction need a native
            # Linux filesystem (symlinks, exec bits); a /mnt/<drive> DrvFs tree
            # cannot hold them. Removed when this platform finishes.
            scratch = Path(tempfile.mkdtemp(prefix='psx-title-release.', dir='/tmp'))
            fs = run(['stat', '-f', '-c', '%T', scratch], 'stat', capture=True).strip()
            if fs in ('9p', 'drvfs') or fs.startswith('ntfs'):
                fail('/tmp is not a native Linux filesystem (%s)' % fs)
            ctx.stage = scratch / 'stage'
            ctx.tc.env['TMPDIR'] = str(scratch)
            tempfile.tempdir = str(scratch)
        try:
            artifacts.append(package_platform(ctx, args))
        finally:
            if scratch is not None:
                tempfile.tempdir = None
                shutil.rmtree(scratch, ignore_errors=True)
    for artifact in artifacts:
        if not artifact.is_file():
            fail('expected artifact was not produced: %s' % artifact)
        log('PACKAGED %s' % artifact)
    return 0


def package_platform(ctx, args):
    plat = ctx.platform
    epoch = os.environ.get('SOURCE_DATE_EPOCH')
    ctx.source_date_epoch = (int(epoch) if epoch and epoch.isdigit()
                             else ctx.prov['source_date_epoch'])
    needs_emitters = (not args.sources_prepared or ctx.config.ship_toolchain or
                      ctx.config.aot_mode == 'cache')
    if needs_emitters:
        build_emitters(ctx)
    ensure_openbios(ctx)
    if not args.sources_prepared:
        prepare_sources(ctx)
    configure_and_build(ctx)
    check_build_outputs(ctx)
    stage_payload(ctx)
    artifact = package_windows(ctx) if plat == 'windows' else package_linux(ctx)
    verify_artifact(artifact, ctx.config, ctx.channel, ctx.version,
                    objdump=ctx.tc.objdump if plat == 'windows' else None,
                    scratch=ctx.work if plat == 'windows' else None)
    return artifact


def verify(args):
    root = Path(args.root).resolve()
    config = Config(Path(args.config) if Path(args.config).is_absolute() else root / args.config, root)
    objdump = None
    if os.name == 'nt':
        candidate = Path(args.toolchain_bin) / 'objdump.exe'
        objdump = str(candidate) if candidate.is_file() else shutil.which('objdump')
    for artifact in args.artifacts:
        verify_artifact(artifact, config, args.channel, config.version(), objdump=objdump)
    return 0


def check_diagnostics_dir(args):
    """Every *.diagnostics package in a staged mods/bundled follows the channel.
    For packagers that have no release config (package_game_release.sh)."""
    found = diagnostics_manifests(args.bundled)
    want = args.channel == 'playtest'
    for package, entries in sorted(found.items()):
        for manifest, data in entries:
            features = {f.get('id'): f for f in data.get('feature', [])}
            for feature in ('recording', 'osd'):
                got = features[feature].get('default_enabled')
                if got is not want or features[feature].get('channel') == 'developer':
                    fail('%s: %s default_enabled=%r, a %s package needs %r'
                         % (manifest, feature, got, args.channel, want))
        log('%s: Playtest Diagnostics default %s (%s)' % (package, 'on' if want else 'off',
                                                        args.channel))
    if not found:
        log('no Playtest Diagnostics package in %s' % args.bundled)
    return 0


def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog='package_title_release.py', description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command')

    def common(p):
        p.add_argument('--channel', required=True, choices=CHANNELS,
                       help='REQUIRED, no default: public or playtest')
        p.add_argument('--config', default='packaging/release.toml',
                       help='title release config, relative to --root')
        p.add_argument('--root', default='.', help='title repository root (default: cwd)')
        p.add_argument('--toolchain-bin', default=r'C:\msys64\mingw64\bin',
                       help='Windows: MinGW-w64 bin with cmake/ninja/gcc/objdump')

    pkg = sub.add_parser('package', help='build, stage, package and verify (default)')
    common(pkg)
    pkg.add_argument('--platform', choices=PLATFORMS + ('all',),
                     help='default: this host; on Windows "linux"/"all" use WSL')
    cores = os.cpu_count() or 4
    pkg.add_argument('--jobs', type=int, default=max(1, min(6, cores - 2)),
                     help='parallel jobs (default min(6, cores-2))')
    pkg.add_argument('--out-dir', help='artifact directory (default <root>/dist)')
    pkg.add_argument('--work-dir', help='build/stage directory (default <root>/build-package)')
    pkg.add_argument('--recompiler-build-dir',
                     help='shared native emitter build directory; reconfigured/rebuilt against current framework each run')
    pkg.add_argument('--allow-dirty', action='store_true',
                     help='package uncommitted tracked changes (recorded; not for publication)')
    pkg.add_argument('--sources-prepared', action='store_true', help=argparse.SUPPRESS)

    ver = sub.add_parser('verify', help='extract and verify finished artifacts')
    common(ver)
    ver.add_argument('artifacts', nargs='+')

    chk = sub.add_parser('check-diagnostics',
                         help='check a staged mods/bundled against the channel (no config)')
    chk.add_argument('--channel', required=True, choices=CHANNELS)
    chk.add_argument('bundled', type=Path)

    if not argv or argv[0] not in ('package', 'verify', 'check-diagnostics', '-h', '--help'):
        argv = ['package'] + list(argv)
    args = parser.parse_args(argv)
    if args.command == 'package' and args.jobs < 1:
        parser.error('--jobs must be positive')
    return args


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        args = parse_args(argv)
    except SystemExit as exit_:
        if exit_.code not in (0, None):
            print('\nPACKAGING FAILED: invalid arguments (--channel public|playtest is required)',
                  file=sys.stderr, flush=True)
        raise
    try:
        if args.command == 'check-diagnostics':
            return check_diagnostics_dir(args)
        return package(args) if args.command == 'package' else verify(args)
    except PackagingError as error:
        print('\nPACKAGING FAILED: %s' % error, file=sys.stderr, flush=True)
        return 1
    except KeyboardInterrupt:
        print('\nPACKAGING FAILED: interrupted', file=sys.stderr, flush=True)
        return 130
    except Exception as error:  # an unexpected bug is still a loud failure
        import traceback
        traceback.print_exc()
        print('\nPACKAGING FAILED: %s: %s' % (type(error).__name__, error), file=sys.stderr, flush=True)
        return 1


if __name__ == '__main__':
    sys.exit(main())
