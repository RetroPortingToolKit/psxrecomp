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
 * Raw peeks and private callback internals are not. Substituted callback
 * dispatches have a separate width-only lane in headroom_counters.h.
 * This accounting is host-only and removed by the M0 overhead control. */
enum { PSX_MEM_RAM, PSX_MEM_SCRATCH, PSX_MEM_MMIO, PSX_MEM_OTHER };
enum { PSX_MEM_U8, PSX_MEM_U16, PSX_MEM_U32 };
typedef struct PSXMemoryWidthStats {
    uint64_t inline_access[2][2][3];
    uint64_t helper_access[2][4][3];
} PSXMemoryWidthStats;
extern PSXMemoryWidthStats g_psx_memory_width_stats;
void psx_memory_get_width_stats(PSXMemoryWidthStats *out);

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
extern int g_ram_read_watch_active;
extern int g_ls_mode, g_ls_replay_active;
extern volatile int g_ds_recording;
extern uint64_t g_guest_store_count;
extern int g_psx_track_last_store;
extern uint32_t g_guest_last_store_addr, g_guest_last_store_val;
extern uint32_t g_debug_last_store_pc;
extern void (*g_overlay_flush_pending_cycles)(void);

/* ABI-owned pointers to maintained storage/guards, never snapshots of mutable
 * observer state. The CPU execution owner mutates/reads these just as it does
 * on the static native path. No guest address escapes through this interface. */
typedef struct PSXDirectMemoryView {
    uint32_t bytes;
    int native_spans, direct_memory, direct_stores;
    uint8_t **g_psx_ram;
    uint8_t **g_psx_scratchpad;
    uint32_t *g_psx_ram_mask;
    uint32_t *g_psx_text_write_lo;
    uint32_t *g_psx_text_write_hi;
    const uint32_t **g_psx_memory_sr_ptr;
    uint32_t *g_parity_trace_armed;
    volatile int *g_card_data_write_armed;
    int *g_host_store_depth;
    int *g_dma_exec_depth;
    int *g_psx_render_pass_active;
    int *g_ls_mode;
    int *g_ls_replay_active;
    volatile int *g_ds_recording;
    int *g_ram_read_watch_active;
    uint64_t *g_guest_store_count;
    int *g_psx_track_last_store;
    uint32_t *g_guest_last_store_addr;
    uint32_t *g_guest_last_store_val;
    PSXDirectMemoryStats *g_psx_direct_memory_stats;
    uint32_t *g_psx_overlay_watch_bitmap, *g_dirty_ram_exec_page_bitmap;
    void (**g_overlay_flush_pending_cycles)(void);
    PSXMemoryWidthStats *width_stats; /* NULL in a counter-OFF host */
    /* ABI v39: the same pre-write passive history producer as canonical memory.
     * Required for DLL inline stores; NULL refuses direct-store admission. */
    void (*record_write)(uint32_t,uint32_t,uint32_t,uint8_t);
} PSXDirectMemoryView;
const PSXDirectMemoryView *psx_direct_memory_host_view(void);
#ifdef PSX_OVERLAY_DLL_BUILD
extern const PSXDirectMemoryView *g_psx_overlay_memory_view;
extern uint32_t *g_psx_overlay_pending_cycles_p;
extern uint32_t (*g_psx_overlay_psx_read_word)(uint32_t);
extern uint16_t (*g_psx_overlay_psx_read_half)(uint32_t);
extern uint8_t (*g_psx_overlay_psx_read_byte)(uint32_t);
extern void (*g_psx_overlay_psx_write_word)(uint32_t,uint32_t);
extern void (*g_psx_overlay_psx_write_half)(uint32_t,uint16_t);
extern void (*g_psx_overlay_psx_write_byte)(uint32_t,uint8_t);
#define PSX_DM_VALUE(name) (*g_psx_overlay_memory_view->name)
#define PSX_DM_ARRAY(name) (g_psx_overlay_memory_view->name)
#define PSX_DM_CANONICAL(name) (g_psx_overlay_##name)
static inline int psx_direct_native_enabled(void) {
    return g_psx_overlay_memory_view && g_psx_overlay_memory_view->native_spans;
}
static inline int psx_direct_load_owner_valid(void) {
    if(!g_psx_overlay_memory_view || !g_psx_overlay_pending_cycles_p)return 0;
    void (*owner)(void)=PSX_DM_VALUE(g_overlay_flush_pending_cycles);
    // Quiet native values need no observation boundary, but only this DLL
    // may carry a charge that the host can publish before an external effect.
    return owner==overlay_flush_cycles || (!owner && !*g_psx_overlay_pending_cycles_p);
}
static inline int psx_direct_pending_cycles(void) {
    if(!g_psx_overlay_memory_view || !g_psx_overlay_pending_cycles_p)return 1;
    void (*owner)(void)=PSX_DM_VALUE(g_overlay_flush_pending_cycles);
    return *g_psx_overlay_pending_cycles_p || (owner && owner!=overlay_flush_cycles);
}
static inline void psx_direct_callback_barrier(void) {
    overlay_flush_cycles();
    /* Maintained module entry already publishes the previous owner. Still
     * fail safely when an unproved/foreign scope reaches a private callback:
     * that callback need not enter canonical memory to publish this owner. */
    if(g_psx_overlay_memory_view){
        void (*owner)(void)=PSX_DM_VALUE(g_overlay_flush_pending_cycles);
        if(owner && owner!=overlay_flush_cycles)owner();
    }
}
#else
#define PSX_DM_VALUE(name) (name)
#define PSX_DM_ARRAY(name) (name)
#define PSX_DM_CANONICAL(name) (name)
static inline int psx_direct_pending_cycles(void) {return g_overlay_flush_pending_cycles!=NULL;}
static inline void psx_direct_callback_barrier(void) {}
#endif

