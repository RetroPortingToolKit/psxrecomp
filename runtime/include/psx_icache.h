#ifndef PSXRECOMP_PSX_ICACHE_H
#define PSXRECOMP_PSX_ICACHE_H

#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

extern uint32_t g_psx_icache_tv[1024];
extern int g_psx_icache_active;
extern int g_ls_replay_active;
void psx_icache_reset(void);
void psx_icache_fetch(CPUState *cpu, uint32_t addr);
void psx_icache_fetch_miss(CPUState *cpu, uint32_t addr);

/* BIU / cache control (0xFFFE0130) bits the cache model reads (PSX-SPX
 * memorycontrol, "FFFE0130h - BCC, BIU/Cache Configuration Register"). */
#define PSX_BIU_ICACHE_ENABLE 0x800u  /* bit 11, IS1 */
#define PSX_BIU_DCACHE_ENABLE 0x080u  /* bit 7, DS */
#define PSX_BIU_TAG_TEST      0x004u  /* bit 2, TAG */
#define PSX_BIU_INVALIDATE    0x002u  /* bit 1, INV */
#define PSX_BIU_LOCK          0x001u  /* bit 0, LOCK */

/* A CPU store made while SR.IsC is set, applied to the I-cache model: the
 * I-cache half of Beetle PS_CPU::WriteMemory's IsC branch. With the I-cache
 * enabled and a tag-test, invalidate or lock mode bit set, the store rewrites
 * the tag and valid bits of the four words of its line; that is how FlushCache
 * (A 44h) invalidates the cache. `biu` is the current 0xFFFE0130 value and
 * `value` the stored register value (only its low byte lanes matter). */
void psx_icache_isc_store(uint32_t biu, uint32_t addr, uint32_t value);

/* Keep the interpreter's steady-state tag hit inside its translation unit.
 * Misses use the shared slow path, preserving exact cache evolution/timing. */
static inline void psx_icache_fetch_interp(CPUState *cpu, uint32_t addr) {
#ifdef PSX_ENABLE_BLOCK_CYCLES
    if (g_ls_replay_active) return;
    if (g_psx_icache_active < 0) {
        psx_icache_fetch(cpu, addr);
        return;
    }
    if (!g_psx_icache_active) return;
    uint32_t idx = (addr & 0xFFCu) >> 2;
    if (g_psx_icache_tv[idx] == addr) return;
    psx_icache_fetch_miss(cpu, addr);
#else
    (void)cpu;
    (void)addr;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
