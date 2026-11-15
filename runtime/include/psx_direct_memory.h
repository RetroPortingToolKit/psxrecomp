#ifndef PSX_DIRECT_MEMORY_H
#define PSX_DIRECT_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include "cpu_state.h"
#include "psx_memory.h"
#include "headroom_counters.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generated-code accesses only. Helpers used by devices/host services keep
 * their existing counters and observer path. These totals are never state. */
typedef struct PSXDirectMemoryStats {
    uint64_t load_inline_ram, load_inline_scratch;
    uint64_t load_helper_ram, load_helper_scratch, load_helper_mmio, load_helper_other;
    uint64_t store_inline_ram, store_inline_scratch;
    uint64_t store_helper_ram, store_helper_scratch, store_helper_mmio, store_helper_other;
} PSXDirectMemoryStats;
extern PSXDirectMemoryStats g_psx_direct_memory_stats;
void psx_direct_memory_get_stats(PSXDirectMemoryStats *out);

/* M0 width counts, separate from the existing generated-site totals above.
 * access 0=load/1=store; region RAM/scratch/MMIO/other; width u8/u16/u32.
 * Inline and canonical helper entries are disjoint execution paths. A native
 * generated fallback is also a canonical entry when its callback is psx_*;
 * do not add the generated fallback totals to these helper counts.
 * Host/device/BIOS/interpreter calls through psx_read/write_* are included.
 * Raw peeks, private overlay callbacks and substituted bus readers are not.
 * This accounting is host-only and removed by the M0 overhead control. */
enum { PSX_MEM_RAM, PSX_MEM_SCRATCH, PSX_MEM_MMIO, PSX_MEM_OTHER };
enum { PSX_MEM_U8, PSX_MEM_U16, PSX_MEM_U32 };
typedef struct PSXMemoryWidthStats {
    uint64_t inline_access[2][2][3];
    uint64_t helper_access[2][4][3];
} PSXMemoryWidthStats;
extern PSXMemoryWidthStats g_psx_memory_width_stats;
void psx_memory_get_width_stats(PSXMemoryWidthStats *out);

static inline void psx_memory_note_inline_width(unsigned store, unsigned region,
                                                uint32_t width) {
    PSX_HR_INC(g_psx_memory_width_stats.inline_access[store][region]
        [width == 4u ? PSX_MEM_U32 : width == 2u ? PSX_MEM_U16 : PSX_MEM_U8]);
    (void)store; (void)region; (void)width;
}
static inline void psx_memory_note_helper_width(uint32_t address, unsigned store,
                                                uint32_t width) {
#if !defined(PSX_DISABLE_HEADROOM_COUNTERS) || !PSX_DISABLE_HEADROOM_COUNTERS
    const uint32_t phys = address & 0x1FFFFFFFu;
    unsigned region = PSX_MEM_OTHER;
    if (address < 0xC0000000u) {
        if (phys < PSX_MAIN_RAM_WINDOW_BYTES) region = PSX_MEM_RAM;
        else if (phys >= 0x1F800000u && phys < 0x1F800400u) region = PSX_MEM_SCRATCH;
        else if (phys >= 0x1F801000u && phys < 0x1F804000u) region = PSX_MEM_MMIO;
    }
    ++g_psx_memory_width_stats.helper_access[store][region]
        [width == 4u ? PSX_MEM_U32 : width == 2u ? PSX_MEM_U16 : PSX_MEM_U8];
#else
    (void)address; (void)store; (void)width;
#endif
}


/* Borrow the maintained observer state rather than a second page registry.
 * Registration, clear, restore and first execution keep using their original
 * owners. Every protected page goes through the complete canonical writer. */
extern uint8_t *g_psx_scratchpad;
uint32_t psx_read_word(uint32_t addr);
uint16_t psx_read_half(uint32_t addr);
uint8_t psx_read_byte(uint32_t addr);
void psx_write_word(uint32_t addr, uint32_t value);
void psx_write_half(uint32_t addr, uint16_t value);
void psx_write_byte(uint32_t addr, uint8_t value);
extern uint32_t g_psx_text_write_lo, g_psx_text_write_hi;
extern uint32_t g_psx_overlay_watch_bitmap[];
extern uint32_t g_dirty_ram_exec_page_bitmap[];
extern const uint32_t *g_psx_memory_sr_ptr;
extern uint32_t g_parity_trace_armed;
extern volatile int g_card_data_write_armed;
extern int g_host_store_depth, g_dma_exec_depth, g_psx_render_pass_active;
extern int g_ls_mode, g_ls_replay_active;
extern volatile int g_ds_recording;
extern uint64_t g_guest_store_count;
extern int g_psx_track_last_store;
extern uint32_t g_guest_last_store_addr, g_guest_last_store_val;
extern uint32_t g_debug_last_store_pc;
extern void (*g_overlay_flush_pending_cycles)(void);

#if defined(PSX_NATIVE_CPU_SPANS) && PSX_NATIVE_CPU_SPANS && \
    !defined(PSX_OVERLAY_DLL_BUILD) && !defined(PSX_COSIM) && \
    !defined(PSX_MEMORY_INTERNAL)

