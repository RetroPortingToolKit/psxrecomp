#ifndef PSX_PGXP_OPERATION_H
#define PSX_PGXP_OPERATION_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;
/* Borrowed only for one admission call. Read pointers are the canonical,
 * aligned containing word; write pointers prove the maintained store policy.
 * No value is cached for execution and no private/custom bus is admitted. */
typedef struct PSXPGXPOperationAccess {
    uint32_t address, kind; /* width1/2/4 | 0x100 for writes */
    const uint8_t *data;
} PSXPGXPOperationAccess;
int psx_pgxp_operation_begin(struct CPUState*, uint32_t entry_reads,
    const PSXPGXPOperationAccess*, uint32_t count, uint32_t alu, uint32_t loads, uint32_t stores);
void psx_pgxp_operation_end(struct CPUState*, uint32_t plain, uint32_t exact,
    uint32_t high, uint32_t low, uint32_t zero_halves);
/* The actual write owns its touched halves even if its integer bits are equal.
 * A following typed PGXP/native receipt may publish precision over this kill. */
void pgxp_write_owned(uint32_t address, uint32_t value, uint32_t width);
#ifdef __cplusplus
}
#endif
#endif
