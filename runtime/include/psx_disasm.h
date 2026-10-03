#ifndef PSX_DISASM_H
#define PSX_DISASM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format one MIPS R3000 instruction as text into out (NUL-terminated).
 * addr is used to resolve branch/jump targets. Returns the number of
 * characters written (excluding the NUL), or 0 if out/cap are unusable. */
int psx_disasm_one(uint32_t word, uint32_t addr, char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_DISASM_H */
