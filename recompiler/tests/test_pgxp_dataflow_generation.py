#!/usr/bin/env python3
"""Reject retired PGXP modes and retain current demand-generation admission."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LEGACY = ('PSX_PGXP_DATAFLOW', 'PSX_PGXP_OPERATIONS', 'PSX_NATIVE_PRECISION_SLICES')
FAMILIES = (
    ('pgxp_dataflow', LEGACY[0], 'psxrecomp_pgxp_dataflow', 'psx-pgxp-dataflow-v2'),
    ('precision_operations', LEGACY[1], 'psxrecomp_precision_operations', 'psx-pgxp-operations-v2'),
    ('precision_slices', LEGACY[2], 'psxrecomp_precision_slices', None),
)


def cmake_case(cmake, work, family, option, callee, marker_name, *,
               profile='ENHANCED', spans='ON', observation='ON', selected=None,
               markers=(False,), after_include=False, error=None, expected=None):
    sources = []
    for index, marker in enumerate(markers):
        source = work / ('body%d.c' % index)
        source.write_text('/* %s */\n' % (marker_name if marker else 'stale precision generation'),
                          encoding='utf-8')
        sources.append(source.as_posix())
    options = {flag: 'OFF' for flag in (*LEGACY, 'PSX_PGXP_DEMAND_REGIONS')}
    options.pop(option)
    if selected is not None and not after_include:
        options[option] = selected
    settings = '\n'.join('set(%s %s)' % item for item in options.items())
    arguments = '' if family == 'precision_slices' else ' GENERATED_SOURCES ' + ' '.join('"%s"' % p for p in sources)
    driver = work / 'check.cmake'
    driver.write_text("""cmake_minimum_required(VERSION 3.20)
set(PSXRECOMP_ROOT "@ROOT@")
set(CMAKE_CURRENT_BINARY_DIR "@WORK@")
set(PSX_NATIVE_CPU_SPANS @SPANS@)
set(PSX_OBSERVATION_TIME @OBSERVATION@)
@SETTINGS@
function(get_target_property output)
    set(${output} "@PROFILE@" PARENT_SCOPE)
endfunction()
function(target_compile_definitions)
    message(STATUS "selected: ${ARGV}")
