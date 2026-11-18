/* Generated-code timing policy. Include only AFTER the runtime declarations.
 * Native spans publish one approximate CPU charge per control-flow span;
 * values, stores, PGXP, interrupt checks and device/MMIO barriers stay live.
 * The interpreter and ordinary native services retain their original helpers.
 */
#ifndef PSX_NATIVE_SPAN_H
#define PSX_NATIVE_SPAN_H

#include "pgxp_operation.h"
#include "cpu_state.h"
#include "psx_memory.h"
#include "psx_direct_memory.h"

#if (defined(PSX_NATIVE_CPU_SPANS) && PSX_NATIVE_CPU_SPANS) || defined(PSX_OVERLAY_DLL_BUILD)
#if defined(PSX_COSIM)
#error Native CPU spans cannot be used by a faithful cycle oracle
#endif

static inline void psx_native_span_charge(uint32_t cycles) {
#ifdef PSX_OVERLAY_DLL_BUILD
    (void)cycles; /* DLL retains its existing instruction-charge shim. */
#else
    psx_cpu_charge(cycles);
#endif
}

/* Pure preview of this maintained charge, not another event prediction or
 * admission/read-set walk. Native observation charges only call out at the
 * sampling bound; device/IRQ delivery stays at the existing block edge. */
static inline int psx_native_span_charge_observable(uint32_t raw_cycles) {
#if defined(PSX_OBSERVATION_TIME) && PSX_OBSERVATION_TIME && !defined(PSX_OVERLAY_DLL_BUILD)
    if (g_ls_replay_active || g_psx_guest_time_frozen || g_event_step_conservative ||
        psx_in_device_service || g_ls_mode || g_ds_recording || g_psx_render_pass_active ||
        g_overlay_flush_pending_cycles || g_psx_cyc_batch || g_psx_cyc_local_acc)
        return 1;
    const uint32_t charge = g_psx_gcs_recip_q16 == 65536u ? raw_cycles :
        psx_gcs_scaled_charge(raw_cycles, g_psx_gcs_recip_q16, g_psx_gcs_frac);
    if (!charge) return 0;
    return charge > UINT64_MAX - psx_cycle_count ||
        psx_cycle_count + charge >= psx_next_cycle_sample;
#else
    (void)raw_cycles;
    return 1; /* Portable/reference/DLL uses its canonical charge owner. */
#endif
}

/* Direct RAM values are safe only while no read observer owns the access.
 * MMIO, instrumented reads, unusual segments and mirror-edge crossings use
 * the normal callback, which also publishes device work before the read.
 */
static inline const uint8_t *psx_native_span_ram(uint32_t addr, uint32_t width) {
    if(!width || width>4u)return NULL;
#ifdef PSX_OVERLAY_DLL_BUILD
    if(!psx_direct_native_enabled() || !psx_direct_load_owner_valid())return NULL;
#endif
    if (addr < 0xC0000000u && PSX_DM_VALUE(g_ls_mode) == 0 && !PSX_DM_VALUE(g_ls_replay_active) &&
        !PSX_DM_VALUE(g_ds_recording) && !PSX_DM_VALUE(g_ram_read_watch_active) && !PSX_DM_VALUE(g_dma_exec_depth)) {
        uint32_t phys = addr & 0x1FFFFFFFu;
        if (phys < 0x00800000u) {
            phys &= PSX_DM_VALUE(g_psx_ram_mask);
            if (phys <= PSX_DM_VALUE(g_psx_ram_mask) + 1u - width)
                return PSX_DM_VALUE(g_psx_ram) + phys;
        }
#if (defined(PSX_DIRECT_GUEST_MEMORY) && PSX_DIRECT_GUEST_MEMORY) || defined(PSX_OVERLAY_DLL_BUILD)
#ifdef PSX_OVERLAY_DLL_BUILD
        if(g_psx_overlay_memory_view->direct_memory)
#endif
        if (phys >= 0x1F800000u && phys <= 0x1F800400u - width)
            return PSX_DM_VALUE(g_psx_scratchpad) + (phys - 0x1F800000u);
#endif
    }
    return NULL;
}

/* A substituted reader owns both RAM and scratch values and effects. */
static inline const uint8_t *psx_native_span_load_view(CPUState *cpu,
                                                     uint32_t addr, uint32_t width) {
    if ((width==4u && cpu->read_word!=PSX_DM_CANONICAL(psx_read_word)) ||
        (width==2u && cpu->read_half!=PSX_DM_CANONICAL(psx_read_half)) ||
        (width==1u && cpu->read_byte!=PSX_DM_CANONICAL(psx_read_byte)))return NULL;
    return psx_native_span_ram(addr,width);
}

static inline void psx_native_span_note_load(uint32_t addr, const uint8_t *view, uint32_t width) {
#if !defined(PSX_COSIM)
    if (!view) { psx_direct_note_helper(addr, 0); return; }
    if ((addr & 0x1FFFFFFFu) < PSX_MAIN_RAM_WINDOW_BYTES) {
        ++PSX_DM_VALUE(g_psx_direct_memory_stats).load_inline_ram;
        psx_memory_note_inline_width(0u, PSX_MEM_RAM, width);
    } else {
        ++PSX_DM_VALUE(g_psx_direct_memory_stats).load_inline_scratch;
        psx_memory_note_inline_width(0u, PSX_MEM_SCRATCH, width);
    }
#else
    (void)addr; (void)view; (void)width;
#endif
}

