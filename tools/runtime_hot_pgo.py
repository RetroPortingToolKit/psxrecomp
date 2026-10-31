"""Reproduce the qualified selective shared-runtime PGO recipe.

Three build phases share one build/object directory. Between generate and use,
run the instrumented game through representative work and close it normally.
This tool never launches games, times workloads or treats instrumentation as a
runtime gain. Ordinary builds remain available without this optional hook.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

SOURCES = ('runtime/src/pgxp.cpp', 'runtime/src/psx_cycles.c', 'runtime/src/memory.c')
ROOT = Path(__file__).resolve().parents[1]
HOOK = ROOT / 'cmake/psx_runtime_hot_pgo.cmake'
CONTROLLED = ('PSXRECOMP_ROOT', 'PSX_DEBUG_TOOLS', 'PSX_RUNTIME_IPO', 'PSX_PGO',
              'CMAKE_EXPORT_COMPILE_COMMANDS', 'CMAKE_PROJECT_INCLUDE',
              'PSX_RUNTIME_HOT_PGO_MODE', 'PSX_RUNTIME_HOT_PGO_TARGET')


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def cache_values(build):
    result = {}
    for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace').splitlines():
        if line and not line.startswith(('#', '//')) and ':' in line and '=' in line:
            key, value = line.split('=', 1)
            result[key.split(':', 1)[0]] = value
    return result


def output_path(entry):
    if 'output' in entry:
        output = entry['output']
    else:
        match = re.search(r'(?:^|\s)-o\s+(?:"([^"]+)"|(\S+))', entry['command'])
        if not match:
            raise RuntimeError('Selected GCC command has no output path')
        output = next(value for value in match.groups() if value is not None)
    path = Path(output)
    return (path if path.is_absolute() else Path(entry['directory']) / path).resolve()


def compiler_commands(build, framework, target, phase):
    entries = read_json(build / 'compile_commands.json')
    selected = {}
    for entry in entries:
        source = Path(entry['file']).resolve()
        name = next((name for name in SOURCES if source == (framework / name).resolve()), None)
        output = output_path(entry)
        owned = ('CMakeFiles/' + target + '.dir/') in output.as_posix()
        command = entry['command']
        if name is None or not owned:
            if '-fprofile-' in command:
                raise RuntimeError('Profile flags leaked into another source/target: ' + str(source))
            continue
        if name in selected:
            raise RuntimeError('Ambiguous selected source command: ' + name)
        flag = {'baseline': None, 'generate': '-fprofile-generate=', 'use': '-fprofile-use='}[phase]
        if flag and flag not in command:
            raise RuntimeError('Profile flags missing from selected source: ' + name)
        if phase == 'baseline' and '-fprofile-' in command:
            raise RuntimeError('Baseline has profile flags')
        normalized = re.sub(r'(?:"-fprofile-[^"]+"|-fprofile-\S+|-Werror=(?:missing-profile|coverage-mismatch))', '', command)
        selected[name] = dict(command=command, normalized=' '.join(normalized.split()),
                              output=str(output), directory=entry['directory'])
    if set(selected) != set(SOURCES):
        raise RuntimeError('Exactly three selected runtime source commands are required')
    return selected


def dependency_identity(build, commands, cache):
    """Read Ninja's actual dependency sets for the three selected objects."""
    if cache.get('CMAKE_GENERATOR') != 'Ninja':
        raise RuntimeError('This reproducible workflow currently requires the Ninja generator')
    ninja = Path(cache['CMAKE_MAKE_PROGRAM'])
    files = {str(HOOK.resolve()): sha(HOOK), str(Path(__file__).resolve()): sha(__file__)}
    for row in commands.values():
        output = Path(row['output']).relative_to(build).as_posix()
        text = subprocess.check_output([str(ninja), '-C', str(build), '-t', 'deps', output],
                                       text=True, encoding='utf-8', errors='strict')
        headers = [Path(line.strip()) for line in text.splitlines() if line.startswith('    ')]
        if not headers:
            raise RuntimeError('Selected object has no actual Ninja dependency record: ' + output)
        for header in headers:
            path = (header if header.is_absolute() else build / header).resolve()
            if not path.is_file():
                raise RuntimeError('Recorded source/header dependency is missing: ' + str(path))
            files[str(path)] = sha(path)
    compilers = {}
    for key in ('CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER'):
        path = Path(cache[key]).resolve()
        compilers[key] = dict(path=str(path), sha256=sha(path))
    return dict(files=files, compilers=compilers)


