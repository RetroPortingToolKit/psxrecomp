/* Guest services for seamless-loading adapters (mod_plugins.h). Each is the
 * one shared implementation of an effect several titles' adapters need; see
 * the header for the contract. */
#include "mod_plugins.h"
#include "cpu_state.h"
#include "dma.h"
#include "gpu.h"
#include "interrupts.h"
#include "psx_cycle_freeze.h"
#include "psx_memory.h"
#include "spu.h"
#include <string.h>

uint32_t psx_mod_call_guest(struct CPUState* cpu, uint32_t function,
                            uint32_t return_address, uint32_t a0, uint32_t a1,
                            uint32_t a2, uint32_t a3) {
    uint32_t regs[32];
    const uint32_t pc = cpu->pc, hi = cpu->hi, lo = cpu->lo;
    memcpy(regs, cpu->gpr, sizeof regs);
    cpu->gpr[4] = a0;
    cpu->gpr[5] = a1;
    cpu->gpr[6] = a2;
    cpu->gpr[7] = a3;
    cpu->gpr[31] = return_address;
    psx_snapshot_host_call_begin();
    psx_dispatch_call(cpu, function, return_address);
    const uint32_t result = cpu->gpr[2];
    memcpy(cpu->gpr, regs, sizeof regs);
    cpu->pc = pc;
    cpu->hi = hi;
    cpu->lo = lo;
    psx_snapshot_host_call_end();
    return result;
}

/* The caller's in-flight timing state: deadlines are absolute guest cycles
 * and the load pipeline describes the instructions around the call, so an
 * uncharged callee must leave them as they were. */
typedef struct CallerTiming {
    uint64_t muldiv_ts_done, gte_ts_done;
    uint8_t  read_absorb[33];
    uint8_t  read_absorb_which, read_fudge, ld_which_t;
    uint32_t ld_absorb;
} CallerTiming;

static void timing_save(CallerTiming* t, const struct CPUState* cpu) {
    t->muldiv_ts_done = cpu->muldiv_ts_done;
    t->gte_ts_done = cpu->gte_ts_done;
    memcpy(t->read_absorb, cpu->read_absorb, sizeof t->read_absorb);
    t->read_absorb_which = cpu->read_absorb_which;
    t->read_fudge = cpu->read_fudge;
    t->ld_which_t = cpu->ld_which_t;
    t->ld_absorb = cpu->ld_absorb;
}

static void timing_restore(struct CPUState* cpu, const CallerTiming* t) {
    cpu->muldiv_ts_done = t->muldiv_ts_done;
    cpu->gte_ts_done = t->gte_ts_done;
    memcpy(cpu->read_absorb, t->read_absorb, sizeof t->read_absorb);
    cpu->read_absorb_which = t->read_absorb_which;
    cpu->read_fudge = t->read_fudge;
    cpu->ld_which_t = t->ld_which_t;
    cpu->ld_absorb = t->ld_absorb;
}

uint32_t psx_mod_call_guest_uncharged(struct CPUState* cpu, uint32_t function,
                                      uint32_t return_address, uint32_t a0,
                                      uint32_t a1, uint32_t a2, uint32_t a3,
                                      uint32_t budget_cycles, int* charged) {
    PsxCycleFreeze save;
    CallerTiming timing;
    timing_save(&timing, cpu);
    const int frozen = psx_cycle_uncharged_begin(&save, budget_cycles);
    const uint32_t result =
        psx_mod_call_guest(cpu, function, return_address, a0, a1, a2, a3);
    const int uncharged = frozen ? psx_cycle_uncharged_end(&save) : 1;
    if (frozen && uncharged) timing_restore(cpu, &timing);
    if (charged) *charged = !uncharged;
    return result;
}

int psx_mod_dma_write_ram(uint32_t address, const void* data, uint32_t bytes,
                          int lba) {
    return dma_host_cdrom_write(address, (const uint8_t*)data, bytes, lba);
}

static int ram_span(uint32_t address, uint32_t bytes) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    const uint32_t live = psx_ram_live_bytes();
    return address < 0xC0000000u && phys < live && bytes <= live - phys;
}

int psx_mod_host_write_ram(uint32_t address, const void* data, uint32_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    if ((!p && bytes) || !ram_span(address, bytes)) return 0;
    while (bytes && (address & 3u)) { psx_host_write_byte(address++, *p++); --bytes; }
    for (; bytes >= 4; address += 4, p += 4, bytes -= 4) {
        uint32_t word;
        memcpy(&word, p, 4);
        psx_host_write_word(address, word);
    }
    while (bytes--) psx_host_write_byte(address++, *p++);
    return 1;
}

#define SPU_RAM_BYTES 0x80000u
#define SPU_TRANSFER_ADDR 0x1F801DA6u
#define SPU_CONTROL 0x1F801DAAu

int psx_mod_spu_sample_bank(uint32_t bank,const void *adpcm,uint32_t bytes) {
    return spu_register_sample_bank(bank,adpcm,bytes);
}
int psx_mod_spu_bind_voice_bank(unsigned voice,uint32_t bank) {
    return spu_bind_next_voice_bank(voice,bank);
}

int psx_mod_spu_upload(uint32_t spu_address, uint32_t guest_source,
                       uint32_t bytes, int stop_after) {
    if ((spu_address & 7u) || (guest_source & 3u) || (bytes & 3u) ||
        spu_address >= SPU_RAM_BYTES || bytes > SPU_RAM_BYTES ||
        !ram_span(guest_source, bytes))
        return 0;
    spu_write(SPU_TRANSFER_ADDR, spu_address >> 3);
    const uint16_t control = spu_ctrl_read();
    spu_write(SPU_CONTROL, (control & ~0x30u) | 0x20u);
    for (uint32_t i = 0; i < bytes; i += 4u) spu_dma_write(psx_mod_read_word(guest_source + i));
    if (stop_after) spu_write(SPU_CONTROL, control & ~0x30u);
    return 1;
}

int psx_mod_psyq_load_image(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t guest_source) {
    if (!w || w > 1024u || !h || h > 512u || x >= 1024u || y >= 512u ||
        (guest_source & 3u))
        return 0;
    const uint32_t words = (w * h + 1u) / 2u;
    if (!ram_span(guest_source, words * 4u)) return 0;
    gpu_set_gp0_source(guest_source);
    gpu_write_gp1(0x04000000u);
    gpu_write_gp0(0x01000000u);
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0(x | (y << 16));
    gpu_write_gp0(w | (h << 16));
    for (uint32_t i = 0; i < words; i++) {
        gpu_set_gp0_source(guest_source + 4u * i);
        gpu_write_gp0(psx_mod_read_word(guest_source + 4u * i));
    }
    if (words >= 16u) gpu_write_gp1(0x04000002u);
    return 1;
}
