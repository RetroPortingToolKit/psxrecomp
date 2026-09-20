"""Execute the trusted entry registry and its opt-in return contract."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    compiler = shutil.which('g++') or ('C:/msys64/mingw64/bin/g++.exe' if os.name == 'nt' else None)
    if not compiler:
        raise SystemExit('C++ compiler required')
    source = r'''
#include "cpu_state.h"
#include "mod_plugins.h"
#include <cassert>
static bool defer;
static unsigned observed;
static void observer(CPUState *, uint32_t) {
    assert(psx_mod_function_entry_active()); ++observed;
}
static int filter(CPUState *cpu, uint32_t address) {
    assert(address == 0x80020000u);
    assert(psx_mod_function_entry_active());
    if (!defer) return 0;
    cpu->gpr[2] = 1;
    return 1;
}
int main() {
    CPUState cpu = {};
    cpu.pc = 0x80020000u; cpu.gpr[31] = 0x80030000u; cpu.gpr[2] = 99;
    assert(!psx_mod_function_entry(&cpu, cpu.pc));
    assert(psx_mod_register_function_entry_plugin("observe", cpu.pc, observer));
    assert(psx_mod_register_function_filter_plugin("filter", cpu.pc, filter));
    assert(!psx_mod_register_function_filter_plugin("filter", cpu.pc, filter));
    assert(!psx_mod_function_entry(&cpu, cpu.pc));
    assert(observed == 1 && cpu.pc == 0x80020000u && cpu.gpr[2] == 99);
    defer = true;
    assert(psx_mod_function_entry(&cpu, cpu.pc));
    assert(observed == 2 && cpu.pc == cpu.gpr[31] && cpu.gpr[2] == 1);
    assert(!psx_mod_function_entry(&cpu, cpu.pc));
    assert(observed == 2);
    assert(!psx_mod_function_entry_active());
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp)
        test = path / 'test.cpp'
        test.write_text(source, encoding='utf-8')
        exe = path / ('test.exe' if os.name == 'nt' else 'test')
        subprocess.run([compiler, '-std=c++17', '-O1', '-flto', '-ffunction-sections',
                        '-fdata-sections', '-Wl,--gc-sections',
                        '-I' + str(ROOT / 'runtime/include'),
                        str(test), str(ROOT / 'runtime/src/mod_runtime.cpp'),
                        '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('mod function filters: registry, opt-in return and native fallthrough PASS')


if __name__ == '__main__':
    main()
