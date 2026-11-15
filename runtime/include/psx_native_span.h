/* Generated-code timing policy. Include only AFTER the runtime declarations.
 * Native spans publish one approximate CPU charge per control-flow span;
 * values, stores, PGXP, interrupt checks and device/MMIO barriers stay live.
 * The interpreter and ordinary native services retain their original helpers.
 */
#ifndef PSX_NATIVE_SPAN_H
#define PSX_NATIVE_SPAN_H

#include "cpu_state.h"
#include "psx_memory.h"
#include "psx_direct_memory.h"

#if defined(PSX_NATIVE_CPU_SPANS) && PSX_NATIVE_CPU_SPANS
#if defined(PSX_COSIM)
#error Native CPU spans cannot be used by a faithful cycle oracle
#endif

static inline void psx_native_span_charge(uint32_t cycles) {
    psx_cpu_charge(cycles);
}

/* Direct RAM values are safe only while no read observer owns the access.
 * MMIO, instrumented reads, unusual segments and mirror-edge crossings use
 * the normal callback, which also publishes device work before the read.
 */
static inline const uint8_t *psx_native_span_ram(uint32_t addr, uint32_t width) {
#if !defined(PSX_OVERLAY_DLL_BUILD)
    extern int g_ram_read_watch_active;
    extern int g_dma_exec_depth;
    if (addr < 0xC0000000u && g_ls_mode == 0 && !g_ls_replay_active &&
        !g_ds_recording && !g_ram_read_watch_active && !g_dma_exec_depth) {
        uint32_t phys = addr & 0x1FFFFFFFu;
        if (phys < 0x00800000u) {
            phys &= g_psx_ram_mask;
            if (phys <= g_psx_ram_mask + 1u - width)
                return g_psx_ram + phys;
        }
#if defined(PSX_DIRECT_GUEST_MEMORY) && PSX_DIRECT_GUEST_MEMORY
        if (phys >= 0x1F800000u && phys <= 0x1F800400u - width)
            return g_psx_scratchpad + (phys - 0x1F800000u);
#endif
    }
#else
    (void)addr; (void)width;
#endif
    return NULL;
}

static inline void psx_native_span_note_load(uint32_t addr, const uint8_t *view) {
#if !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM)
    if (!view) { psx_direct_note_helper(addr, 0); return; }
    if ((addr & 0x1FFFFFFFu) < PSX_MAIN_RAM_WINDOW_BYTES)
        ++g_psx_direct_memory_stats.load_inline_ram;
    else
        ++g_psx_direct_memory_stats.load_inline_scratch;
#else
    (void)addr; (void)view;
#endif
}

static inline uint32_t psx_native_span_word(CPUState *cpu, uint32_t addr,
                                           uint32_t rt, uint32_t deps) {
    const uint8_t *ram = psx_native_span_ram(addr, 4u);
    (void)rt; (void)deps;
    psx_native_span_note_load(addr, ram);
    if (!ram) return cpu->read_word(addr);
    return (uint32_t)ram[0] | ((uint32_t)ram[1] << 8)
         | ((uint32_t)ram[2] << 16) | ((uint32_t)ram[3] << 24);
}
static inline uint16_t psx_native_span_half(CPUState *cpu, uint32_t addr,
                                           uint32_t rt, uint32_t deps) {
    const uint8_t *ram = psx_native_span_ram(addr, 2u);
    (void)rt; (void)deps;
    psx_native_span_note_load(addr, ram);
    if (!ram) return cpu->read_half(addr);
    return (uint16_t)((uint16_t)ram[0] | ((uint16_t)ram[1] << 8));
}
static inline uint8_t psx_native_span_byte(CPUState *cpu, uint32_t addr,
                                          uint32_t rt, uint32_t deps) {
    const uint8_t *ram = psx_native_span_ram(addr, 1u);
    (void)rt; (void)deps;
    psx_native_span_note_load(addr, ram);
    return ram ? *ram : cpu->read_byte(addr);
}
static inline uint32_t psx_native_span_lwc2(CPUState *cpu, uint32_t addr) {
    return psx_native_span_word(cpu, addr, 0u, 0u);
}

/* Source-local aliases preserve the faithful generated text and leave the
 * reference/interpreter ABI untouched. This header never enters runtime TUs.
 */
#define psx_cyc_step(cpu, deps) ((void)(cpu), (void)(deps))
#define psx_icache_fetch(cpu, pc) ((void)(cpu), (void)(pc))
#define psx_cyc_load_word psx_native_span_word
#define psx_cyc_load_half psx_native_span_half
#define psx_cyc_load_byte psx_native_span_byte
#define psx_cyc_lwc2_read psx_native_span_lwc2
#else
static inline void psx_native_span_charge(uint32_t cycles) { (void)cycles; }
#endif

#endif
