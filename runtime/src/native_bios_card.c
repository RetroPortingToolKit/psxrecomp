#include "cpu_state.h"
#include "native_bios_services.h"
#include "psx_cycles.h"
#include "interrupts.h"
#include "memcard.h"
#include "sio.h"
#include "psx_memory.h"

#include <stdio.h>
#include <stdlib.h>

enum {
    CARD_EVENT = 0xf0000011u,
    BACKUP_EVENT = 0xf4000001u,
    CARD_SUCCESS = 4,
    CARD_ERROR = 0x8000,
    CARD_NEW = 0x2000,
    CARD_REQ_DEVICE = 0,
    CARD_REQ_SECTOR = 4,
    CARD_REQ_BUFFER = 8,
    CARD_REQ_INVALID = 12,
    CARD_REQ_BACKUP_INFO = 16,
    CARD_REQ_STRIDE = 32,
};

static CPUState *bound_cpu;

static uint32_t card_request(unsigned port)
{
    return PSX_NATIVE_CARD_REQUEST + port * CARD_REQ_STRIDE;
}

static void cards_replaced(void)
{
    /* Replay installs its sandbox backing before the first frame.
     * Before InitCARD, zeroed status words are not pending requests and
     * must not change the power-on guest RAM or replay digest. */
    if (!bound_cpu || !bound_cpu->read_word(PSX_NATIVE_CARD_INIT)) return;
    for (unsigned port = 0; port < 2; ++port) {
        if (!(bound_cpu->read_word(PSX_NATIVE_CARD_STATUS + port * 4) & 1u))
            bound_cpu->write_word(card_request(port) + CARD_REQ_INVALID, 1);
        bound_cpu->write_word(PSX_NATIVE_CARD_VOLUME_VALID + port * 4, 0);
    }
}

void psx_native_card_bind(CPUState *cpu)
{
    bound_cpu = cpu;
    sio_set_cards_replaced_hook(cpu ? cards_replaced : NULL);
}

static int card_checksum_valid(const uint8_t *data)
{
    uint8_t checksum = 0;
    for (unsigned i = 0; i < MEMCARD_SECTOR_SIZE - 1; ++i)
        checksum ^= data[i];
    return checksum == data[MEMCARD_SECTOR_SIZE - 1];
}

static int write_card_sector(unsigned port, unsigned sector, const uint8_t *data)
{
    if (memcard_write_sector(port, sector, data) != 0) return 0;
    memcard_flush(port);
    if (memcard_last_flush_result(port) != 0) return 0;
    sio_card_mark_written(port);
    return 1;
}

/* The backup unit's scan operates on the live card directory rather than
 * retaining a second, stale copy of it. Preserve its successful header
 * write-test (sector 63), new-card state, and directory checksum checks. */
static int initialize_volume(unsigned port)
{
    uint8_t data[MEMCARD_SECTOR_SIZE];
    if (memcard_read_sector(port, 0, data) != 0 ||
        data[0] != 'M' || data[1] != 'C') return 0;
    if (!write_card_sector(port, 63, data)) return 0;
    for (unsigned sector = 1; sector <= 35; ++sector) {
        if (memcard_read_sector(port, sector, data) != 0 ||
            !card_checksum_valid(data)) return 0;
    }
    return 1;
}

static uint32_t enqueue(CPUState *cpu, uint32_t status, int backup_info)
{
    uint32_t device = cpu->gpr[4];
    if (device >= 32) return 0;
    unsigned port = device >> 4;
    uint32_t address = card_request(port);
    if (!(cpu->read_word(PSX_NATIVE_CARD_STATUS + port * 4) & 1u)) return 0;
    if (status && (cpu->gpr[5] >= MEMCARD_SECTORS ||
        !psx_ram_resolve(cpu->gpr[6], MEMCARD_SECTOR_SIZE, NULL))) return 0;
    cpu->write_word(address + CARD_REQ_DEVICE, device);
    cpu->write_word(address + CARD_REQ_SECTOR, status ? cpu->gpr[5] : 0);
    cpu->write_word(address + CARD_REQ_BUFFER, status ? cpu->gpr[6] : 0);
    cpu->write_word(address + CARD_REQ_INVALID, 0);
    cpu->write_word(address + CARD_REQ_BACKUP_INFO, backup_info);
    cpu->write_word(PSX_NATIVE_CARD_STATUS + port * 4, status);
    return 1;
}

