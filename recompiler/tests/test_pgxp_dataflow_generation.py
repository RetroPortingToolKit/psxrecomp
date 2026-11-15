#!/usr/bin/env python3
"""A capable generation may contain no dead sites; old generation cannot opt in."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cmake', required=True)
    a = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='psx-pgxp-generation-') as folder:
        work = Path(folder)
        source = work/'body.c'
        for profile, native_spans, selected, marker, expected in (
            ('ENHANCED', 'ON', 'ON', False, False),
            ('ENHANCED', 'ON', 'ON', True, True),
            ('ENHANCED', 'ON', 'OFF', False, True),
            ('REFERENCE', 'ON', 'ON', False, True),
            ('ENHANCED', 'OFF', 'ON', False, True),
        ):
            source.write_text('/* psx-pgxp-dataflow-v1 */\n' if marker else '/* older generation */\n')
            driver = work/'check.cmake'
            driver.write_text('''cmake_minimum_required(VERSION 3.20)
set(PSXRECOMP_ROOT "@ROOT@")
set(CMAKE_CURRENT_BINARY_DIR "@WORK@")
set(PSX_NATIVE_CPU_SPANS @SPANS@)
set(PSX_PGXP_DATAFLOW @SELECTED@)
function(get_target_property output)
    set(${output} "@PROFILE@" PARENT_SCOPE)
endfunction()
function(target_compile_definitions)
    message(STATUS "selected: ${ARGV}")
endfunction()
function(psxrecomp_add_implementation)
endfunction()
include("@ROOT@/cmake/psx_pgxp_dataflow.cmake")
psxrecomp_pgxp_dataflow(runtime GENERATED_SOURCES "@SOURCE@")
'''.replace('@ROOT@', ROOT.as_posix()).replace('@WORK@', work.as_posix())
                .replace('@SOURCE@', source.as_posix()).replace('@SPANS@', native_spans)
                .replace('@SELECTED@', selected).replace('@PROFILE@', profile), encoding='utf-8')
            p = subprocess.run([a.cmake, '-P', str(driver)], capture_output=True, text=True)
            if (p.returncode == 0) != expected:
                raise RuntimeError(p.stdout+p.stderr)
            if expected:
                active = int(profile == 'ENHANCED' and native_spans == selected == 'ON')
                if 'PSX_PGXP_DATAFLOW='+str(active) not in p.stdout:
                    raise RuntimeError('incorrect profile selection\n'+p.stdout+p.stderr)
            elif 'Regenerate game sources' not in p.stderr:
                raise RuntimeError('stale generation did not fail for the intended reason\n'+p.stderr)
    print('PASS: precision generation guard (stale refused; capable zero-site accepted; fallback profiles full)')


if __name__ == '__main__':
    main()
