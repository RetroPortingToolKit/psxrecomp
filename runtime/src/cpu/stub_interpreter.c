/* stub_interpreter.c — No-op stubs for native (non-oracle) build.
 *
 * The native build links this instead of psx_interpreter.c.
 * All functions are no-ops so oracle code can reference the
 * interpreter API without #ifdef guards.
 */

#include "psx_interpreter.h"

void     interp_init(CPUState* cpu)                   { (void)cpu; }
uint32_t interp_step(CPUState* cpu, uint32_t count)   { (void)cpu; (void)count; return 0; }
uint32_t interp_run(CPUState* cpu, uint32_t max)      { (void)cpu; (void)max; return 0; }
int      interp_hit_breakpoint(void)                   { return 0; }
int      interp_break_add(uint32_t pc)                 { (void)pc; return -1; }
int      interp_break_remove(uint32_t pc)              { (void)pc; return 0; }
void     interp_break_clear_all(void)                  { }
void     interp_trace_enable(int on)                   { (void)on; }
uint64_t interp_trace_count(void)                      { return 0; }
const InterpTraceEntry* interp_trace_get(uint32_t idx) { (void)idx; return 0; }