def verify_inputs(record):
    for path, digest in record['dependencies']['files'].items():
        if not Path(path).is_file() or sha(path) != digest:
            raise RuntimeError('Stale profile source/header/workflow input: ' + path)
    for compiler in record['dependencies']['compilers'].values():
        if sha(compiler['path']) != compiler['sha256']:
            raise RuntimeError('Stale profile compiler: ' + compiler['path'])


def retain_artifacts(build, directory, identity, phase):
    """Retain phase binaries/sidecars, not an incomplete claim of a release package."""
    destination = directory / 'artifacts' / phase
    destination.mkdir(parents=True, exist_ok=False)
    result = []
    for relative in [identity['binary']] + identity['companions']:
        source = (build / relative).resolve()
        source.relative_to(build)
        if not source.is_file():
            raise RuntimeError('Configured build artifact is missing: ' + str(source))
        target = (destination / relative).resolve()
        target.relative_to(destination.resolve())
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        result.append(dict(path=str(target), sha256=sha(target), bytes=target.stat().st_size))
    return result


def profile_paths(build, target, commands):
    directory = build / 'runtime-hot-pgo' / target / 'data'
    result = {}
    for name, row in commands.items():
        key = Path(row['output']).relative_to(build).with_suffix('.gcda').as_posix().replace('/', '#')
        path = directory / key
        # MinGW libgcov uses Win32 paths without Python's long-path support.
        # Refuse an unwritable training destination before compiling counters.
        if os.name == 'nt' and (len(str(path)) >= 260 or len(key) > 255):
            raise RuntimeError('MinGW profile path is too long; use a shorter dedicated build directory: ' + str(path))
        result[name] = path
    return result


def profiles(build, target, commands):
    directory = build / 'runtime-hot-pgo' / target / 'data'
    expected = profile_paths(build, target, commands)
    found = []
    for parent, children, files in os.walk(directory, followlinks=False):
        parent = Path(parent)
        if len(parent.relative_to(directory).parts) > 16:
            raise RuntimeError('Unexpected profile directory depth')
        for path in [parent] + [parent / name for name in children + files]:
            if path.is_symlink() or getattr(path, 'is_junction', lambda: False)():
                raise RuntimeError('Linked profile path: ' + str(path))
        found += [parent / name for name in files if name.endswith('.gcda')]
        if len(found) > len(SOURCES):
            raise RuntimeError('Profiles from an unselected target/configuration are present')
    result = {}
    for name, row in commands.items():
        key = expected[name].name
        matches = [path for path in found if path.name == key]
        if len(matches) != 1 or matches[0].stat().st_size <= 16:
            raise RuntimeError('Missing/ambiguous/empty selected profile: ' + name)
        path = matches[0]
        with path.open('rb') as stream:
            if stream.read(4) not in (b'adcg', b'gcda'):
                raise RuntimeError('Invalid GCC profile header: ' + name)
        result[name] = dict(path=str(path), sha256=sha(path), bytes=path.stat().st_size)
    if len(found) != len(SOURCES):
        raise RuntimeError('Exactly three profile files are required')
    return result


