#include "cpu_state.h"
#include "native_bios_services.h"
#include "psx_cycles.h"
#include "interrupts.h"

#include <stdio.h>
#include <stdlib.h>

/* Native card executive state is guest-resident, so snapshots and rewinds
 * retain the same initialization, VBlank registration and slot state. The
 * host card backing remains under the shared memcard runtime's ownership. */
int psx_native_card_service(CPUState *cpu, uint32_t vector, uint32_t fn)
{
    if (vector != 0xb0) return PSX_NATIVE_BIOS_UNSUPPORTED;
    switch (fn) {
    case 0x4a: /* InitCARD */
        cpu->gpr[2] = cpu->read_word(PSX_NATIVE_CARD_INIT);
        cpu->write_word(PSX_NATIVE_CARD_INIT, 1);
        cpu->write_word(PSX_NATIVE_CARD_PAD_STARTED, cpu->gpr[4]);
        cpu->write_word(PSX_NATIVE_CARD_PORT, 0);
        cpu->write_word(PSX_NATIVE_CARD_STATUS, 1);
        cpu->write_word(PSX_NATIVE_CARD_STATUS + 4, 1);
        break;
    case 0x4b: /* StartCARD */
        /* Preserve device register state without the old select/delay loop. */
        cpu->write_half(0x1f80104au, 0x40);
        cpu->write_half(0x1f80104eu, 2073600 / 15200);
        cpu->write_half(0x1f801048u, 13);
        cpu->write_half(0x1f80104au, 0);
        cpu->write_word(PSX_NATIVE_CARD_ALLOW_NEW, 0);
        cpu->write_word(PSX_NATIVE_CARD_STARTED, 1);
        cpu->write_word(PSX_NATIVE_SIO_AUTO_ACK, 1);
        cpu->write_word(PSX_NATIVE_TIMER_AUTO_ACK + 3 * 4, 0);
        cpu->write_word(0x1f801074u, cpu->read_word(0x1f801074u) | (1u << IRQ_VBLANK));
        cpu->cop0[12] |= 0x401u;
        cpu->gpr[2] = 1;
        break;
    case 0x4c: /* StopCARD */
        cpu->write_word(PSX_NATIVE_CARD_STARTED, 0);
        cpu->write_word(PSX_NATIVE_TIMER_AUTO_ACK + 3 * 4, 1);
        cpu->cop0[12] |= 0x401u;
        cpu->gpr[2] = 1;
        break;
    case 0x50: /* _card_allowNew */
        cpu->write_word(PSX_NATIVE_CARD_ALLOW_NEW, 1);
        break;
    case 0x5c: /* card status: bit 0 means idle */
        if (cpu->gpr[4] > 1) return PSX_NATIVE_BIOS_UNSUPPORTED;
        cpu->gpr[2] = cpu->read_word(PSX_NATIVE_CARD_STATUS + cpu->gpr[4] * 4);
        break;
    default:
        return PSX_NATIVE_BIOS_UNSUPPORTED;
    }
    psx_cpu_charge(32);
    return PSX_NATIVE_BIOS_RETURN;
}

void psx_native_card_vblank(CPUState *cpu)
{
    if (!cpu->read_word(PSX_NATIVE_CARD_STARTED)) return;
    if (cpu->read_word(PSX_NATIVE_CARD_PAD_STARTED)) {
        fprintf(stderr, "native BIOS SIO provider gap: card VBlank requires active pad driver\n");
        exit(1);
    }
    if (cpu->read_word(PSX_NATIVE_SIO_AUTO_ACK))
        cpu->write_word(0x1f801070u, ~(1u << IRQ_VBLANK));
    cpu->write_word(PSX_NATIVE_CARD_PORT, 1 - cpu->read_word(PSX_NATIVE_CARD_PORT));
}