static inline void psx_direct_note_helper(uint32_t address, int store) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    PSXDirectMemoryStats *s = &g_psx_direct_memory_stats;
    if (address < 0xC0000000u && phys < PSX_MAIN_RAM_WINDOW_BYTES) {
        if (store) ++s->store_helper_ram; else ++s->load_helper_ram;
    } else if (address < 0xC0000000u && phys >= 0x1F800000u && phys < 0x1F800400u) {
        if (store) ++s->store_helper_scratch; else ++s->load_helper_scratch;
    } else if (address < 0xC0000000u && phys >= 0x1F801000u && phys < 0x1F804000u) {
        if (store) ++s->store_helper_mmio; else ++s->load_helper_mmio;
    } else {
        if (store) ++s->store_helper_other; else ++s->load_helper_other;
    }
}

static inline uint8_t *psx_direct_store_view(uint32_t address, uint32_t width) {
    if (!width || width > 4u) return NULL;
#if defined(PSX_DIRECT_GUEST_MEMORY) && PSX_DIRECT_GUEST_MEMORY && defined(PSX_NO_DEBUG_TOOLS)
    if (address >= 0xC0000000u || g_ls_mode || g_ls_replay_active ||
        g_ds_recording || g_psx_render_pass_active || g_dma_exec_depth ||
        g_host_store_depth || g_overlay_flush_pending_cycles ||
        (g_psx_memory_sr_ptr && (*g_psx_memory_sr_ptr & 0x10000u)))
        return NULL;
    uint32_t phys = address & 0x1FFFFFFFu;
    if (phys < PSX_MAIN_RAM_WINDOW_BYTES) {
        phys &= g_psx_ram_mask;
        if (width > g_psx_ram_mask + 1u - phys || phys < 0x10000u ||
            (phys & 0xFFFu) > 0x1000u - width ||
            g_parity_trace_armed || g_card_data_write_armed ||
            g_debug_last_store_pc == 0x00001CE4u)
            return NULL;
        const uint32_t page = phys >> 12;
        const uint32_t bit = UINT32_C(1) << (page & 31u);
        if ((g_psx_overlay_watch_bitmap[page >> 5] |
             g_dirty_ram_exec_page_bitmap[page >> 5]) & bit)
            return NULL;
        if (g_psx_text_write_hi > g_psx_text_write_lo &&
            page >= (g_psx_text_write_lo >> 12) &&
            page <= ((g_psx_text_write_hi - 1u) >> 12))
            return NULL;
        return g_psx_ram + phys;
    }
    if (phys >= 0x1F800000u && phys <= 0x1F800400u - width) {
        return g_psx_scratchpad + (phys - 0x1F800000u);
    }
#else
    (void)address; (void)width;
#endif
    return NULL;
}

static inline void psx_direct_note_store(uint32_t addr, uint32_t value, uint32_t width) {
    if ((addr & 0x1FFFFFFFu) < PSX_MAIN_RAM_WINDOW_BYTES) {
        ++g_psx_direct_memory_stats.store_inline_ram;
        psx_memory_note_inline_width(1u, PSX_MEM_RAM, width);
    } else {
        ++g_psx_direct_memory_stats.store_inline_scratch;
        psx_memory_note_inline_width(1u, PSX_MEM_SCRATCH, width);
    }
    ++g_guest_store_count;
    if (!g_psx_track_last_store) return;
    g_guest_last_store_addr = width == 4u ? addr : UINT32_MAX;
    if (width == 4u) g_guest_last_store_val = value;
}

static inline void psx_native_store_word(CPUState *cpu, uint32_t addr, uint32_t value) {
    uint8_t *p = cpu->write_word == psx_write_word ? psx_direct_store_view(addr, 4u) : NULL;
    if (!p) { psx_direct_note_helper(addr, 1); cpu->write_word(addr, value); return; }
    psx_direct_note_store(addr, value, 4u);
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static inline void psx_native_store_half(CPUState *cpu, uint32_t addr, uint16_t value) {
    uint8_t *p = cpu->write_half == psx_write_half ? psx_direct_store_view(addr, 2u) : NULL;
    if (!p) { psx_direct_note_helper(addr, 1); cpu->write_half(addr, value); return; }
    psx_direct_note_store(addr, value, 2u);
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static inline void psx_native_store_byte(CPUState *cpu, uint32_t addr, uint8_t value) {
    uint8_t *p = cpu->write_byte == psx_write_byte ? psx_direct_store_view(addr, 1u) : NULL;
    if (!p) { psx_direct_note_helper(addr, 1); cpu->write_byte(addr, value); return; }
    psx_direct_note_store(addr, value, 1u);
    p[0] = value;
}

#undef PSX_STORE_WORD
#undef PSX_STORE_HALF
#undef PSX_STORE_BYTE
#define PSX_STORE_WORD(cpu, addr, value) psx_native_store_word((cpu), (addr), (value))
#define PSX_STORE_HALF(cpu, addr, value) psx_native_store_half((cpu), (addr), (value))
#define PSX_STORE_BYTE(cpu, addr, value) psx_native_store_byte((cpu), (addr), (value))
#endif

#ifdef __cplusplus
}
#endif
#endif
