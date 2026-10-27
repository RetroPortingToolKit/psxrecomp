"""Build the real wait-policy selection and verify binding and execution identity."""
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
    with tempfile.TemporaryDirectory(prefix='psx-native-wait-') as temp:
        source = Path(temp)/'src'
        source.mkdir()
        (source/'main.c').write_text('''#include "psx_cycles.h"
#include "psx_native_idle_wait.h"
#include <stdio.h>
int g_idle_skip_enabled = 7, g_idle_skip_ext = 9;
static int audited_only = 7;
void psx_idle_skip_set_audited_only(int enabled) { audited_only = enabled; }
static int count = -1;
static uint32_t first, last;
void psx_idle_skip_set_store_counter_sites(const uint32_t *sites, int n) {
    count = n;
    if(n) { first=sites[0]; last=sites[n-1]; }
}
int main(void) {
    int selected=psx_build_idle_wait_configure();
    printf("%d %d %d %d %u %u %d\\n", selected, g_idle_skip_enabled, g_idle_skip_ext,
           count, first, last, audited_only);
    return 0;
}
''')
        (source/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.20)
project(wait_policy C)
set(PSXRECOMP_ROOT "{root.as_posix()}")
include("${{PSXRECOMP_ROOT}}/cmake/psx_execution_profile.cmake")
include("${{PSXRECOMP_ROOT}}/cmake/psx_native_idle_wait.cmake")
add_executable(fixture main.c)
target_include_directories(fixture PRIVATE "${{PSXRECOMP_ROOT}}/runtime/include")
psxrecomp_execution_profile(fixture)
psxrecomp_native_idle_wait(fixture ${{WAIT_SITES}})
''')

        def configure(name, profile, sites, ok=True):
            build = Path(temp)/name
            run([args.cmake, '-S', str(source), '-B', str(build), '-G', 'Ninja',
                 '-DCMAKE_C_COMPILER='+args.cc, '-DCMAKE_MAKE_PROGRAM='+args.ninja,
                 '-DPSX_EXECUTION_PROFILE='+profile, '-DWAIT_SITES='+sites], ok=ok)
            return build

        def result(build):
            run([args.cmake, '--build', str(build), '--parallel', '2'])
            exe = build/('fixture.exe' if (build/'fixture.exe').exists() else 'fixture')
            values = [int(x) for x in run([str(exe)]).split()]
            manifest = json.loads((build/'psx_execution/fixture/manifest.json').read_text())
            return values, manifest

        native, hle = result(configure('native', 'ENHANCED', '0x8002E6C0;0x8002E708'))
        reference, lle = result(configure('original', 'REFERENCE', '0x8002E6C0;0x8002E708'))
        assert native == [2,1,1,2,0x8002E6C0,0x8002E708,1], native
        assert reference == [0,0,0,0,0,0,0], reference
        assert hle['identity'] != lle['identity']
        assert hle['implementations'][0]['name'] == lle['implementations'][0]['name'] == 'idle-wait'
        untouched, unbound = result(configure('unbound', 'ENHANCED', ''))
        assert untouched == [-1,7,9,-1,0,0,7], untouched
        assert not unbound['implementations']
        _, changed = result(configure('different-anchor', 'ENHANCED', '0x8002E6C0'))
        assert changed['identity'] != hle['identity']
        for name, sites in [('unaligned','0x8002E6C1'), ('mmio','0x1F801810'),
                            ('overflow','0x18002E6C0'), ('invalid','surprise')]:
            configure(name, 'ENHANCED', sites, ok=False)
    print('PASS: native/reference/unbound behavior, identities, anchor changes and invalid bindings')


if __name__ == '__main__':
    main()
