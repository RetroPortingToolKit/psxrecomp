"""Execute the actual ADDI/ADDIU bodies with isolated widescreen services."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/dirty_ram_interp.c').read_text(encoding='utf-8')
start = source.index('    case 0x08: /* ADDI rt, rs, simm')
end = source.index('    case 0x0A: /* SLTI */', start)
cases = source[start:end]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t gpr[32]; } CPUState;
static int margin;
static int psx_ws_activation_margin(void) { return margin; }
static int psx_ws_is_cull_bias_site(uint32_t pc) { return pc == 4; }
static int psx_ws_is_cull_bias_lower_site(uint32_t pc) { return pc == 8; }
static int psx_ws_angle_site(uint32_t pc, uint32_t insn, uint32_t *out) { return 0; }
static int psx_ws_is_signed_x_bound_site(uint32_t pc, uint32_t insn) { return 0; }
static int psx_ws_screen_x_bound(int imm) { assert(0); return imm; }
#define psx_pgxp_alu(...) ((void)0)
static int execute(CPUState *cpu, uint32_t pc, uint32_t insn) {
    unsigned rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
    int32_t simm = (int16_t)insn;
    switch (insn >> 26) {
@CASES@
    default: assert(0); return 1;
    }
}
int main(void) {
    const int margins[] = {0, 85, 138, 234};
    const uint32_t cameras[] = {0, 51, 1024, 0xffff, 0xffffffff};
    for (unsigned op = 8; op <= 9; op++)
    for (unsigned m = 0; m < 4; m++)
    for (unsigned c = 0; c < 5; c++)
    for (unsigned kind = 0; kind < 3; kind++) {
        CPUState cpu, expected;
        for (unsigned r = 0; r < 32; r++) cpu.gpr[r] = r * 7919;
        cpu.gpr[4] = cameras[c];
        expected = cpu;
        margin = margins[m];
        int imm = kind == 1 ? 368 : -48;
        expected.gpr[2] = cameras[c] + (uint32_t)imm;
        if (kind == 1) expected.gpr[2] += margin;
        if (kind == 2) expected.gpr[2] -= margin;
        assert(execute(&cpu, kind * 4, (op << 26) | (4 << 21) | (2 << 16) | (uint16_t)imm) == 0);
        assert(memcmp(&cpu, &expected, sizeof cpu) == 0);
    }
    return 0;
}
'''.replace('@CASES@', cases)
with tempfile.TemporaryDirectory(prefix='ws-activation-') as directory:
    path = Path(directory)
    (path / 'test.c').write_text(fixture, encoding='utf-8')
    subprocess.run([sys.argv[1], '-std=c99', str(path / 'test.c'), '-o', str(path / 'test.exe')], check=True)
    subprocess.run([str(path / 'test.exe')], check=True)
print('activation bias ADDI/ADDIU: 4:3 identity, both sides, unrelated sites and register preservation PASS')
