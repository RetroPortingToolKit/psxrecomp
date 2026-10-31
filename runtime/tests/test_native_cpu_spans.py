"""Build the real native span selection and reject stale generated inputs."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def run(argv, ok=True):
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if (result.returncode == 0) != ok:
        raise AssertionError(result.stdout)
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--ninja', required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix='psx-native-spans-') as temp:
        source = Path(temp) / 'src'
        source.mkdir()
        (source / 'main.c').write_text('''#include <stdio.h>
#if defined(PSX_NATIVE_CPU_SPANS) && PSX_NATIVE_CPU_SPANS
#define NATIVE 1u
#else
#define NATIVE 0u
#endif
#if BOUND
extern const unsigned psx_build_native_cpu_span_model;
#else
static const unsigned psx_build_native_cpu_span_model = 0u;
#endif
int main(void) {
    printf("%u %u\\n", NATIVE, psx_build_native_cpu_span_model);
    return 0;
}
''')
        (source / 'generated.c').write_text('/* psx_native_span.h */\n')
        (source / 'stale.c').write_text('/* generated before native span support */\n')
        (source / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(span_policy C)
set(PSXRECOMP_ROOT "{root.as_posix()}")
include("${{PSXRECOMP_ROOT}}/cmake/psx_execution_profile.cmake")
include("${{PSXRECOMP_ROOT}}/cmake/psx_native_cpu_spans.cmake")
add_executable(fixture main.c)
target_compile_definitions(fixture PRIVATE BOUND=$<BOOL:${{PSX_NATIVE_CPU_SPANS}}>)
psxrecomp_execution_profile(fixture)
psxrecomp_native_cpu_spans(fixture "${{GENERATED_INPUT}}")
''')

        def configure(name, profile, enabled, generated, ok=True):
            build = Path(temp) / name
            argv = [args.cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
                    '-DCMAKE_C_COMPILER=' + args.cc, '-DCMAKE_MAKE_PROGRAM=' + args.ninja,
                    '-DPSX_EXECUTION_PROFILE=' + profile, '-DGENERATED_INPUT=' + generated]
            if enabled is not None:
                argv.append('-DPSX_NATIVE_CPU_SPANS=' + enabled)
            output = run(argv, ok=ok)
            if not ok:
                assert 'Regenerate game sources' in output or 'require materialized regenerated' in output, output
            return build

        def result(build):
            run([args.cmake, '--build', str(build), '--parallel', '2'])
            exe = build / ('fixture.exe' if (build / 'fixture.exe').exists() else 'fixture')
            values = [int(x) for x in run([str(exe)]).split()]
            manifest = json.loads((build / 'psx_execution/fixture/manifest.json').read_text())
            return values, manifest

        native, hle = result(configure('native', 'ENHANCED', 'ON', 'generated.c'))
        reference, lle = result(configure('reference', 'REFERENCE', 'ON', 'missing.c'))
        disabled, unbound = result(configure('disabled', 'ENHANCED', 'OFF', 'stale.c'))
        fresh_native, fresh_hle = result(configure('fresh-native', 'ENHANCED', None, 'generated.c'))
        fresh_reference, fresh_lle = result(configure('fresh-reference', 'REFERENCE', None, 'missing.c'))
        assert native == [1, 1], native
        assert reference == [0, 0], reference
        assert disabled == [0, 0], disabled
        assert fresh_native == native and fresh_hle['identity'] == hle['identity']
        assert fresh_reference == reference and fresh_lle['identity'] == lle['identity']
        assert hle['identity'] != lle['identity']
        assert hle['implementations'][0]['name'] == lle['implementations'][0]['name'] == 'cpu-spans'
        assert not unbound['implementations']
        configure('stale', 'ENHANCED', 'ON', 'stale.c', ok=False)
        configure('missing', 'ENHANCED', 'ON', 'missing.c', ok=False)
    print('PASS: fresh native/reference defaults, explicit native/reference/off execution, identity, and stale/missing rejection')


if __name__ == '__main__':
    main()