def run(config, phase, jobs):
    source, build, framework = [Path(config[key]).resolve() for key in ('source', 'build', 'framework')]
    target = config.get('target', 'psx-runtime')
    if jobs not in (1, 2):
        raise RuntimeError('Use one or two build jobs')
    if any(character in str(build) for character in ';,<>\n\r'):
        raise RuntimeError('Build path contains unsupported CMake generator-expression characters')
    if not re.fullmatch(r'[A-Za-z0-9_.+-]+', target):
        raise RuntimeError('Invalid selected target name')
    args = list(config.get('configure_args', []))
    if any(not isinstance(arg, str) for arg in args):
        raise RuntimeError('configure_args must be a JSON array of strings')
    for arg in args:
        if any(re.match(r'-[DU]' + re.escape(key) + r'(?::|=|$)', arg) for key in CONTROLLED):
            raise RuntimeError('Workflow owns the production/profile option: ' + arg)
        if arg in ('-S', '-B'):
            raise RuntimeError('Use source/build fields rather than -S/-B in configure_args')
    identity = dict(source=str(source), build=str(build), framework=str(framework), target=target,
                    cmake=str(Path(config['cmake']).resolve()), configure_args=args,
                    binary=config['binary'], companions=list(config.get('companions', [])))
    for relative in [identity['binary']] + identity['companions']:
        if not isinstance(relative, str) or Path(relative).is_absolute() or Path(relative).drive:
            raise RuntimeError('Build artifact names must be relative paths')
        if not Path(relative).parts or '..' in Path(relative).parts:
            raise RuntimeError('Build artifact names cannot contain parent components or be empty')
        (build / relative).resolve().relative_to(build)
    directory = build / 'runtime-hot-pgo' / target
    directory.mkdir(parents=True, exist_ok=True)
    receipt_path = directory / (phase + '.json')
    if receipt_path.exists():
        raise RuntimeError('Preserve the existing phase receipt; use another dedicated build directory')
    generated = None
    baseline = None
    if phase == 'generate' and (directory / 'data').exists():
        raise RuntimeError('Preserve existing profile data; use a fresh dedicated build directory')
    if phase == 'generate':
        baseline = read_json(directory / 'baseline.json')
        if baseline['status'] != 'built' or baseline['identity'] != identity:
            raise RuntimeError('No matching successful baseline build')
        verify_inputs(baseline)
    if phase == 'use':
        generated = read_json(directory / 'generate.json')
        if generated['status'] != 'built' or generated['identity'] != identity:
            raise RuntimeError('No matching successful instrumented build')
        verify_inputs(generated)
        training = profiles(build, target, generated['commands'])
    options = dict(PSXRECOMP_ROOT=framework.as_posix(), PSX_DEBUG_TOOLS='OFF', PSX_RUNTIME_IPO='OFF',
        PSX_PGO='', CMAKE_EXPORT_COMPILE_COMMANDS='ON', CMAKE_PROJECT_INCLUDE=HOOK.as_posix(),
        PSX_RUNTIME_HOT_PGO_MODE=phase, PSX_RUNTIME_HOT_PGO_TARGET=target)
    configure = [identity['cmake'], '-S', str(source), '-B', str(build)] + args + [
        '-D' + key + '=' + value for key, value in options.items()]
    record = dict(schema=1, phase=phase, status='building', identity=identity, configure=configure,
                  selected_sources=list(SOURCES), jobs=jobs)
    try:
        subprocess.run(configure, check=True)
        commands = compiler_commands(build, framework, target, phase)
        if phase != 'baseline':
            profile_paths(build, target, commands)
        record['commands'] = commands
        previous = generated if generated is not None else baseline
        if previous is not None:
            if {name: row['normalized'] for name, row in commands.items()} != {
                    name: row['normalized'] for name, row in previous['commands'].items()}:
                raise RuntimeError('Stale profile: selected compilation command changed')
        subprocess.run([identity['cmake'], '--build', str(build), '--target', target,
                        '--parallel', str(jobs)], check=True)
        record['dependencies'] = dependency_identity(build, commands, cache_values(build))
        if previous is not None:
            if record['dependencies'] != previous['dependencies']:
                raise RuntimeError('Source/header/compiler dependency set changed')
        if generated is not None:
            if profiles(build, target, commands) != training:
                raise RuntimeError('Profile files changed during use build')
            record['profiles'] = training
        record['artifacts'] = retain_artifacts(build, directory, identity, phase)
        record['status'] = 'built'
        print('Selective runtime PGO ' + phase + ' built. ' + (
            'Run representative gameplay and close normally before the use phase; training is not a performance sample.'
            if phase == 'generate' else 'Measure matched complete baseline/use packages before adoption.'))
    except BaseException as error:
        record.update(status='failed', failure=str(error))
        raise
    finally:
        receipt_path.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('baseline', 'generate', 'use'))
    parser.add_argument('--config', type=Path, required=True, help='Exact CMake source/build/framework/argv JSON')
    parser.add_argument('--jobs', type=int, choices=(1, 2), default=2)
    args = parser.parse_args()
    run(read_json(args.config), args.phase, args.jobs)


if __name__ == '__main__':
    main()
