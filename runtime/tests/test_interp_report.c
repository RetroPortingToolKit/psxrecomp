/*
 * The run report's "interp_detail" object (PS1B-391).
 *
 * A release product's report gave one number for interpreted dispatches above
 * the kernel window. This object names the hottest interpreted addresses and
 * where they lie, the text guard's state and counters, and the overlay
 * loader's misses by reason. The builder is pure: this test hands it tables.
 *
 *   nothing ran        a valid object, guard not armed, an empty list, every reason 0;
 *   a full table       the 32 hottest, hottest first, the lower address first among
 *                      equals, each with its place; sums per place; empty slots ignored;
 *   guard and misses   the values it was given, every reason by name;
 *   game entry         (PS1B-513) the rows so far move to preload by place, keep their
 *                      address and lose their counts; by_place and hottest then hold
 *                      only what ran after entry; a second entry adds to the sums;
 *   a small buffer     -1 and no partial object; one byte more than the text fits.
 *
 * Build/run: ctest -R interp_report_test
 */
#include "interp_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); failures++; }
    else printf("ok: %s\n", message);
}

/* A strict enough JSON reader for this object: values, strings without escapes,
 * unsigned numbers, objects, arrays. Returns the position after the value, or NULL. */
static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++; return p; }
static const char *json_value(const char *p);
static const char *json_string(const char *p)
{
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; p++) if (*p == '\\' || (unsigned char)*p < 0x20) return NULL;
    return *p == '"' ? p + 1 : NULL;
}
static const char *json_value(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') return json_string(p);
    if (*p >= '0' && *p <= '9') { while (*p >= '0' && *p <= '9') p++; return p; }
    if (*p == '{' || *p == '[') {
        const char open = *p, close = (open == '{') ? '}' : ']';
        p = skip_ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (open == '{') {
                p = json_string(skip_ws(p));
                if (!p) return NULL;
                p = skip_ws(p);
                if (*p != ':') return NULL;
                p++;
            }
            p = json_value(p);
            if (!p) return NULL;
            p = skip_ws(p);
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    }
    return NULL;
}
static int json_valid(const char *text)
{
    const char *end = json_value(text);
    return end && *skip_ws(end) == '\0';
}

static int count_of(const char *text, const char *needle)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, needle)) != NULL; p += strlen(needle)) n++;
    return n;
}

static char s_json[64 * 1024];
static DirtyRamPcEntry s_table[512];

