#include "psx_static_calls.h"
#include "psx_memory.h"
#include "dirty_ram_interp.h"

static PsxStaticCallStats s_stats;

void psx_static_call_get_stats(PsxStaticCallStats *out) {
    if (out) *out = s_stats;
}

void psx_static_call_note_entry(int handled) {
    if (handled) ++s_stats.fast_entries;
    else ++s_stats.fast_misses;
}

int psx_static_code_valid(PsxStaticCodeValidity *state,
                          const uint32_t *ranges, uint32_t count, uint32_t pc) {
    ++s_stats.validity_queries;
    if (!state || !ranges || !count || count > 4096u) {
        ++s_stats.validity_verifies;
        return dirty_ram_text_native_ok_ranges_from(ranges, count, pc);
    }
    const int bound = state->ranges == ranges && state->range_count == count;
    const uint64_t revision = dirty_ram_text_validation_revision();
    uint64_t sum = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t offset;
        const uint32_t length = ranges[i * 2u + 1u];
        if (!psx_ram_resolve(ranges[i * 2u], length, &offset)) {
            ++s_stats.validity_verifies;
            return dirty_ram_text_native_ok_ranges_from(ranges, count, pc);
        }
        if (!bound || state->revision != revision || state->ram_mask != g_psx_ram_mask)
            overlay_watch_set_range(offset, length);
        sum += overlay_watch_pagegen_sum64(offset, length);
    }
    if (bound && state->revision == revision && state->ram_mask == g_psx_ram_mask &&
        state->page_sum == sum) {
        ++s_stats.validity_hits;
        return state->matches != 0;
    }
    ++s_stats.validity_verifies;
    const int matches = dirty_ram_text_native_ok_ranges_from(ranges, count, pc);
    state->ranges = ranges;
    state->range_count = count;
    state->revision = revision;
    state->page_sum = sum;
    state->ram_mask = g_psx_ram_mask;
    state->matches = matches != 0;
    return matches;
}
