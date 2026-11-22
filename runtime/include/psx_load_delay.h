#ifndef PSX_LOAD_DELAY_H
#define PSX_LOAD_DELAY_H
#include <stdint.h>
struct CPUState;
#ifdef __cplusplus
extern "C" {
#endif

/* Both compiled and interpreted instructions use the same checkpointed
 * pipeline window. The loaded GPR is published before any host observation;
 * begin exposes its previous value only during the actual successor opcode.
 * A cookie prevents a returning instruction from retiring another stream's
 * load after IRQ/thread handoff. A successor's own write cancels that load. */
void dirty_ram_ld_delay_stage(struct CPUState* cpu, uint32_t rt, uint32_t value,
                              uint32_t successor_pc);
uint64_t dirty_ram_ld_delay_begin(struct CPUState* cpu, uint32_t pc, uint32_t instruction);
void dirty_ram_ld_delay_end(struct CPUState* cpu, uint64_t cookie, uint32_t instruction);

#ifdef __cplusplus
}
#endif
#endif
