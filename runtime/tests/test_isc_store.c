#include "psx_thread_local.h"
/* Cache-isolated (SR.IsC) CPU stores, end to end through the REAL memory.c
 * store paths and the REAL psx_icache.c tag model, against Beetle's
 * PS_CPU::WriteMemory IsC branch (mednafen/psx/cpu.cpp:482-512):
 *
 *   if(BIU & BIU_ENABLE_ICACHE_S1) {
 *     if(BIU & (TAG_TEST | INVALIDATE | LOCK))  -> rewrite the line's tag + valid bits
 *     else                                      -> write an I-cache data word
 *   }
 *   if((BIU & 0x081) == 0x080)                  -> write the scratchpad at addr & 0x3FF
 *
 * and nothing reaches the bus, the BIU register at 0xFFFE0130 included.
 *
 * Load-bearing claims (each fails on a memory.c that drops isolated stores):
 *  1. The boot FlushCache loop (BIU 0x804, sw zero to every line) invalidates
 *     a warm line: the next fetch pays a refill again (7 cycles at a line
 *     start) instead of hitting. Invalidate and lock mode do the same.
 *  2. Tag-test mode loads the valid bits from the stored byte lane: a line
 *     can be marked valid without a refill, and a byte/half store off lane 0
 *     loads no valid bits.
 *  3. Data mode (BIU 0x800) and a disabled I-cache leave the tags alone.
 *  4. With the D-cache on and lock mode off, the store lands in the
 *     scratchpad at addr & 0x3FF; RAM never changes.
 *  5. An isolated store to 0xFFFE0130 does not change the BIU.
 *  6. A DMA write is not a CPU store: it reaches RAM while IsC is set and
 *     leaves the tags alone. Neither is a host store (psx_host_write_*: mods,
 *     FMV skip, debug pokes), even one that would load valid bits.
 *  7. A non-isolated store never touches the I-cache.
 *  8. Lockstep replay does not move the shared tags. */
#include "cpu_state.h"
#include "psx_icache.h"
#include "psx_memory.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void     psx_write_word(uint32_t addr, uint32_t val);
extern void     psx_write_half(uint32_t addr, uint16_t val);
extern void     psx_write_byte(uint32_t addr, uint8_t val);
extern PSX_THREAD_LOCAL int      g_host_store_depth;
extern uint32_t psx_read_word(uint32_t addr);
extern uint8_t *memory_get_ram_ptr(void);
extern uint8_t *memory_get_scratchpad_ptr(void);
extern void     memory_set_sr_ptr(const uint32_t *p);
extern PSX_THREAD_LOCAL int      g_dma_exec_depth;
extern PSX_THREAD_LOCAL uint64_t psx_cycle_count;

/* BIU values the kernel and the tests use (Beetle cpu.cpp BIU_* bits). */
#define BIU_FLUSH_TAGS  0x804u    /* I-cache on + tag-test (boot / FlushCache) */
#define BIU_FLUSH_DATA  0x800u    /* I-cache on, no mode bit */
#define BIU_DEFAULT     0x1E988u  /* the kernel's run-time value */
#define SR_ISC          0x10000u

static uint32_t sr;
static CPUState cpu;
static int failures;

static void check(int cond, const char *label) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", label);
        failures++;
    }
}

static uint64_t fetch_cost(uint32_t pc) {
    uint64_t before = psx_cycle_count;
    psx_icache_fetch(&cpu, pc);
    return psx_cycle_count - before;
}

/* Refill the line holding pc and confirm it now hits. */
static void warm(uint32_t pc) {
    (void)fetch_cost(pc & ~0xFu);
    check(fetch_cost(pc) == 0u, "warm line hits");
}

static void set_biu(uint32_t v) {
    sr = 0;
    psx_write_word(0xFFFE0130u, v);
}

static uint32_t ram_word(uint32_t off) {
    uint32_t v;
    memcpy(&v, memory_get_ram_ptr() + off, sizeof v);
    return v;
}