static uint32_t complete_request(CPUState *cpu, unsigned port, uint32_t status)
{
    uint32_t request = card_request(port);
    uint32_t device = cpu->read_word(request + CARD_REQ_DEVICE);
    uint32_t sector = cpu->read_word(request + CARD_REQ_SECTOR);
    uint32_t buffer = cpu->read_word(request + CARD_REQ_BUFFER);
    if (cpu->read_word(request + CARD_REQ_INVALID) ||
        (device & 0xfu) || !memcard_is_present(port)) return CARD_ERROR;
    if ((sio_card_flag(port) & 8) &&
        !cpu->read_word(PSX_NATIVE_CARD_ALLOW_NEW)) return CARD_NEW;
    if (status == 0) return CARD_SUCCESS;
    if (!psx_ram_resolve(buffer, MEMCARD_SECTOR_SIZE, NULL)) return CARD_ERROR;
    uint8_t data[MEMCARD_SECTOR_SIZE];
    if (status == 2) {
        if (memcard_read_sector(port, sector, data) != 0) return CARD_ERROR;
        for (unsigned i = 0; i < sizeof data; ++i)
            cpu->write_byte(buffer + i, data[i]);
    } else if (status == 4) {
        for (unsigned i = 0; i < sizeof data; ++i)
            data[i] = cpu->read_byte(buffer + i);
        if (!write_card_sector(port, sector, data)) return CARD_ERROR;
    } else {
        return CARD_ERROR;
    }
    return CARD_SUCCESS;
}

/* Native card executive state is guest-resident, so snapshots and rewinds
 * retain the same initialization, VBlank registration and slot state. The
 * host card backing remains under the shared memcard runtime's ownership. */
int psx_native_card_service(CPUState *cpu, uint32_t vector, uint32_t fn)
{
    if (vector == 0xa0 && (fn == 0x55 || fn == 0x70)) {
        for (unsigned port = 0; port < 2; ++port)
            cpu->write_word(PSX_NATIVE_CARD_VOLUME_VALID + port * 4,
                            initialize_volume(port));
        cpu->write_word(PSX_NATIVE_CARD_ALLOW_NEW, 0);
        cpu->gpr[2] = 0;
        psx_cpu_charge(128);
        return PSX_NATIVE_BIOS_RETURN;
    }
    if (vector == 0xa0 && fn == 0xab) {
        cpu->gpr[2] = enqueue(cpu, 0, 1);
        psx_cpu_charge(32);
        return PSX_NATIVE_BIOS_RETURN;
    }
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
    case 0x4d: /* cardInfoInternal */
        cpu->gpr[2] = enqueue(cpu, 0, 0);
        break;
    case 0x4e: /* _card_write */
        cpu->gpr[2] = enqueue(cpu, 4, 0);
        break;
    case 0x4f: /* _card_read */
        cpu->gpr[2] = enqueue(cpu, 2, 0);
        break;
    case 0x58: /* _card_chan */
        cpu->gpr[2] = cpu->read_word(PSX_NATIVE_CARD_LAST_DEVICE);
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
    static const uint32_t specs[] = {CARD_SUCCESS, CARD_ERROR, 0x100, 0x200, CARD_NEW};
    for (unsigned i = 0; i < sizeof specs / sizeof specs[0]; ++i)
        psx_native_bios_event(cpu, CARD_EVENT, specs[i], 0);
    unsigned port = 1 - (cpu->read_word(PSX_NATIVE_CARD_PORT) & 1u);
    cpu->write_word(PSX_NATIVE_CARD_PORT, port);
    uint32_t status_address = PSX_NATIVE_CARD_STATUS + port * 4;
    uint32_t status = cpu->read_word(status_address);
    if (status & 1u) return;

    uint32_t request = card_request(port);
    uint32_t spec = complete_request(cpu, port, status);
    int backup_info = cpu->read_word(request + CARD_REQ_BACKUP_INFO) != 0;
    cpu->write_word(PSX_NATIVE_CARD_LAST_DEVICE,
                    cpu->read_word(request + CARD_REQ_DEVICE));
    cpu->write_word(PSX_NATIVE_CARD_ALLOW_NEW, 0);
    cpu->write_word(status_address, spec == CARD_ERROR ? 0x21 : 1);
    cpu->write_word(request + CARD_REQ_BACKUP_INFO, 0);
    if (spec == CARD_NEW)
        cpu->write_word(PSX_NATIVE_CARD_VOLUME_VALID + port * 4, 0);
    if (backup_info) psx_native_bios_event(cpu, BACKUP_EVENT, spec, 1);
    psx_native_bios_event(cpu, CARD_EVENT, spec, 1);
}
