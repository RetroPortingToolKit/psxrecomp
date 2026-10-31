"""Configure real BIOS binding/profile helpers without copyrighted inputs."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cmake', required=True)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--ninja', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    source = args.output.resolve() / 'src'
    source.mkdir(parents=True, exist_ok=True)
    body = '''/* BIOS SHA256: fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57 */
void OpenBIOS_func_1FC03A78(CPUState* cpu);
void OpenBIOS_func_1FC03A78(CPUState* cpu) { original_request(cpu); }
void continuation(CPUState* cpu) { OpenBIOS_func_1FC03A78(cpu); }
void direct_caller(CPUState* cpu) { OpenBIOS_func_1FC03A78(cpu); }
'''
    bios = source / 'OpenBIOS_full.c'
    bios.write_text(body, encoding='utf-8')
    (source / 'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(bios_binding C)
set(PSXRECOMP_ROOT "{root.as_posix()}")
include("${{PSXRECOMP_ROOT}}/cmake/psx_execution_profile.cmake")
include("${{PSXRECOMP_ROOT}}/cmake/psx_openbios_read_job.cmake")
set(FIXTURE_BIOS OpenBIOS_full.c CACHE STRING "Fixture BIOS source")
add_executable(fixture ${{FIXTURE_BIOS}})
psxrecomp_execution_profile(fixture)
psxrecomp_openbios_read_job(fixture)
get_target_property(_sources fixture SOURCES)
file(WRITE "${{CMAKE_CURRENT_BINARY_DIR}}/selected.txt" "${{_sources}}")
''', encoding='utf-8')

    def configure(name, profile, enabled=None, *, cached=None, other_bios=False, error=None):
        out = args.output.resolve() / name
        command = [args.cmake, '-S', str(source), '-B', str(out),
                   '-G', 'Ninja', '-DCMAKE_C_COMPILER=' + args.cc,
                   '-DCMAKE_MAKE_PROGRAM=' + args.ninja,
                   '-DPSX_EXECUTION_PROFILE=' + profile]
        if enabled is not None:
            command.append('-DPSX_NATIVE_OPENBIOS_READ=' + enabled)
        if other_bios:
            command.append('-DFIXTURE_BIOS=RetailBIOS_full.c')
        result = subprocess.run(command,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (args.output / (name + '.log')).write_text(result.stdout, encoding='utf-8')
        if error:
            assert result.returncode != 0 and error in result.stdout, result.stdout
        else:
            assert result.returncode == 0, result.stdout
            manifest = json.loads((out / 'psx_execution/fixture/manifest.json').read_text())
            native = any(x['name'] == 'openbios-block-read' for x in manifest['implementations'])
            cache = (out / 'CMakeCache.txt').read_text()
            effective = cached or enabled or 'ON'
            assert 'PSX_NATIVE_OPENBIOS_READ:BOOL=' + effective in cache
            assert native == (profile == 'ENHANCED' and effective == 'ON' and not other_bios)
            if native:
                selected = (out / 'psx_execution/fixture/OpenBIOS_read_selected.c').read_text()
                assert selected == body.replace('void OpenBIOS_func_1FC03A78(CPUState* cpu) {',
                                                 'void psx_lle_OpenBIOS_func_1FC03A78(CPUState* cpu) {', 1)
                assert selected.count('OpenBIOS_func_1FC03A78(cpu);') == 2
            else:
                selected = (out / 'selected.txt').read_text()
                assert 'openbios_read_job.c' not in selected
                assert ('RetailBIOS_full.c' if other_bios else 'OpenBIOS_full.c') in selected
        print(name, 'passed')

    configure('default-on', 'ENHANCED')
    configure('explicit-off', 'ENHANCED', 'OFF')
    # Reconfiguring an existing OFF cache must preserve the developer's choice.
    configure('explicit-off', 'ENHANCED', cached='OFF')
    configure('native', 'ENHANCED', 'ON')
    configure('reference-default', 'REFERENCE')
    configure('reference', 'REFERENCE', 'ON')
    (source / 'RetailBIOS_full.c').write_text('void retail_bios(void) {}\n', encoding='utf-8')
    configure('retail-bios-default', 'ENHANCED', other_bios=True)
    configure('retail-bios-on', 'ENHANCED', 'ON', other_bios=True)
    bios.write_text(body.replace('fabe498f', '00000000'), encoding='utf-8')
    configure('wrong-image', 'ENHANCED', 'ON', error='audited BIOS image')
    bios.write_text(body.replace('void OpenBIOS_func_1FC03A78(CPUState* cpu) {',
                                 'void missing_read_body(CPUState* cpu) {'), encoding='utf-8')
    configure('missing-body', 'ENHANCED', 'ON', error='exactly one generated read body')
    bios.write_text(body, encoding='utf-8')


if __name__ == '__main__':
    main()