static inline void psx_memory_note_inline_width(unsigned store, unsigned region,
                                                uint32_t width) {
#ifdef PSX_OVERLAY_DLL_BUILD
    if(g_psx_overlay_memory_view && g_psx_overlay_memory_view->width_stats)
        PSX_HR_INC(g_psx_overlay_memory_view->width_stats->inline_access[store][region]
        [width == 4u ? PSX_MEM_U32 : width == 2u ? PSX_MEM_U16 : PSX_MEM_U8]);
#else
    PSX_HR_INC(g_psx_memory_width_stats.inline_access[store][region]
        [width == 4u ? PSX_MEM_U32 : width == 2u ? PSX_MEM_U16 : PSX_MEM_U8]);
#endif
    (void)store; (void)region; (void)width;
}
static inline void psx_memory_note_helper_width(uint32_t address, unsigned store,
                                                uint32_t width) {
#if !defined(PSX_OVERLAY_DLL_BUILD) && (!defined(PSX_DISABLE_HEADROOM_COUNTERS) || !PSX_DISABLE_HEADROOM_COUNTERS)
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


#if ((defined(PSX_NATIVE_CPU_SPANS) && PSX_NATIVE_CPU_SPANS) || defined(PSX_OVERLAY_DLL_BUILD)) && \
    !defined(PSX_COSIM) && \
    !defined(PSX_MEMORY_INTERNAL)

static inline void psx_direct_note_helper(uint32_t address, int store) {
#ifdef PSX_OVERLAY_DLL_BUILD
    if(!g_psx_overlay_memory_view)return;
#endif
    const uint32_t phys = address & 0x1FFFFFFFu;
    PSXDirectMemoryStats *s = &PSX_DM_VALUE(g_psx_direct_memory_stats);
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
#if (defined(PSX_DIRECT_GUEST_MEMORY) && PSX_DIRECT_GUEST_MEMORY) || defined(PSX_OVERLAY_DLL_BUILD)
#ifdef PSX_OVERLAY_DLL_BUILD
    if(!psx_direct_native_enabled() || !g_psx_overlay_memory_view->direct_stores ||
        !g_psx_overlay_memory_view->record_write)return NULL;
#endif
    if (address >= 0xC0000000u || PSX_DM_VALUE(g_ls_mode) || PSX_DM_VALUE(g_ls_replay_active) ||
        PSX_DM_VALUE(g_ds_recording) || PSX_DM_VALUE(g_psx_render_pass_active) || PSX_DM_VALUE(g_dma_exec_depth) ||
        PSX_DM_VALUE(g_host_store_depth) || psx_direct_pending_cycles() ||
        (PSX_DM_VALUE(g_psx_memory_sr_ptr) && (*PSX_DM_VALUE(g_psx_memory_sr_ptr) & 0x10000u)))
        return NULL;
    uint32_t phys = address & 0x1FFFFFFFu;
    if (phys < PSX_MAIN_RAM_WINDOW_BYTES) {
        phys &= PSX_DM_VALUE(g_psx_ram_mask);
        if (width > PSX_DM_VALUE(g_psx_ram_mask) + 1u - phys || phys < 0x10000u ||
            (phys & 0xFFFu) > 0x1000u - width ||
            PSX_DM_VALUE(g_parity_trace_armed) || PSX_DM_VALUE(g_card_data_write_armed) ||
            g_debug_last_store_pc == 0x00001CE4u)
            return NULL;
        const uint32_t page = phys >> 12;
        const uint32_t bit = UINT32_C(1) << (page & 31u);
        if ((PSX_DM_ARRAY(g_psx_overlay_watch_bitmap)[page >> 5] |
             PSX_DM_ARRAY(g_dirty_ram_exec_page_bitmap)[page >> 5]) & bit)
            return NULL;
        if (PSX_DM_VALUE(g_psx_text_write_hi) > PSX_DM_VALUE(g_psx_text_write_lo) &&
            page >= (PSX_DM_VALUE(g_psx_text_write_lo) >> 12) &&
            page <= ((PSX_DM_VALUE(g_psx_text_write_hi) - 1u) >> 12))
            return NULL;
        return PSX_DM_VALUE(g_psx_ram) + phys;
    }
    if (phys >= 0x1F800000u && phys <= 0x1F800400u - width) {
        return PSX_DM_VALUE(g_psx_scratchpad) + (phys - 0x1F800000u);
    }
#else
    (void)address; (void)width;
#endif
    return NULL;
}

static inline void psx_direct_note_store(uint32_t addr, uint32_t value, uint32_t width) {
    if ((addr & 0x1FFFFFFFu) < PSX_MAIN_RAM_WINDOW_BYTES) {
        ++PSX_DM_VALUE(g_psx_direct_memory_stats).store_inline_ram;
        psx_memory_note_inline_width(1u, PSX_MEM_RAM, width);
    } else {
        ++PSX_DM_VALUE(g_psx_direct_memory_stats).store_inline_scratch;
        psx_memory_note_inline_width(1u, PSX_MEM_SCRATCH, width);
    }
    ++PSX_DM_VALUE(g_guest_store_count);
    if (!PSX_DM_VALUE(g_psx_track_last_store)) return;
    PSX_DM_VALUE(g_guest_last_store_addr) = width == 4u ? addr : UINT32_MAX;
    if (width == 4u) PSX_DM_VALUE(g_guest_last_store_val) = value;
}

/* Shared late-probe history observes the same pre-mutation bytes and canonical physical
 * address as memory.c. Keep every existing trace/fingerprint/watch consumer;
 * only avoid the canonical bus/filter layers already excluded by store_view.
 * The maintained hook only updates recorders (and reads RAM/scratch stacks);
 * it never pumps commands, advances devices or restores/swaps memory. Keep
 * that contract when using the admitted pointer after observation.
 * Host and DLL stores in both builds use this same producer. Missing or
 * stale DLL view/callback admission retains the complete canonical fallback. */
#ifndef PSX_OVERLAY_DLL_BUILD
void debug_server_trace_write_check(uint32_t phys, uint32_t old_value,
                                   uint32_t new_value, uint8_t width);
#endif
static inline void psx_direct_observe_store(uint32_t addr, const uint8_t *p,
                                            uint32_t value, uint32_t width) {
    uint32_t phys = addr & 0x1FFFFFFFu;
    uint32_t old_value = p[0];
    if (width >= 2u) old_value |= (uint32_t)p[1] << 8;
    if (width == 4u) old_value |= (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    if (phys < PSX_MAIN_RAM_WINDOW_BYTES) phys &= PSX_DM_VALUE(g_psx_ram_mask);
#ifdef PSX_OVERLAY_DLL_BUILD
    g_psx_overlay_memory_view->record_write(phys, old_value, value, (uint8_t)width);
#else
    debug_server_trace_write_check(phys, old_value, value, (uint8_t)width);
#endif
}

static inline void psx_native_store_word(CPUState *cpu, uint32_t addr, uint32_t value) {
#ifdef PSX_OVERLAY_DLL_BUILD
    if(!psx_direct_native_enabled()){
        PSX_HR_CUSTOM_BUS_CALLBACK(cpu->write_word,psx_write_word,1u,4u);
        cpu->write_word(addr,value);return;
    }
#endif
    uint8_t *p = cpu->write_word == PSX_DM_CANONICAL(psx_write_word) ? psx_direct_store_view(addr, 4u) : NULL;
    if (!p) { psx_direct_callback_barrier(); psx_direct_note_helper(addr, 1);
        PSX_HR_CUSTOM_BUS_CALLBACK(cpu->write_word,psx_write_word,1u,4u);
        cpu->write_word(addr, value); return; }
    psx_direct_note_store(addr, value, 4u);
    psx_direct_observe_store(addr, p, value, 4u);
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static inline void psx_native_store_half(CPUState *cpu, uint32_t addr, uint16_t value) {
#ifdef PSX_OVERLAY_DLL_BUILD
    if(!psx_direct_native_enabled()){
        PSX_HR_CUSTOM_BUS_CALLBACK(cpu->write_half,psx_write_half,1u,2u);
        cpu->write_half(addr,value);return;
    }
#endif
    uint8_t *p = cpu->write_half == PSX_DM_CANONICAL(psx_write_half) ? psx_direct_store_view(addr, 2u) : NULL;
    if (!p) { psx_direct_callback_barrier(); psx_direct_note_helper(addr, 1);
        PSX_HR_CUSTOM_BUS_CALLBACK(cpu->write_half,psx_write_half,1u,2u);
        cpu->write_half(addr, value); return; }
    psx_direct_note_store(addr, value, 2u);
    psx_direct_observe_store(addr, p, value, 2u);
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
}
static inline void psx_native_store_byte(CPUState *cpu, uint32_t addr, uint8_t value) {
#ifdef PSX_OVERLAY_DLL_BUILD
    if(!psx_direct_native_enabled()){
        PSX_HR_CUSTOM_BUS_CALLBACK(cpu->write_byte,psx_write_byte,1u,1u);
        cpu->write_byte(addr,value);return;
    }
#endif
    uint8_t *p = cpu->write_byte == PSX_DM_CANONICAL(psx_write_byte) ? psx_direct_store_view(addr, 1u) : NULL;
    if (!p) { psx_direct_callback_barrier(); psx_direct_note_helper(addr, 1);
        PSX_HR_CUSTOM_BUS_CALLBACK(cpu->write_byte,psx_write_byte,1u,1u);
        cpu->write_byte(addr, value); return; }
    psx_direct_note_store(addr, value, 1u);
    psx_direct_observe_store(addr, p, value, 1u);
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