endfunction()
function(psxrecomp_add_implementation)
endfunction()
include("@ROOT@/cmake/psx_@FAMILY@.cmake")
@LATE@
@CALLEE@(runtime@ARGUMENTS@)
""".replace('@ROOT@', ROOT.as_posix()).replace('@WORK@', work.as_posix())
        .replace('@SPANS@', spans).replace('@OBSERVATION@', observation)
        .replace('@SETTINGS@', settings).replace('@PROFILE@', profile)
        .replace('@FAMILY@', family).replace('@CALLEE@', callee)
        .replace('@ARGUMENTS@', arguments)
        .replace('@LATE@', 'set(%s %s)' % (option, selected) if after_include else ''),
        encoding='utf-8')
    result = subprocess.run([cmake, '-P', str(driver)], capture_output=True, text=True)
    output = result.stdout + result.stderr
    if error:
        if result.returncode == 0 or any(token not in output for token in error):
            raise RuntimeError('configuration did not reject for the intended reason\n' + output)
    elif result.returncode or any(token not in output for token in expected or ()):
        raise RuntimeError('supported precision configuration failed\n' + output)


def generation_checks(cmake, work):
    for family, option, callee, marker in FAMILIES:
        # Retired modes fail even when the former fast path would be inactive,
        # and even when old generated sources carry their capability marker.
        for profile, spans, capable in (
                ('ENHANCED', 'ON', False), ('ENHANCED', 'ON', True),
                ('REFERENCE', 'ON', True), ('ENHANCED', 'OFF', True)):
            cmake_case(cmake, work, family, option, callee, marker,
                       profile=profile, spans=spans, selected='ON', markers=(capable,),
                       error=('DEPRECATED', option))
        cmake_case(cmake, work, family, option, callee, marker,
                   selected='ON', after_include=True, error=('DEPRECATED', option))
        for profile in ('ENHANCED', 'REFERENCE'):
            for selected in (None, 'OFF'):
                cmake_case(cmake, work, family, option, callee, marker,
                           profile=profile, selected=selected, expected=(option + '=0',))

    family, option, callee, marker = ('precision_operations', 'PSX_PGXP_DEMAND_REGIONS',
                                    'psxrecomp_precision_operations', 'psx-pgxp-demand-regions-v2')
    for markers in ((False,), (True, False)):
        cmake_case(cmake, work, family, option, callee, marker,
                   selected='ON', markers=markers,
                   error=('Regenerate', 'PSX_PGXP_DEMAND_REGIONS'))
    cmake_case(cmake, work, family, option, callee, marker, selected='ON', markers=(True, True),
               expected=('PSX_PGXP_OPERATIONS=0', 'PSX_PGXP_DEMAND_REGIONS=1'))
    for selected, profile, spans, observation in (
            (None, 'ENHANCED', 'ON', 'ON'), ('OFF', 'ENHANCED', 'ON', 'ON'),
            ('ON', 'REFERENCE', 'ON', 'ON'), ('ON', 'ENHANCED', 'OFF', 'ON'),
            ('ON', 'ENHANCED', 'ON', 'OFF')):
        cmake_case(cmake, work, family, option, callee, marker, selected=selected,
                   profile=profile, spans=spans, observation=observation,
                   expected=('PSX_PGXP_OPERATIONS=0', 'PSX_PGXP_DEMAND_REGIONS=0'))
    print('PASS: all retired modes rejected; OFF/default/REFERENCE and current demand-v2 admission preserved')


ENV_SOURCE = r"""#include "pgxp_deprecation.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>
int main(int argc, char** argv) {
    if (argc != 3) return 10;
    bool rejected = false;
    try {
        if (!std::strcmp(argv[1], "operations"))
            PSXRecompV4::precision::reject_legacy_operations();
        else
            PSXRecompV4::precision::validate_legacy_environment();
    } catch (const std::logic_error& error) {
        if (!std::strstr(error.what(), "DEPRECATED") ||
            !std::strstr(error.what(), argv[2])) {
            std::fprintf(stderr, "wrong rejection: %s\n", error.what());
            return 11;
        }
        rejected = true;
    } catch (...) {
        return 12;
    }
    return rejected == (std::strcmp(argv[2], "accept") != 0) ? 0 : 13;
}
"""


def header_contracts(cc, cxx, work):
    include = ROOT / 'runtime/include'
    source = work / 'deprecation.c'
    source.write_text('#include "pgxp_deprecation.h"\n#include "pgxp_hooks.h"\n'
                      '#include "pgxp_operation.h"\nint main(void) { return 0; }\n', encoding='utf-8')
    base = [cc, '-std=c99', '-Werror', '-fsyntax-only', '-I', str(include), str(source)]
    for flags in ([], ['-D%s=0' % flag for flag in LEGACY] + ['-DPSX_PGXP_DEMAND_REGIONS=1']):
        result = subprocess.run([*base, *flags], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError('supported C header configuration failed\n' + result.stdout + result.stderr)
    for flag in LEGACY:
        result = subprocess.run([*base, '-D%s=1' % flag], capture_output=True, text=True)
        output = result.stdout + result.stderr
        if result.returncode == 0 or 'DEPRECATED' not in output or flag not in output:
            raise RuntimeError('legacy C option did not fail clearly\n' + output)
    cpp = work / 'environment.cpp'
    cpp.write_text(ENV_SOURCE, encoding='utf-8')
    exe = work / ('environment.exe' if os.name == 'nt' else 'environment')
    subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(include),
                    '-DPSX_PGXP_DEMAND_REGIONS=1', str(cpp), '-o', str(exe)], check=True)
    clean = dict(os.environ)
    for flag in LEGACY:
        clean.pop(flag, None)
    subprocess.run([str(exe), 'environment', 'accept'], env=clean, check=True)
    for value in ('', '0', 'FaLsE', 'OFF', 'nO'):
        subprocess.run([str(exe), 'environment', 'accept'],
                       env=dict(clean, **{flag: value for flag in LEGACY}), check=True)
    for flag in LEGACY:
        for value in ('1', 'TRUE', 'unsupported'):
            subprocess.run([str(exe), 'environment', flag],
                           env=dict(clean, **{flag: value}), check=True)
    subprocess.run([str(exe), 'operations', 'PSX_PGXP_OPERATIONS'], env=clean, check=True)
    print('PASS: actual C guards and C++ environment/logic_error contracts (current demand remains allowed)')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--cmake')
    parser.add_argument('--cc')
    parser.add_argument('--cxx')
    parser.add_argument('--contracts-only', action='store_true')
    args = parser.parse_args()
    if not args.contracts_only and not args.cmake:
        parser.error('--cmake is required for generation checks')
    if bool(args.cc) != bool(args.cxx) or (args.contracts_only and not args.cc):
        parser.error('header contracts require both --cc and --cxx')
    with tempfile.TemporaryDirectory(prefix='psx-pgxp-generation-') as folder:
        work = Path(folder)
        if not args.contracts_only:
            generation_checks(args.cmake, work)
        if args.cc:
            header_contracts(args.cc, args.cxx, work)


if __name__ == '__main__':
    main()
