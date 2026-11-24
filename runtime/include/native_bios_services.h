#pragma once
#include <stdint.h>

struct CPUState;

#ifdef __cplusplus
extern "C" {
#endif

/* Native-only guest RAM state, restored together with guest snapshots. */
#define PSX_NATIVE_EXCEPTION_JUMP_PTR 0x0000b000u
#define PSX_NATIVE_SIO_AUTO_ACK 0x0000b004u
#define PSX_NATIVE_TIMER_AUTO_ACK 0x0000b008u
#define PSX_NATIVE_EXCEPTION_STACK 0x8000dff0u
#define PSX_NATIVE_IRQ_CALLBACK_LINK 0x80000194u
#define PSX_NATIVE_EXCEPTION_ENTRY 0x00000190u

enum {
    PSX_NATIVE_BIOS_UNSUPPORTED = 0,
    PSX_NATIVE_BIOS_RETURN = 1,
    PSX_NATIVE_BIOS_TRANSFER = 2,
};
int psx_native_bios_service(struct CPUState *cpu, uint32_t vector, uint32_t fn);

#ifdef __cplusplus
}
#endif