static void flush_loop(uint32_t biu, uint32_t value) {
    set_biu(biu);
    sr = SR_ISC;
    for (uint32_t a = 0; a < 0x1000u; a += 0x10u) psx_write_word(a, value);
    sr = 0;
}

int main(void) {
    psx_ram_apply_size_request();
    memory_set_sr_ptr(&sr);
    psx_icache_reset();
    g_psx_icache_active = 1;

    /* 1. FlushCache invalidates; the stores never reach RAM. */
    memcpy(memory_get_ram_ptr() + 0x500u, "\x11\x22\x33\x44", 4);
    warm(0x80000500u);
    flush_loop(BIU_FLUSH_TAGS, 0u);
    check(fetch_cost(0x80000500u) == 7u, "tag-test flush: warm line refills (7)");
    check(fetch_cost(0x80000504u) == 0u, "tag-test flush: refilled line hits");
    check(ram_word(0x500u) == 0x44332211u, "isolated stores leave RAM alone");

    warm(0x80000510u);
    flush_loop(0x802u, 0xFFFFFFFFu);
    check(fetch_cost(0x80000510u) == 7u, "invalidate mode: no valid bits, refill");
    warm(0x80000520u);
    flush_loop(0x801u, 0xFFFFFFFFu);
    check(fetch_cost(0x80000520u) == 7u, "lock mode: no valid bits, refill");

    /* 2. Tag-test valid bits come from the stored byte lane. */
    psx_icache_reset();
    g_psx_icache_active = 1;
    set_biu(BIU_FLUSH_TAGS);
    sr = SR_ISC;
    psx_write_word(0x80000600u, 0x5u);            /* words 0 and 2 valid */
    sr = 0;
    check(g_psx_icache_tv[0x600u >> 2] == 0x80000600u, "tag word 0 valid");
    check(g_psx_icache_tv[(0x600u >> 2) + 1] == (0x80000604u | 2u), "tag word 1 invalid");
    check(fetch_cost(0x80000600u) == 0u, "valid bit set by store: hit");
    check(fetch_cost(0x80000608u) == 0u, "valid bit set by store: hit (word 2)");
    check(fetch_cost(0x80000604u) == 6u, "invalid word: refill from word 1 (6)");

    warm(0x80000700u);
    sr = SR_ISC;
    psx_write_byte(0x80000701u, 0xFFu);           /* (0xFF << 8) & 0xF == 0 */
    sr = 0;
    check(fetch_cost(0x80000700u) == 7u, "byte off lane 0: no valid bits");
    warm(0x80000740u);
    sr = SR_ISC;
    psx_write_half(0x80000742u, 0xFFFFu);         /* (0xFFFF << 16) & 0xF == 0 */
    sr = 0;
    check(fetch_cost(0x80000740u) == 7u, "half off lane 0: no valid bits");
    sr = SR_ISC;
    psx_write_byte(0x80000780u, 0x03u);           /* words 0 and 1 valid */
    sr = 0;
    check(fetch_cost(0x80000784u) == 0u, "byte lane 0 loads valid bits");

    /* 3. Data mode and a disabled I-cache leave the tags alone. */
    warm(0x80000800u);
    flush_loop(BIU_FLUSH_DATA, 0u);
    check(fetch_cost(0x80000800u) == 0u, "data mode keeps the tags");
    /* (The tag itself, not a fetch: fetch costs with the cache disabled are
     * the separate BIU bit 11 item, ACCURACY_BURNDOWN axis 4.) */
    warm(0x80000810u);
    flush_loop(0x004u, 0u);
    check(g_psx_icache_tv[0x810u >> 2] == 0x80000810u, "I-cache disabled keeps the tags");

    /* 4. D-cache on, lock off: the store lands in the scratchpad. */
    memset(memory_get_scratchpad_ptr(), 0, 1024);
    set_biu(BIU_DEFAULT);
    sr = SR_ISC;
    psx_write_word(0x00000104u, 0xDEADBEEFu);
    psx_write_byte(0xA0001203u, 0x7Eu);
    psx_write_half(0x1F800012u, 0xBEEFu);
    sr = 0;
    {
        const uint8_t *sp = memory_get_scratchpad_ptr();
        check(memcmp(sp + 0x104u, "\xEF\xBE\xAD\xDE", 4) == 0, "D-cache word -> scratchpad[addr & 0x3FF]");
        check(sp[0x203u] == 0x7Eu, "D-cache byte -> scratchpad[addr & 0x3FF]");
        check(sp[0x012u] == 0xEFu && sp[0x013u] == 0xBEu, "D-cache half -> scratchpad");
        check(ram_word(0x104u) == 0u, "D-cache store leaves RAM alone");
    }
    set_biu(BIU_DEFAULT | 0x1u);                  /* lock mode: no scratchpad write */
    sr = SR_ISC;
    psx_write_word(0x00000200u, 0x01020304u);
    sr = 0;
    check(memory_get_scratchpad_ptr()[0x200u] == 0u, "lock mode keeps the scratchpad");
    set_biu(BIU_FLUSH_TAGS);                      /* D-cache off */
    sr = SR_ISC;
    psx_write_word(0x00000208u, 0x01020304u);
    sr = 0;
    check(memory_get_scratchpad_ptr()[0x208u] == 0u, "D-cache off keeps the scratchpad");

    /* 5. An isolated store to the BIU register does not reach it. */
    set_biu(BIU_FLUSH_TAGS);
    sr = SR_ISC;
    psx_write_word(0xFFFE0130u, 0u);
    sr = 0;
    check(psx_read_word(0xFFFE0130u) == BIU_FLUSH_TAGS, "isolated store keeps the BIU");

    /* 6. DMA is not isolated. */
    warm(0x80000900u);
    set_biu(BIU_FLUSH_TAGS);
    sr = SR_ISC;
    g_dma_exec_depth = 1;
    psx_write_word(0x80000900u, 0x12345678u);
    g_dma_exec_depth = 0;
    sr = 0;
    check(ram_word(0x900u) == 0x12345678u, "DMA write reaches RAM under IsC");
    check(fetch_cost(0x80000900u) == 0u, "DMA write keeps the tags");

    /* ... nor is a host store. Tag-test mode with lane-0 value 0xF would mark
     * a cold line valid if the store were taken as isolated. */
    psx_icache_reset();
    g_psx_icache_active = 1;
    set_biu(BIU_FLUSH_TAGS);
    sr = SR_ISC;
    psx_host_write_half(0x80000944u, 0xBEEFu);
    psx_host_write_byte(0x80000946u, 0x5Au);
    psx_host_write_word(0x80000940u, 0x0000000Fu);  /* last: its tags stand */
    sr = 0;
    check(ram_word(0x940u) == 0x0000000Fu, "host word reaches RAM under IsC");
    check(ram_word(0x944u) == 0x005ABEEFu, "host half/byte reach RAM under IsC");
    check(fetch_cost(0x80000940u) == 7u, "host store loads no valid bits");
    check(g_host_store_depth == 0, "host store depth unwinds");

    /* 7. Without IsC a store is a bus store; the I-cache does not snoop. */
    warm(0x80000A00u);
    psx_write_word(0x80000A00u, 0u);
    check(ram_word(0xA00u) == 0u && fetch_cost(0x80000A00u) == 0u,
          "plain store: RAM written, tags kept");

    /* 8. Lockstep replay does not move the shared tags. */
    warm(0x80000B00u);
    g_ls_replay_active = 1;
    sr = SR_ISC;
    psx_write_word(0x00000B00u, 0u);
    sr = 0;
    g_ls_replay_active = 0;
    check(fetch_cost(0x80000B00u) == 0u, "lockstep replay keeps the tags");

    if (failures) {
        fprintf(stderr, "isc_store_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS: isolated stores follow Beetle's WriteMemory IsC branch");
    return 0;
}
