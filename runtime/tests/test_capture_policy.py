"""Actual capture-family CMake selection and dependency guards.

Writer/ring executable tests cover behavior; this checks which maintained
bodies and dependency interface a configured product selects.
"""
from pathlib import Path
import argparse
import json
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--cmake', required=True)
p.add_argument('--cc', required=True)
p.add_argument('--ninja', required=True)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='psx-capture-policy-') as temp:
    temp = Path(temp)
    for name, declaration in [('new', '#define RBE_SNAP_CAPTURE_API_VERSION 1\n'), ('old', '')]:
        dependency = temp / name
        (dependency / 'include/retcomm_rbengine').mkdir(parents=True)
        (dependency / 'src/snap').mkdir(parents=True)
        (dependency / 'include/retcomm_rbengine/snap_ring.h').write_text(declaration)
        (dependency / 'src/snap/rbe_snap_ring.c').write_text('/* Hash input for configuration-only fixture. */\n')
    source = temp / 'src'
    source.mkdir()
    (source / 'main.c').write_text('int main(void){return 0;}\n')
    (source / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(capture_policy C)
set(PSXRECOMP_ROOT "{root.as_posix()}")
set(PSXRECOMP_HAS_RBENGINE ${{USE_RING}})
include("${{PSXRECOMP_ROOT}}/cmake/psx_execution_profile.cmake")
include("${{PSXRECOMP_ROOT}}/cmake/psx_capture_jobs.cmake")
add_executable(fixture main.c "${{PSXRECOMP_ROOT}}/runtime/src/boot_state.c")
psxrecomp_execution_profile(fixture)
psxrecomp_capture_jobs(fixture)
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/selected.txt" CONTENT "$<JOIN:$<TARGET_PROPERTY:fixture,COMPILE_DEFINITIONS>,\\n>")
file(GENERATE OUTPUT "${{CMAKE_BINARY_DIR}}/sources.txt" CONTENT "$<JOIN:$<TARGET_PROPERTY:fixture,SOURCES>,\\n>")
''')
    manifests = {}
    cases = [('default', 'ENHANCED', None, True, 'new', 1),
             ('on', 'ENHANCED', 'ON', True, 'new', 1),
             ('off-old', 'ENHANCED', 'OFF', True, 'old', 0),
             ('reference-old', 'REFERENCE', 'ON', True, 'old', 0),
             ('no-ring', 'ENHANCED', 'ON', False, 'absent', 1),
             ('off-no-ring', 'ENHANCED', 'OFF', False, 'absent', 0)]
    for name, profile, option, ring, dependency, selected in cases:
        build = temp / ('build-' + name)
        argv = [a.cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
                '-DCMAKE_C_COMPILER=' + a.cc, '-DCMAKE_MAKE_PROGRAM=' + a.ninja,
                '-DPSX_EXECUTION_PROFILE=' + profile, '-DUSE_RING=' + ('ON' if ring else 'OFF'),
                '-DRECOMP_RBENGINE_ROOT=' + (temp / dependency).as_posix()]
        if option is not None:
            argv += ['-DPSX_NATIVE_CAPTURE_JOBS=' + option]
        result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        assert result.returncode == 0, result.stdout
        definitions = (build / 'selected.txt').read_text().splitlines()
        assert [x for x in definitions if x.startswith('PSX_NATIVE_CAPTURE_JOBS=')] == [f'PSX_NATIVE_CAPTURE_JOBS={selected}']
        names = [Path(x).name for x in (build / 'sources.txt').read_text().splitlines()]
        expected = ['boot_state_capture_jobs.c'] if selected else ['boot_state.c']
        if ring and selected:
            expected += ['capture_ring.c']
        for body in ('boot_state.c', 'boot_state_capture_jobs.c', 'capture_ring.c'):
            assert names.count(body) == int(body in expected), (name, names)
        manifests[name] = json.loads((build / 'psx_execution/fixture/manifest.json').read_text())
    assert manifests['default'] == manifests['on']
    assert manifests['on']['identity'] != manifests['off-old']['identity']
    assert manifests['off-old']['implementations'] == manifests['reference-old']['implementations']
    argv = [a.cmake, '-S', str(source), '-B', str(temp / 'reject-old'), '-G', 'Ninja',
            '-DCMAKE_C_COMPILER=' + a.cc, '-DCMAKE_MAKE_PROGRAM=' + a.ninja,
            '-DUSE_RING=ON', '-DRECOMP_RBENGINE_ROOT=' + (temp / 'old').as_posix()]
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    assert result.returncode != 0 and 'need rbengine reusable capture API v1' in result.stdout, result.stdout
print('PASS: default/ON, old-dependency OFF/REFERENCE, ring-disabled selection and obsolete API rejection')
