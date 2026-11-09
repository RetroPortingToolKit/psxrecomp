/* interp_report.c — see interp_report.h. */
#include "interp_report.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *psx_interp_miss_reason_name(int reason)
{
    switch (reason) {
    case PSX_MISS_CACHED:         return "miss_cached";
    case PSX_MISS_NO_UNIT:        return "no_unit";
    case PSX_MISS_STALE_BYTES:    return "stale_bytes";
    case PSX_MISS_OUTSIDE_WINDOW: return "outside_window";
    case PSX_MISS_DEVICE_TOUCH:   return "device_touch";
    case PSX_MISS_NATIVE_OFF:     return "native_off";
    case PSX_MISS_DIFF_GATE:      return "diff_gate";
    case PSX_MISS_RANK:           return "rank";
    case PSX_MISS_BAD_ENTRY:      return "bad_entry";
    case PSX_MISS_MOD_ENTRY_HOOK: return "mod_entry_hook";
    default:                      return "unknown";
    }
}

/* Append; *n becomes -1 once the object does not fit, and stays there. */
static void put(char *out, int cap, int *n, const char *fmt, ...)
{
    if (*n < 0) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + *n, (size_t)(cap - *n), fmt, ap);
    va_end(ap);
    if (w < 0 || w >= cap - *n) *n = -1;
    else *n += w;
}

enum { WHERE_KERNEL = 0, WHERE_TEXT, WHERE_OVERLAY, WHERE_COUNT };
static const char *const k_where[WHERE_COUNT] = { "kernel", "text", "overlay" };
typedef char where_count_matches_header[(WHERE_COUNT == PSX_INTERP_PLACES) ? 1 : -1];

static int place_of(uint32_t kernel_end, uint32_t text_lo, uint32_t text_hi, uint32_t pc)
{
    uint32_t phys = pc & 0x1FFFFFFFu;
    if (phys < kernel_end) return WHERE_KERNEL;
    if (phys >= text_lo && phys < text_hi) return WHERE_TEXT;
    return WHERE_OVERLAY;
}

static int where_of(const PsxInterpReportInput *in, uint32_t pc)
{
    return place_of(in->kernel_end, in->text_lo, in->text_hi, pc);
}

uint64_t psx_interp_after_entry(uint64_t total, uint64_t preload, int game_started)
{
    if (!game_started) return 0;
    return total >= preload ? total - preload : 0;
}

void psx_interp_preload_counters(PsxInterpPreload *pre,
                                 const uint64_t interpreter[5],
                                 const uint64_t miss[2][PSX_INTERP_MISS_REASONS])
{
    if (!pre || pre->game_started) return;
    if (interpreter) memcpy(pre->interpreter, interpreter, sizeof(pre->interpreter));
    if (miss) memcpy(pre->miss, miss, sizeof(pre->miss));
}

void psx_interp_preload_close(DirtyRamPcEntry *table, uint32_t table_size,
                              uint32_t kernel_end, uint32_t text_lo, uint32_t text_hi,
                              uint64_t frame, PsxInterpPreload *pre)
{
    if (!pre) return;
    for (uint32_t i = 0; table && i < table_size; i++) {
        DirtyRamPcEntry *e = &table[i];
        if (e->pc == 0 || e->hits == 0) continue;
        int w = place_of(kernel_end, text_lo, text_hi, e->pc);
        pre->addresses[w]++;
        pre->hits[w] += e->hits;
        pre->entries[w] += e->entry_hits;
        e->hits = 0;
        e->insns = 0;
        e->entry_hits = 0;
        e->occ_crc = 0;
        e->occ_ok = 0;
        e->last_ext_ra = 0;
    }
    pre->game_started = 1;
    pre->frame = frame;
}

/* More hits first; the lower address first among equals, so the list does not
 * depend on the table's slot order. */
static int hotter(const DirtyRamPcEntry *a, const DirtyRamPcEntry *b)
{
    if (a->hits != b->hits) return a->hits > b->hits;
    return a->pc < b->pc;
}