int main(void)
{
    PsxInterpReportInput in;

    /* --- nothing ran ------------------------------------------------------- */
    memset(&in, 0, sizeof in);
    in.kernel_end = 0x00010000u; in.text_lo = 0x00010000u; in.text_hi = 0x00098000u;
    int n = psx_interp_report_json(s_json, (int)sizeof s_json, &in);
    check(n > 0 && n == (int)strlen(s_json), "nothing ran: an object is written and its length returned");
    check(json_valid(s_json), "nothing ran: the object is valid JSON");
    check(strstr(s_json, "\"armed\": 0") != NULL, "nothing ran: the guard is not armed");
    check(strstr(s_json, "\"hottest\": []") != NULL, "nothing ran: the hottest list is empty");
    check(strstr(s_json, "\"addresses\": 0, \"by_place\"") != NULL, "nothing ran: no address counted");
    for (int r = 0; r < PSX_INTERP_MISS_REASONS; r++) {
        char key[64];
        snprintf(key, sizeof key, "\"%s\": 0", psx_interp_miss_reason_name(r));
        check(count_of(s_json, key) == 6, key);
    }
    check(strcmp(psx_interp_miss_reason_name(PSX_INTERP_MISS_REASONS), "unknown") == 0, "a reason past the list has no name");
    check(strstr(s_json, "\"preload\": {\"game_started\": 0, \"frame\": 0, \"addresses\": 0, \"by_place\": "
                         "{\"kernel\": {\"addresses\": 0, \"hits\": 0, \"entries\": 0}, "
                         "\"text\": {\"addresses\": 0, \"hits\": 0, \"entries\": 0}, "
                         "\"overlay\": {\"addresses\": 0, \"hits\": 0, \"entries\": 0}}, \"interpreter\": "
                         "{\"blocks_run\": 0, \"insns_run\": 0, \"aborts\": 0, \"guard_yields\": 0, \"native_handoffs\": 0}}, \"hottest_max\"") != NULL,
          "nothing ran: preload says the game was not entered and moved nothing");

    /* --- a full table -------------------------------------------------------- */
    memset(&in, 0, sizeof in);
    memset(s_table, 0, sizeof s_table);
    in.kernel_end = 0x00010000u; in.text_lo = 0x00010000u; in.text_hi = 0x00098000u;
    in.table = s_table; in.table_size = 512;
    /* 100 text addresses with hits 1..100, in a slot order that is not the hit order. */
    for (int i = 0; i < 100; i++) {
        DirtyRamPcEntry *e = &s_table[(i * 37) % 500];
        e->pc = 0x80020000u + (uint32_t)i * 4u;
        e->hits = (uint64_t)(i + 1);
        e->entry_hits = 1;
        e->insns = (uint64_t)(i + 1) * 10u;
    }
    /* Two addresses with the same count as the hottest text address: an overlay one
     * above it and a kernel one below it. The kernel address is lower, so it leads. */
    s_table[505].pc = 0x80123450u; s_table[505].hits = 100; s_table[505].entry_hits = 7;
    s_table[505].last_ext_ra = 0x80011111u; s_table[505].occ_crc = 0xABCD1234u; s_table[505].occ_ok = 1;
    s_table[506].pc = 0x00000CF0u; s_table[506].hits = 100; s_table[506].entry_hits = 3;
    /* An empty slot and a slot that was never hit do not count. */
    s_table[507].pc = 0; s_table[507].hits = 999;
    s_table[508].pc = 0x80030000u; s_table[508].hits = 0;
    n = psx_interp_report_json(s_json, (int)sizeof s_json, &in);
    check(n > 0 && json_valid(s_json), "a full table: valid JSON");
    check(count_of(s_json, "{\"pc\": ") == PSX_INTERP_REPORT_HOTTEST, "a full table: 32 entries listed");
    check(strstr(s_json, "\"addresses\": 102, \"by_place\"") != NULL, "a full table: 102 addresses counted");
    {
        const char *k = strstr(s_json, "{\"pc\": \"0x00000CF0\", \"place\": \"kernel\", \"hits\": 100, \"entries\": 3");
        const char *t = strstr(s_json, "{\"pc\": \"0x8002018C\", \"place\": \"text\", \"hits\": 100, \"entries\": 1, \"insns\": 1000");
        const char *o = strstr(s_json, "{\"pc\": \"0x80123450\", \"place\": \"overlay\", \"hits\": 100, \"entries\": 7, "
                                       "\"insns\": 0, \"last_caller_ra\": \"0x80011111\", \"unit_crc\": \"0xABCD1234\", \"unit_valid\": 1}");
        check(k && t && o, "a full table: the three hottest carry their place and their fields");
        check(k && t && o && k < t && t < o, "a full table: equal counts are listed by address");
        check(k && k == strstr(s_json, "{\"pc\": "), "a full table: the hottest entry is first");
    }
    /* 32 listed = the three at 100, then 99 down to 71. 70 is the first one left out. */
    check(strstr(s_json, "\"pc\": \"0x80020118\", \"place\": \"text\", \"hits\": 71,") != NULL, "a full table: hits 71 is the last listed");
    check(strstr(s_json, "\"hits\": 70,") == NULL, "a full table: hits 70 is not listed");
    check(strstr(s_json, "\"kernel\": {\"addresses\": 1, \"hits\": 100, \"entries\": 3}") != NULL, "a full table: the kernel sum");
    check(strstr(s_json, "\"text\": {\"addresses\": 100, \"hits\": 5050, \"entries\": 100}") != NULL, "a full table: the text sum");
    check(strstr(s_json, "\"overlay\": {\"addresses\": 1, \"hits\": 100, \"entries\": 7}") != NULL, "a full table: the overlay sum");

    /* An address below a boot text that loads high is overlay RAM, not text. */
    in.text_lo = 0x00180000u; in.text_hi = 0x0018B000u;
    n = psx_interp_report_json(s_json, (int)sizeof s_json, &in);
    check(n > 0 && strstr(s_json, "{\"pc\": \"0x8002018C\", \"place\": \"overlay\"") != NULL,
          "a high-loading text: the RAM below it is overlay");

    /* --- guard and misses ------------------------------------------------------- */
    memset(&in, 0, sizeof in);
    in.guard_armed = 1; in.guard_lo = 0x00010000u; in.guard_hi = 0x000AE800u;
    in.native_blocked = 123456789012ull; in.diverged_pages = 3; in.exact_mismatches = 55;
    in.exact_last[0] = 0x00064D80u; in.exact_last[1] = 844; in.exact_last[2] = 0x00065070u;
    in.exact_last[3] = 0x21; in.exact_last[4] = 0x25;
    in.kernel_end = 0x00010000u; in.text_lo = 0x00010000u; in.text_hi = 0x000AE800u;
    in.blocks_run = 11; in.insns_run = 22; in.aborts = 33; in.guard_yields = 44; in.native_handoffs = 55;
    for (int r = 0; r < PSX_INTERP_MISS_REASONS; r++) { in.miss[0][r] = 1000u + (unsigned)r; in.miss[1][r] = 2000u + (unsigned)r; }
    in.preload.interpreter[0] = 1; in.preload.interpreter[1] = 2; in.preload.interpreter[2] = 3;
    in.preload.interpreter[3] = 4; in.preload.interpreter[4] = 5;
    in.preload.game_started = 1;
    for (int r = 0; r < PSX_INTERP_MISS_REASONS; r++) {
        in.preload.miss[0][r] = 100u + (unsigned)r;
        in.preload.miss[1][r] = 200u + (unsigned)r;
    }
    n = psx_interp_report_json(s_json, (int)sizeof s_json, &in);
    check(n > 0 && json_valid(s_json), "guard and misses: valid JSON");
    check(strstr(s_json, "\"text_guard\": {\"armed\": 1, \"lo\": \"0x00010000\", \"hi\": \"0x000AE800\", "
                         "\"native_blocked\": 123456789012, \"diverged_pages\": 3, \"exact_mismatches\": 55, "
                         "\"last_mismatch\": {\"range\": \"0x00064D80\", \"len\": 844, \"at\": \"0x00065070\", "
                         "\"live_byte\": 33, \"image_byte\": 37}}") != NULL, "guard and misses: the guard's fields");
    check(strstr(s_json, "\"interpreter\": {\"blocks_run\": 11, \"insns_run\": 22, \"aborts\": 33, \"guard_yields\": 44, "
                         "\"native_handoffs\": 55}") != NULL, "guard and misses: the interpreter totals");
    check(strstr(s_json, "\"interpreter_after_game_entry\": {\"blocks_run\": 10, \"insns_run\": 20, \"aborts\": 30, \"guard_yields\": 40, \"native_handoffs\": 50}") != NULL,
          "game entry counters: interpreter totals exclude preload");
    check(strstr(s_json, "\"loader_miss_after_game_entry\": {\"above_kernel\": {\"miss_cached\": 900, \"no_unit\": 900") != NULL &&
          strstr(s_json, "\"loader_miss_preload\": {\"above_kernel\": {\"miss_cached\": 100, \"no_unit\": 101") != NULL,
          "game entry counters: loader misses expose after-entry and preload values");
    check(strstr(s_json, "\"above_kernel\": {\"miss_cached\": 1000, \"no_unit\": 1001, \"stale_bytes\": 1002, "
                         "\"outside_window\": 1003, \"device_touch\": 1004, \"native_off\": 1005, \"diff_gate\": 1006, "
                         "\"rank\": 1007, \"bad_entry\": 1008, \"mod_entry_hook\": 1009}") != NULL, "guard and misses: the misses above the kernel");
    check(strstr(s_json, "\"kernel\": {\"miss_cached\": 2000, \"no_unit\": 2001,") != NULL, "guard and misses: the misses in the kernel window");

    /* Nonzero BIOS-only totals belong wholly to preload until game entry. */
    memset(&in, 0, sizeof in);
    in.blocks_run = 11; in.insns_run = 22; in.aborts = 3; in.guard_yields = 4; in.native_handoffs = 5;
    in.miss[0][PSX_MISS_NO_UNIT] = 17; in.miss[1][PSX_MISS_CACHED] = 19;
    n = psx_interp_report_json(s_json, (int)sizeof s_json, &in);
    check(n > 0 && json_valid(s_json), "no game entry: valid JSON with nonzero counters");
    check(strstr(s_json, "\"interpreter_after_game_entry\": {\"blocks_run\": 0, \"insns_run\": 0, \"aborts\": 0, \"guard_yields\": 0, \"native_handoffs\": 0}") != NULL,
          "no game entry: interpreter after-entry values are zero");
    check(strstr(s_json, "\"interpreter\": {\"blocks_run\": 11, \"insns_run\": 22, \"aborts\": 3, \"guard_yields\": 4, \"native_handoffs\": 5}}, \"hottest_max\"") != NULL,
          "no game entry: current interpreter totals are preload");
    check(strstr(s_json, "\"loader_miss_after_game_entry\": {\"above_kernel\": {\"miss_cached\": 0, \"no_unit\": 0") != NULL &&
          strstr(s_json, "\"loader_miss_preload\": {\"above_kernel\": {\"miss_cached\": 0, \"no_unit\": 17") != NULL,
          "no game entry: current loader misses are preload and after-entry is zero");

    /* The first snapshot wins, and bad data cannot wrap subtraction. */
    {
        PsxInterpPreload pre;
        uint64_t first_i[5] = {10, 20, 30, 40, 50}, later_i[5] = {100, 200, 300, 400, 500};
        uint64_t first_m[2][PSX_INTERP_MISS_REASONS] = {{0}}, later_m[2][PSX_INTERP_MISS_REASONS] = {{0}};
        memset(&pre, 0, sizeof pre);
        first_m[0][PSX_MISS_NO_UNIT] = 7; later_m[0][PSX_MISS_NO_UNIT] = 70;
        psx_interp_preload_counters(&pre, first_i, first_m);
        pre.game_started = 1;
        psx_interp_preload_counters(&pre, later_i, later_m);
        check(pre.interpreter[0] == 10 && pre.interpreter[4] == 50 && pre.miss[0][PSX_MISS_NO_UNIT] == 7,
              "repeated entry: the first counter snapshot is preserved");
        memset(&in, 0, sizeof in);
        in.preload = pre; in.blocks_run = 9; in.miss[0][PSX_MISS_NO_UNIT] = 6;
        n = psx_interp_report_json(s_json, (int)sizeof s_json, &in);
        check(n > 0 && strstr(s_json, "\"interpreter_after_game_entry\": {\"blocks_run\": 0,") != NULL &&
              strstr(s_json, "\"loader_miss_after_game_entry\": {\"above_kernel\": {\"miss_cached\": 0, \"no_unit\": 0") != NULL,
              "counter subtraction: a snapshot above the total clamps to zero");
        check(psx_interp_after_entry(25, 10, 1) == 15 && psx_interp_after_entry(9, 10, 1) == 0 &&
              psx_interp_after_entry(25, 10, 0) == 0,
              "counter subtraction: total equals preload plus after-entry without underflow");
    }

    /* --- game entry (PS1B-513) ------------------------------------------------------ */
    /* With a real BIOS intro the BIOS shell runs from RAM 0x30000 and up before the
     * game's boot executable is loaded, inside the game's text range. Game entry moves
     * those rows to preload; by_place and hottest then hold only what ran after it. */
    {
        PsxInterpPreload pre;
        memset(&in, 0, sizeof in);
        memset(&pre, 0, sizeof pre);
        memset(s_table, 0, sizeof s_table);
        in.kernel_end = 0x00010000u; in.text_lo = 0x00010000u; in.text_hi = 0x000E5800u;
        in.table = s_table; in.table_size = 512;
        /* Before entry: two BIOS shell blocks in the text range (the counts of the EU
         * BIOS run of Resident Evil 3), a kernel stub, a BIOS block above the text. */
        s_table[10].pc = 0x000597E8u; s_table[10].hits = 5133281; s_table[10].insns = 25666405;
        s_table[11].pc = 0x0005981Cu; s_table[11].hits = 5133281; s_table[11].insns = 25666405;
        s_table[11].entry_hits = 2;
        s_table[12].pc = 0x00000CF0u; s_table[12].hits = 40; s_table[12].entry_hits = 40;
        s_table[13].pc = 0x001F0000u; s_table[13].hits = 9; s_table[13].entry_hits = 1;
        s_table[13].occ_crc = 0x1234u; s_table[13].occ_ok = 1; s_table[13].last_ext_ra = 0x80001234u;
        s_table[14].pc = 0x80060000u;   /* inserted, never counted */
        psx_interp_preload_close(s_table, 512, in.kernel_end, in.text_lo, in.text_hi, 1234, &pre);
        check(pre.game_started == 1 && pre.frame == 1234, "game entry: the move carries its frame");
        check(pre.addresses[0] == 1 && pre.hits[0] == 40 && pre.entries[0] == 40,
              "game entry: the kernel row moves to the kernel sum");
        check(pre.addresses[1] == 2 && pre.hits[1] == 10266562 && pre.entries[1] == 2,
              "game entry: the BIOS rows in the text range move to the text sum");
        check(pre.addresses[2] == 1 && pre.hits[2] == 9 && pre.entries[2] == 1,
              "game entry: the row above the text moves to the overlay sum");
        check(s_table[10].pc == 0x000597E8u && s_table[11].pc == 0x0005981Cu && s_table[12].pc == 0x00000CF0u &&
              s_table[13].pc == 0x001F0000u && s_table[14].pc == 0x80060000u,
              "game entry: every row keeps its address, so no probe chain breaks");
        {
            int cleared = 1;
            for (int i = 10; i <= 14; i++)
                cleared &= s_table[i].hits == 0 && s_table[i].insns == 0 && s_table[i].entry_hits == 0 &&
                           s_table[i].occ_crc == 0 && s_table[i].occ_ok == 0 && s_table[i].last_ext_ra == 0;
            check(cleared, "game entry: each moved row's counts and caller fields are cleared");
        }
        /* After entry: the game's own blocks, and the kernel stub again in its old row. */
        s_table[20].pc = 0x80020000u; s_table[20].hits = 300; s_table[20].entry_hits = 30; s_table[20].insns = 3000;
        s_table[21].pc = 0x80180000u; s_table[21].hits = 50; s_table[21].entry_hits = 5;
        s_table[12].hits = 7; s_table[12].entry_hits = 7;
        in.preload = pre;
        n = psx_interp_report_json(s_json, (int)sizeof s_json, &in);
        check(n > 0 && json_valid(s_json), "game entry: valid JSON");
        check(strstr(s_json, "0x000597E8") == NULL && strstr(s_json, "0x0005981C") == NULL,
              "game entry: the BIOS blocks are not in the hottest list");
        check(strstr(s_json, "\"addresses\": 3, \"by_place\": {\"kernel\": {\"addresses\": 1, \"hits\": 7, \"entries\": 7}, "
                             "\"text\": {\"addresses\": 1, \"hits\": 300, \"entries\": 30}, "
                             "\"overlay\": {\"addresses\": 1, \"hits\": 50, \"entries\": 5}}") != NULL,
              "game entry: by_place counts only what ran after entry");
        check(strstr(s_json, "\"preload\": {\"game_started\": 1, \"frame\": 1234, \"addresses\": 4, \"by_place\": "
                             "{\"kernel\": {\"addresses\": 1, \"hits\": 40, \"entries\": 40}, "
                             "\"text\": {\"addresses\": 2, \"hits\": 10266562, \"entries\": 2}, "
                             "\"overlay\": {\"addresses\": 1, \"hits\": 9, \"entries\": 1}}, \"interpreter\": "
                             "{\"blocks_run\": 0, \"insns_run\": 0, \"aborts\": 0, \"guard_yields\": 0, \"native_handoffs\": 0}}, \"hottest_max\"") != NULL,
              "game entry: preload holds what ran before entry, by place");
        {
            const char *first = strstr(s_json, "{\"pc\": ");
            check(first && first == strstr(s_json, "{\"pc\": \"0x80020000\", \"place\": \"text\", \"hits\": 300, \"entries\": 30"),
                  "game entry: the game's own block leads the hottest list");
        }
        /* The game range armed again and entered again: the sums grow, the frame is the latest. */
        psx_interp_preload_close(s_table, 512, in.kernel_end, in.text_lo, in.text_hi, 5000, &pre);
        check(pre.frame == 5000 && pre.addresses[0] == 2 && pre.hits[0] == 47 && pre.addresses[1] == 3 &&
              pre.hits[1] == 10266862 && pre.addresses[2] == 2 && pre.hits[2] == 59,
              "a second entry: the sums grow and the frame is the latest");
        /* No table: marked, nothing moved; no sums: nothing happens. */
        {
            PsxInterpPreload none;
            memset(&none, 0, sizeof none);
            psx_interp_preload_close(NULL, 0, in.kernel_end, in.text_lo, in.text_hi, 77, &none);
            check(none.game_started == 1 && none.frame == 77 && none.addresses[0] + none.addresses[1] + none.addresses[2] == 0,
                  "an empty table: entry is marked and nothing moves");
            s_table[20].hits = 5;
            psx_interp_preload_close(s_table, 512, in.kernel_end, in.text_lo, in.text_hi, 77, NULL);
            check(s_table[20].hits == 5, "no sums given: the rows are left as they were");
        }
    }

    /* --- a small buffer ------------------------------------------------------------ */
    {
        static char small[64 * 1024];
        int full = psx_interp_report_json(s_json, (int)sizeof s_json, &in);
        memset(small, 'x', sizeof small);
        check(psx_interp_report_json(small, full, &in) == -1 && small[0] == '\0',
              "a buffer one byte short: -1 and no partial object");
        check(psx_interp_report_json(small, full + 1, &in) == full && strcmp(small, s_json) == 0,
              "a buffer that just fits: the same object");
        memset(small, 'x', sizeof small);
        check(psx_interp_report_json(small, 16, &in) == -1 && small[0] == '\0', "a tiny buffer: -1");
        check(psx_interp_report_json(NULL, 100, &in) == -1 && psx_interp_report_json(small, 0, &in) == -1 &&
              psx_interp_report_json(small, 100, NULL) == -1, "no buffer or no input: -1");
    }

    printf("%s %d\n", failures ? "FAILED:" : "passed, failures:", failures);
    return failures ? 1 : 0;
}
