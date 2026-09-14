#include "cpu_state.h"
#include "mod_plugins.h"

/* Bounded, allocation-free and emulation-thread-only. Nothing is registered
 * by default; only trusted linked game code may opt in. */
static struct {
    uint32_t address;
    PSXModFunctionReplacement callback;
} replacements[64];
static unsigned replacement_count;
int psx_mod_set_function_replacement(uint32_t address, PSXModFunctionReplacement callback) {
    unsigned i;
    if (!address || (address & 3))
        return 0;
    for (i = 0; i < replacement_count; ++i) {
        if (replacements[i].address == address) {
            if (callback)
                replacements[i].callback = callback;
            else {
                replacements[i] = replacements[--replacement_count];
                replacements[replacement_count].address = 0;
                replacements[replacement_count].callback = 0;
            }
            return 1;
        }
    }
    if (!callback)
        return 1;
    if (replacement_count == 64)
        return 0;
    replacements[replacement_count].address = address;
    replacements[replacement_count++].callback = callback;
    return 1;
}
int psx_mod_try_function_replacement(CPUState *cpu, uint32_t address) {
    unsigned i;
    /* No table scan in faithful/default-off games or builds. */
    if (!replacement_count || !cpu)
        return 0;
    for (i = 0; i < replacement_count; ++i)
        if (replacements[i].address == address) {
            const uint32_t return_pc = cpu->gpr[31];
            if (!replacements[i].callback(cpu, address))
                return 0;
            cpu->pc = return_pc;
            return 1;
        }
    return 0;
}
