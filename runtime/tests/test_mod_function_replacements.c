#include "cpu_state.h"
#include "mod_plugins.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
static int enabled, calls;
static int replace(CPUState *cpu, uint32_t address) {
    ++calls;
    if (!enabled)
        return 0;
    cpu->gpr[2] = address;
    return 1;
}
static int replace_with_ra_clobber(CPUState *cpu, uint32_t address) {
    cpu->gpr[31] = address;
    return 1;
}
int main(void) {
    CPUState cpu = {0}, before;
    cpu.gpr[31] = 0x80001234;
    cpu.pc = 0;
    before = cpu;
    assert(!psx_mod_try_function_replacement(&cpu, 0x80001000));
    assert(!memcmp(&before, &cpu, sizeof cpu));
    assert(!psx_mod_set_function_replacement(0, replace));
    assert(!psx_mod_set_function_replacement(0x80001001, replace));
    assert(psx_mod_set_function_replacement(0x80001000, replace));
    assert(!psx_mod_try_function_replacement(&cpu, 0x80001000));
    assert(calls == 1 && !memcmp(&before, &cpu, sizeof cpu));
    enabled = 1;
    assert(!psx_mod_try_function_replacement(&cpu, 0x80001004));
    assert(psx_mod_try_function_replacement(&cpu, 0x80001000));
    assert(cpu.pc == 0x80001234 && cpu.gpr[2] == 0x80001000 && cpu.gpr[31] == before.gpr[31]);
    assert(psx_mod_set_function_replacement(0x80001000, 0));
    assert(!psx_mod_try_function_replacement(&cpu, 0x80001000));
    assert(psx_mod_set_function_replacement(0x80001000, 0));
    assert(psx_mod_set_function_replacement(0x80001000, replace_with_ra_clobber));
    assert(!psx_mod_try_function_replacement(0, 0x80001000));
    cpu.gpr[31] = 0x80001234;
    assert(psx_mod_try_function_replacement(&cpu, 0x80001000));
    assert(cpu.pc == 0x80001234 && cpu.gpr[31] == 0x80001000);
    assert(psx_mod_set_function_replacement(0x80001000, 0));
    for (unsigned i = 0; i < 64; ++i)
        assert(psx_mod_set_function_replacement(0x80002000 + 4 * i, replace));
    assert(!psx_mod_set_function_replacement(0x80004000, replace));
    assert(psx_mod_set_function_replacement(0x80002000, 0));
    assert(psx_mod_set_function_replacement(0x80004000, replace));
    assert(psx_mod_try_function_replacement(&cpu, 0x800020FC));
    assert(psx_mod_try_function_replacement(&cpu, 0x80004000));
    for (unsigned i = 1; i < 64; ++i)
        assert(psx_mod_set_function_replacement(0x80002000 + 4 * i, 0));
    assert(psx_mod_set_function_replacement(0x80004000, 0));
    before = cpu;
    assert(!psx_mod_try_function_replacement(&cpu, 0x80004000));
    assert(!memcmp(&before, &cpu, sizeof cpu));
    puts("PASS native replacement opt-in, fallback, return ABI, unregister and capacity");
}