int psx_interp_report_json(char *out, int cap, const PsxInterpReportInput *in)
{
    if (!out || cap <= 0 || !in) return -1;
    int n = 0;

    const DirtyRamPcEntry *top[PSX_INTERP_REPORT_HOTTEST];
    int top_n = 0;
    uint32_t used = 0;
    uint64_t hits[WHERE_COUNT] = {0}, entries[WHERE_COUNT] = {0};
    uint32_t addresses[WHERE_COUNT] = {0};
    for (uint32_t i = 0; in->table && i < in->table_size; i++) {
        const DirtyRamPcEntry *e = &in->table[i];
        if (e->pc == 0 || e->hits == 0) continue;
        used++;
        int w = where_of(in, e->pc);
        hits[w] += e->hits;
        entries[w] += e->entry_hits;
        addresses[w]++;
        if (top_n == PSX_INTERP_REPORT_HOTTEST && !hotter(e, top[top_n - 1])) continue;
        int at = (top_n < PSX_INTERP_REPORT_HOTTEST) ? top_n++ : top_n - 1;
        while (at > 0 && hotter(e, top[at - 1])) { top[at] = top[at - 1]; at--; }
        top[at] = e;
    }

    put(out, cap, &n,
        "{\n"
        "    \"text_guard\": {\"armed\": %d, \"lo\": \"0x%08X\", \"hi\": \"0x%08X\", "
        "\"native_blocked\": %llu, \"diverged_pages\": %u, "
        "\"exact_mismatches\": %llu, \"last_mismatch\": {\"range\": \"0x%08X\", \"len\": %u, "
        "\"at\": \"0x%08X\", \"live_byte\": %u, \"image_byte\": %u}},\n",
        in->guard_armed ? 1 : 0, (unsigned)in->guard_lo, (unsigned)in->guard_hi,
        (unsigned long long)in->native_blocked,
        (unsigned)in->diverged_pages, (unsigned long long)in->exact_mismatches,
        (unsigned)in->exact_last[0], (unsigned)in->exact_last[1], (unsigned)in->exact_last[2],
        (unsigned)in->exact_last[3], (unsigned)in->exact_last[4]);
    put(out, cap, &n,
        "    \"ranges\": {\"kernel_end\": \"0x%08X\", \"text_lo\": \"0x%08X\", \"text_hi\": \"0x%08X\"},\n"
        "    \"interpreter\": {\"blocks_run\": %llu, \"insns_run\": %llu, \"aborts\": %llu, "
        "\"guard_yields\": %llu, \"native_handoffs\": %llu},\n",
        (unsigned)in->kernel_end, (unsigned)in->text_lo, (unsigned)in->text_hi,
        (unsigned long long)in->blocks_run, (unsigned long long)in->insns_run,
        (unsigned long long)in->aborts, (unsigned long long)in->guard_yields,
        (unsigned long long)in->native_handoffs);
    put(out, cap, &n,
        "    \"interpreter_after_game_entry\": {\"blocks_run\": %llu, \"insns_run\": %llu, \"aborts\": %llu, "
        "\"guard_yields\": %llu, \"native_handoffs\": %llu},\n",
        (unsigned long long)psx_interp_after_entry(in->blocks_run, in->preload.interpreter[0], in->preload.game_started),
        (unsigned long long)psx_interp_after_entry(in->insns_run, in->preload.interpreter[1], in->preload.game_started),
        (unsigned long long)psx_interp_after_entry(in->aborts, in->preload.interpreter[2], in->preload.game_started),
        (unsigned long long)psx_interp_after_entry(in->guard_yields, in->preload.interpreter[3], in->preload.game_started),
        (unsigned long long)psx_interp_after_entry(in->native_handoffs, in->preload.interpreter[4], in->preload.game_started));

    put(out, cap, &n, "    \"per_address\": {\"table_size\": %u, \"addresses\": %u, \"by_place\": {",
        (unsigned)in->table_size, (unsigned)used);
    for (int w = 0; w < WHERE_COUNT; w++)
        put(out, cap, &n, "%s\"%s\": {\"addresses\": %u, \"hits\": %llu, \"entries\": %llu}",
            w ? ", " : "", k_where[w], (unsigned)addresses[w],
            (unsigned long long)hits[w], (unsigned long long)entries[w]);
    {
        const PsxInterpPreload *pre = &in->preload;
        uint32_t pre_used = 0;
        for (int w = 0; w < WHERE_COUNT; w++) pre_used += pre->addresses[w];
        put(out, cap, &n, "}, \"preload\": {\"game_started\": %d, \"frame\": %llu, \"addresses\": %u, \"by_place\": {",
            pre->game_started ? 1 : 0, (unsigned long long)pre->frame, (unsigned)pre_used);
        for (int w = 0; w < WHERE_COUNT; w++)
            put(out, cap, &n, "%s\"%s\": {\"addresses\": %u, \"hits\": %llu, \"entries\": %llu}",
                w ? ", " : "", k_where[w], (unsigned)pre->addresses[w],
                (unsigned long long)pre->hits[w], (unsigned long long)pre->entries[w]);
        put(out, cap, &n, "}, \"interpreter\": {\"blocks_run\": %llu, \"insns_run\": %llu, \"aborts\": %llu, "
            "\"guard_yields\": %llu, \"native_handoffs\": %llu}",
            (unsigned long long)(pre->game_started ? pre->interpreter[0] : in->blocks_run),
            (unsigned long long)(pre->game_started ? pre->interpreter[1] : in->insns_run),
            (unsigned long long)(pre->game_started ? pre->interpreter[2] : in->aborts),
            (unsigned long long)(pre->game_started ? pre->interpreter[3] : in->guard_yields),
            (unsigned long long)(pre->game_started ? pre->interpreter[4] : in->native_handoffs));
    }
    put(out, cap, &n, "}, \"hottest_max\": %d, \"hottest\": [", PSX_INTERP_REPORT_HOTTEST);
    for (int i = 0; i < top_n; i++) {
        const DirtyRamPcEntry *e = top[i];
        put(out, cap, &n,
            "%s\n      {\"pc\": \"0x%08X\", \"place\": \"%s\", \"hits\": %llu, \"entries\": %llu, "
            "\"insns\": %llu, \"last_caller_ra\": \"0x%08X\", \"unit_crc\": \"0x%08X\", \"unit_valid\": %u}",
            i ? "," : "", (unsigned)e->pc, k_where[where_of(in, e->pc)],
            (unsigned long long)e->hits, (unsigned long long)e->entry_hits,
            (unsigned long long)e->insns, (unsigned)e->last_ext_ra,
            (unsigned)e->occ_crc, (unsigned)e->occ_ok);
    }
    put(out, cap, &n, "%s]},\n", top_n ? "\n    " : "");

    put(out, cap, &n, "    \"loader_miss\": {");
    for (int place = 0; place < 2; place++) {
        put(out, cap, &n, "%s\"%s\": {", place ? ", " : "", place ? "kernel" : "above_kernel");
        for (int r = 0; r < PSX_INTERP_MISS_REASONS; r++)
            put(out, cap, &n, "%s\"%s\": %llu", r ? ", " : "", psx_interp_miss_reason_name(r),
                (unsigned long long)in->miss[place][r]);
        put(out, cap, &n, "}");
    }
    put(out, cap, &n, "},\n    \"loader_miss_after_game_entry\": {");
    for (int place = 0; place < 2; place++) {
        put(out, cap, &n, "%s\"%s\": {", place ? ", " : "", place ? "kernel" : "above_kernel");
        for (int r = 0; r < PSX_INTERP_MISS_REASONS; r++)
            put(out, cap, &n, "%s\"%s\": %llu", r ? ", " : "", psx_interp_miss_reason_name(r),
                (unsigned long long)psx_interp_after_entry(in->miss[place][r], in->preload.miss[place][r],
                                                           in->preload.game_started));
        put(out, cap, &n, "}");
    }
    put(out, cap, &n, "},\n    \"loader_miss_preload\": {");
    for (int place = 0; place < 2; place++) {
        put(out, cap, &n, "%s\"%s\": {", place ? ", " : "", place ? "kernel" : "above_kernel");
        for (int r = 0; r < PSX_INTERP_MISS_REASONS; r++)
            put(out, cap, &n, "%s\"%s\": %llu", r ? ", " : "", psx_interp_miss_reason_name(r),
                (unsigned long long)(in->preload.game_started ? in->preload.miss[place][r]
                                                              : in->miss[place][r]));
        put(out, cap, &n, "}");
    }
    put(out, cap, &n, "}\n  }");

    if (n < 0) { out[0] = '\0'; return -1; }
    return n;
}
