/* Production write/watch/reference code, with private state visible only in
 * this fixture so generation wrap can be tested without billions of writes. */
#include "memory.c"
#include "psx_static_calls.h"
#include <assert.h>

/* Capture modules are outside this validity fixture, but the production
 * memory view still retains their correctly typed, inactive live state. */
uint32_t g_parity_trace_armed;
volatile int g_card_data_write_armed;

static uint8_t reference[0x4000];
static const uint32_t ranges[] = { 0x80010ff8u, 16u, 0x80012000u, 16u };
static const uint32_t other[] = { 0x80013000u, 16u };
static PsxStaticCodeValidity state;

static int valid(uint32_t pc) {
    return PSX_STATIC_CODE_VALID(&state, ranges, 2u, pc);
}
static uint64_t verifies(void) {
    PsxStaticCallStats stats;
    psx_static_call_get_stats(&stats);
    return stats.validity_verifies;
}

int main(int argc, char **argv) {
    (void)argv;
    memset(reference, 0x5a, sizeof reference);
    memcpy(ram + 0x10000, reference, sizeof reference);
    dirty_ram_register_text_image(0x10000u, reference, sizeof reference);
    if (argc > 1) {
        assert(!valid(0x80011000u)); /* PSX_FORCE_INTERP is latched per boot. */
        puts("force-interpreter remains authoritative");
        return 0;
    }
    /* Force the exact verifier to compare ranges, even though code is equal. */
    dirty_ram_mark_executable_range(0x10000u, sizeof reference);
    assert(valid(0x80011000u));
    const uint64_t cmp = dirty_ram_text_compare_calls();
    const uint64_t checks = verifies();
    for (unsigned i = 0; i < 1000u; ++i) assert(valid(0x80011000u));
#if PSX_NATIVE_STATIC_CALLS
    assert(dirty_ram_text_compare_calls() == cmp && verifies() == checks);
#else
    assert(dirty_ram_text_compare_calls() == cmp + 2000u && checks == 0u);
#endif
    /* A data-only write on a watched mixed code/data page revalidates exactly
     * once and can then reuse the positive result. */
    psx_host_write_byte(0x80011080u, 7u);
    assert(valid(0x80011000u));
    /* A continuation still verifies its prologue, including backward edges. */
    psx_host_write_byte(0xa0210ff8u, 9u); /* retail mirror + KSEG1 */
    assert(!valid(0x80011000u));
    psx_host_write_byte(0x80010ff8u, 0x5au);
    assert(valid(0x80011000u));
    /* Unaligned host stores can cross two watched pages. Only the second
     * page's byte belongs to this alternate cache owner. */
    const uint32_t second[] = {0x80011000u, 8u};
    PsxStaticCodeValidity cross = {0};
    assert(PSX_STATIC_CODE_VALID(&cross, second, 1u, 0));
    psx_host_write_word(0x80010ffeu, 0u);
    assert(!PSX_STATIC_CODE_VALID(&cross, second, 1u, 0));
    psx_host_write_word(0x80010ffeu, 0x5a5a5a5au);
    assert(valid(0));
    /* Bulk loaders and current-build state restore bypass individual stores. */
    ram[0x12000u] = 1;
    dirty_ram_mark_executable_range(0x12000u, 1u);
    assert(!valid(0));
    ram[0x12000u] = 0x5a;
    overlay_watch_invalidate_after_ram_restore();
    assert(valid(0));
    ram[0x12000u] = 2;
    dirty_ram_text_guard_resync_after_restore();
    assert(!valid(0));
    /* Intentional reference changes invalidate negative cached decisions. */
    dirty_ram_text_bless(0x12000u, ram + 0x12000u, 1u);
    assert(valid(0));
    /* A cache object cannot inherit a different function's positive result. */
    psx_host_write_byte(0x80013000u, 3u);
    assert(!PSX_STATIC_CODE_VALID(&state, other, 1u, 0));
    assert(valid(0));
    overlay_watch_clear_range(0x10000u, sizeof reference);
    assert(valid(0)); /* rearms watches */
    psx_host_write_byte(0x80012000u, 4u);
    assert(!valid(0));
    psx_host_write_byte(0x80012000u, 2u);
    assert(valid(0));
    /* Generation wrap cannot collide with an ancient cached witness. */
    overlay_page_gen[0x12] = UINT32_MAX;
    assert(valid(0));
    psx_host_write_byte(0x80012000u, 5u);
    assert(!valid(0));
    psx_host_write_byte(0x80012000u, 2u);
    assert(valid(0));
    /* Geometry and boot/reference epochs invalidate even otherwise equal keys. */
    g_psx_ram_mask = 0x7fffffu; g_psx_ram_size = 0x800000u;
    assert(valid(0));
    g_psx_ram_mask = 0x1fffffu; g_psx_ram_size = 0x200000u;
    dirty_ram_reset_for_boot();
    ram[0x12000u] = 6;
    dirty_ram_text_guard_resync_after_restore();
    assert(!valid(0));
    assert(!PSX_STATIC_CODE_VALID(&state, ranges, 0u, 0));
    const uint32_t bad[] = {0x801fffffu, 8u};
    assert(!PSX_STATIC_CODE_VALID(&state, bad, 1u, 0));
    puts("exact-range write validity, restore, mirror, owner and wrap checks passed");
    return 0;
}