/* The whole operation has no intermediate precision observation. Unknown
 * execution owners and instrumented policies retain canonical instruction hooks. */
static inline int psx_pgxp_operation_allowed(CPUState *cpu) {
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS && !defined(PSX_OVERLAY_DLL_BUILD)
    return cpu && !g_ls_mode && !g_ls_replay_active && !g_ds_recording &&
        !g_ram_read_watch_active && !g_dma_exec_depth && !g_host_store_depth &&
        !g_psx_render_pass_active && !psx_direct_pending_cycles() &&
        !(g_psx_memory_sr_ptr && (*g_psx_memory_sr_ptr & 0x10000u)) &&
        cpu->read_word==psx_read_word && cpu->read_half==psx_read_half && cpu->read_byte==psx_read_byte;
#else
    (void)cpu;return 0;
#endif
}

/* Admission only; no guest access/count/charge and no substituted callback.
 * Use the maintained native memory policy, including live observers/owners. */
static inline const uint8_t *psx_pgxp_operation_memory(CPUState *cpu,
                                                       uint32_t address, uint32_t kind) {
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS && !defined(PSX_OVERLAY_DLL_BUILD)
    const uint32_t width = kind & 0xffu;
    if(!psx_pgxp_operation_allowed(cpu))return NULL;
    if (!cpu || (width != 1u && width != 2u && width != 4u) || (address & (width - 1u))) return NULL;
    if (kind & 0x100u) {
        if ((width==4u && cpu->write_word!=psx_write_word) ||
            (width==2u && cpu->write_half!=psx_write_half) ||
            (width==1u && cpu->write_byte!=psx_write_byte)) return NULL;
        return psx_direct_store_view(address,width);
    }
    if (cpu->read_word!=psx_read_word || cpu->read_half!=psx_read_half || cpu->read_byte!=psx_read_byte) return NULL;
    return psx_native_span_load_view(cpu,address & ~3u,4u);
#else
    (void)cpu; (void)address; (void)kind; return NULL;
#endif
}

static inline uint32_t psx_native_span_word(CPUState *cpu, uint32_t addr,
                                           uint32_t rt, uint32_t deps) {
    const uint8_t *ram = psx_native_span_load_view(cpu, addr, 4u);
    (void)rt; (void)deps;
    psx_native_span_note_load(addr, ram, 4u);
    if (!ram) { psx_direct_callback_barrier(); PSX_HR_CUSTOM_BUS_CALLBACK(cpu->read_word,psx_read_word,0u,4u);
        return cpu->read_word(addr); }
    return (uint32_t)ram[0] | ((uint32_t)ram[1] << 8)
         | ((uint32_t)ram[2] << 16) | ((uint32_t)ram[3] << 24);
}
static inline uint16_t psx_native_span_half(CPUState *cpu, uint32_t addr,
                                           uint32_t rt, uint32_t deps) {
    const uint8_t *ram = psx_native_span_load_view(cpu, addr, 2u);
    (void)rt; (void)deps;
    psx_native_span_note_load(addr, ram, 2u);
    if (!ram) { psx_direct_callback_barrier(); PSX_HR_CUSTOM_BUS_CALLBACK(cpu->read_half,psx_read_half,0u,2u);
        return cpu->read_half(addr); }
    return (uint16_t)((uint16_t)ram[0] | ((uint16_t)ram[1] << 8));
}
static inline uint8_t psx_native_span_byte(CPUState *cpu, uint32_t addr,
                                          uint32_t rt, uint32_t deps) {
    const uint8_t *ram = psx_native_span_load_view(cpu, addr, 1u);
    (void)rt; (void)deps;
    psx_native_span_note_load(addr, ram, 1u);
    if (!ram) { psx_direct_callback_barrier(); PSX_HR_CUSTOM_BUS_CALLBACK(cpu->read_byte,psx_read_byte,0u,1u);
        return cpu->read_byte(addr); }
    return *ram;
}
static inline uint32_t psx_native_span_lwc2(CPUState *cpu, uint32_t addr) {
    return psx_native_span_word(cpu, addr, 0u, 0u);
}

/* Source-local aliases preserve the faithful generated text and leave the
 * reference/interpreter ABI untouched. This header never enters runtime TUs.
 */
#ifndef PSX_OVERLAY_DLL_BUILD
#define psx_cyc_step(cpu, deps) ((void)(cpu), (void)(deps))
#define psx_icache_fetch(cpu, pc) ((void)(cpu), (void)(pc))
#define psx_cyc_load_word psx_native_span_word
#define psx_cyc_load_half psx_native_span_half
#define psx_cyc_load_byte psx_native_span_byte
#define psx_cyc_lwc2_read psx_native_span_lwc2
#endif
#else
static inline void psx_native_span_charge(uint32_t cycles) { (void)cycles; }
#endif

#endif
