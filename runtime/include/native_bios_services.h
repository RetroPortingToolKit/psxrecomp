#pragma once
#include <stdint.h>

struct CPUState;

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PSX_NATIVE_BIOS_UNSUPPORTED = 0,
    PSX_NATIVE_BIOS_RETURN = 1,
    PSX_NATIVE_BIOS_TRANSFER = 2,
};
int psx_native_bios_service(struct CPUState *cpu, uint32_t vector, uint32_t fn);

#ifdef __cplusplus
}
#endif
