"""Bounded real GCC/Ninja contract fixture, with at most two compiler children.

Run through the benchmark pool, for example:
  python tools/tests/test_runtime_hot_pgo.py --tools tools.json \
      --scratch <fresh directory> --receipt <result.json>
tools.json names absolute cmake, gcc, gxx and ninja executables. No game assets,
display, downloaded dependencies or trained production profiles are required.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import uuid


def check_failure(action, expected):
    try:
        action()
    except RuntimeError as error:
        assert expected in str(error), str(error)
        return str(error)
    raise AssertionError('Expected rejection: ' + expected)


def fixture(tools, scratch, receipt):
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location('runtime_hot_pgo', root / 'tools/runtime_hot_pgo.py')
    driver = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(driver)
    scratch = scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=False)
    framework, source, build = [scratch / name for name in ('framework', 'source', 'build')]
    (framework / 'runtime/src').mkdir(parents=True)
    (source / 'app').mkdir(parents=True)
    header = framework / 'runtime/src/fixture.h'
    header.write_text('#pragma once\n#define FIXTURE_BIAS 17u\n', encoding='utf-8')
    memory = framework / 'runtime/src/memory.c'
    original = '''#include "fixture.h"
unsigned mem_helper(unsigned value) {
    if (value & 1u) return value * 3u + FIXTURE_BIAS;
    return value + FIXTURE_BIAS;
}
'''
    memory.write_text(original, encoding='utf-8')
    (framework / 'runtime/src/psx_cycles.c').write_text('''#include "fixture.h"
unsigned cycle_helper(unsigned value) {
    return (value ^ FIXTURE_BIAS) + ((value > 100u) ? 3u : 7u);
}
''', encoding='utf-8')
    (framework / 'runtime/src/pgxp.cpp').write_text('''#include "fixture.h"
extern "C" unsigned pgxp_helper(unsigned value) {
    return value < 200u ? value * FIXTURE_BIAS : value / 3u;
}
''', encoding='utf-8')
    (source / 'main.c').write_text('''#include <stdio.h>
unsigned mem_helper(unsigned); unsigned cycle_helper(unsigned); unsigned pgxp_helper(unsigned);
int main(void) {
    unsigned sum = 0;
    for (unsigned i = 0; i < 100000u; ++i)
        sum += mem_helper(i) ^ cycle_helper(i) ^ pgxp_helper(i);
    printf("%u\\n", sum);
    return 0;
}
''', encoding='utf-8')
    (source / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.19)
project(runtime_hot_pgo_fixture LANGUAGES C CXX)
add_subdirectory(app)
''', encoding='utf-8')
    (source / 'app/CMakeLists.txt').write_text('''set(_shared
    "${PSXRECOMP_ROOT}/runtime/src/memory.c"
    "${PSXRECOMP_ROOT}/runtime/src/psx_cycles.c"
    "${PSXRECOMP_ROOT}/runtime/src/pgxp.cpp")
foreach(_target psx-runtime psx-runtime-pgxp psx-runtime-oracle)
    add_executable(${_target} "${CMAKE_SOURCE_DIR}/main.c" ${_shared})
    target_compile_options(${_target} PRIVATE -O2)
endforeach()
''', encoding='utf-8')
    binary = 'app/psx-runtime' + ('.exe' if os.name == 'nt' else '')
    config = dict(source=str(source), build=str(build), framework=str(framework),
                  cmake=tools['cmake'], binary=binary, configure_args=[
                      '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                      '-DCMAKE_C_COMPILER=' + tools['gcc'],
                      '-DCMAKE_CXX_COMPILER=' + tools['gxx'],
                      '-DCMAKE_MAKE_PROGRAM=' + tools['ninja']])
    result = dict(schema=1, status='running', tools=tools, scratch=str(scratch), phases={}, guards={})
    receipt.parent.mkdir(parents=True, exist_ok=True)

    def build_siblings():
        subprocess.run([tools['cmake'], '--build', str(build), '--target',
                        'psx-runtime-pgxp', 'psx-runtime-oracle', '--parallel', '2'], check=True)

    def output(path):
        return subprocess.check_output([str(path)], text=True).strip()

    def fail_compile(expected):
        process = subprocess.run([tools['cmake'], '--build', str(build), '--target',
                                  'psx-runtime', '--clean-first', '--parallel', '2'],
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        assert process.returncode != 0 and expected in process.stdout, process.stdout
        return dict(exit_code=process.returncode, expected=expected, output=process.stdout)

    try:
        # A path can resolve back into build while its original spelling walks
        # out of artifacts/<phase>. Reject it before any configure/compiler.
        escaped = '../build/' + binary
        result['guards']['binary_parent_path'] = check_failure(
            lambda: driver.run(dict(config, binary=escaped), 'baseline', 2), 'parent components')
        result['guards']['companion_parent_path'] = check_failure(
            lambda: driver.run(dict(config, companions=[escaped]), 'baseline', 2), 'parent components')
        assert not build.exists()
        # The hook is absent in ordinary builds. Shared files also build on each
        # sibling without requiring another target's profiles.
        subprocess.run([tools['cmake'], '-S', str(source), '-B', str(build)] +
                       config['configure_args'] + ['-DPSXRECOMP_ROOT=' + str(framework)], check=True)
        build_siblings()
        for phase in ('baseline', 'generate'):
            result['phases'][phase] = driver.run(config, phase, 2)
            build_siblings()
            result[phase + '_output'] = output(build / binary)
        assert result['baseline_output'] == result['generate_output']
        generated = result['phases']['generate']
        if os.name == 'nt':
            too_long = build / ('long-' + 'x' * 180)
            long_commands = {name: dict(row, output=str(too_long / Path(row['output']).name))
                             for name, row in generated['commands'].items()}
            result['guards']['windows_profile_path_length'] = check_failure(
                lambda: driver.profile_paths(build, 'psx-runtime', long_commands), 'profile path is too long')
        actual = driver.profiles(build, 'psx-runtime', generated['commands'])
        result['actual_training_profiles'] = actual
        absent = Path(next(iter(actual.values()))['path'])
        held = absent.with_suffix('.held')
        absent.rename(held)
        try:
            result['guards']['missing'] = check_failure(lambda: driver.run(config, 'use', 2), 'selected profile')
        finally:
            held.rename(absent)
        header_bytes = header.read_bytes()
        header.write_bytes(header_bytes + b'// Stale selected dependency\n')
        try:
            result['guards']['stale_header'] = check_failure(lambda: driver.run(config, 'use', 2), 'Stale profile')
        finally:
            header.write_bytes(header_bytes)
        changed = dict(config, configure_args=config['configure_args'] + ['-DFIXTURE_CHANGED=ON'])
        result['guards']['changed_configuration'] = check_failure(
            lambda: driver.run(changed, 'use', 2), 'matching successful')
        assert not (build / 'runtime-hot-pgo/psx-runtime/use.json').exists()
        result['phases']['use'] = driver.run(config, 'use', 2)
        build_siblings()
        result['use_output'] = output(build / binary)
        assert result['use_output'] == result['baseline_output']
        result['sibling_outputs'] = {
            name: output(build / ('app/' + name + ('.exe' if os.name == 'nt' else '')))
            for name in ('psx-runtime-pgxp', 'psx-runtime-oracle')}
        assert all(value == result['use_output'] for value in result['sibling_outputs'].values())
        # Prove GCC itself also rejects an absent profile / changed CFG rather
        # than relying only on the driver's earlier identity checks.
        absent.rename(held)
        try:
            result['guards']['compiler_missing'] = fail_compile('missing-profile')
        finally:
            held.rename(absent)
        memory.write_text(original.replace('    if (value & 1u)',
                          '    if (value & 2u) return value * 7u;\n    if (value & 1u)'), encoding='utf-8')
        try:
            result['guards']['compiler_cfg_mismatch'] = fail_compile('coverage-mismatch')
        finally:
            memory.write_text(original, encoding='utf-8')
        subprocess.run([tools['cmake'], '--build', str(build), '--target', 'psx-runtime',
                        '--parallel', '2'], check=True)
        assert output(build / binary) == result['baseline_output']
        assert driver.profiles(build, 'psx-runtime', generated['commands']) == actual
        result['status'] = 'passed-real-gcc-selective-pgo-target-isolation-and-profile-guards'
    except BaseException as error:
        result.update(status='failed', failure=str(error))
        raise
    finally:
        receipt.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(result['status'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True)
    scratch = parser.add_mutually_exclusive_group(required=True)
    scratch.add_argument('--scratch', type=Path)
    scratch.add_argument('--scratch-parent', type=Path,
                         help='CTest mode: retain each fresh run and receipt under this directory')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.scratch_parent:
        if args.receipt:
            parser.error('--scratch-parent retains its own per-run receipt; do not pass --receipt')
        run = args.scratch_parent.resolve() / ('run-' + uuid.uuid4().hex[:8])
        scratch, receipt = run / 'work', run / 'result.json'
    else:
        if args.receipt is None:
            parser.error('--scratch requires --receipt')
        scratch, receipt = args.scratch, args.receipt
    print(json.dumps({'fixture_scratch': str(scratch), 'fixture_receipt': str(receipt)}), flush=True)
    # DLL lookup for the explicitly selected installed MinGW tools only.
    tools = json.loads(args.tools.read_text(encoding='utf-8'))
    os.environ['PATH'] = str(Path(tools['gcc']).parent) + os.pathsep + os.environ['PATH']
    fixture(tools, scratch, receipt)


if __name__ == '__main__':
    main()
