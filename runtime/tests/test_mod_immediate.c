/* [[mod_immediate.sites]] runtime store, selectors and the interpreter's word
 * substitution (mod_immediate.c): physical addresses, (address, word) lookup,
 * sorting, the cap and its log line, invalid entries dropped, empty and NULL
 * lists, selector range checks, the live gate and session reset, and
 * psx_mod_immediate_patch swapping exactly the imm16 of a listed word.
 * The interpreter call site is pinned by test_interp_mod_immediate_guards.py. */
#include "../src/mod_immediate.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#else
#include <unistd.h>
#endif

static int failures;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);              \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

#define SLTI_V0_V0_20   0x28420020u /* slti  v0, v0, 0x20   */
#define SLTIU_T2_A1_20  0x2CAA0020u /* sltiu t2, a1, 0x20   */
#define LBU_V0_V1_2E9   0x906202E9u /* lbu   v0, 0x2e9(v1)  */

static const char *s_capture_path = "mod_immediate_unit_stderr.txt";

static void capture_stderr(void (*fn)(void), char *buf, size_t cap) {
    FILE *tmp = fopen(s_capture_path, "w+b");
    buf[0] = '\0';
    if (!tmp) {
        CHECK(0, "cannot open the stderr capture file %s", s_capture_path);
        fn();
        return;
    }
    fflush(stderr);
    int saved = dup(fileno(stderr));
    dup2(fileno(tmp), fileno(stderr));
    fn();
    fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    rewind(tmp);
    size_t n = fread(buf, 1, cap - 1, tmp);
    buf[n] = '\0';
    fclose(tmp);
    remove(s_capture_path);
}

/* ---- store ----------------------------------------------------------------- */
#define BIG_N (PSX_MOD_IMMEDIATE_SITES_MAX + 44)
static PSXModImmediateSite big[BIG_N];
static void set_big(void) { (void)psx_mod_immediate_set_sites(big, BIG_N); }

static PSXModImmediateSite mk(uint32_t addr, uint32_t word, uint32_t group, uint32_t n,
                              uint16_t a, uint16_t b) {
    PSXModImmediateSite s;
    memset(&s, 0, sizeof s);
    s.address = addr; s.expected = word; s.group = group; s.count = n;
    s.values[0] = a; s.values[1] = b;
    return s;
}

static void test_store(void) {
    const PSXModImmediateSite sites[] = {
        mk(0x80020010u, SLTI_V0_V0_20, 1, 2, 0x40, 0x80),
        mk(0xA0020000u, LBU_V0_V1_2E9, 2, 1, 0x3E9, 0),    /* KSEG1 spelling */
        mk(0x00020010u, SLTIU_T2_A1_20, 1, 2, 0x40, 0x80), /* same address, other word */
        mk(0x80020020u, SLTI_V0_V0_20, 64, 1, 1, 0),       /* bad group: dropped */
        mk(0x80020030u, SLTI_V0_V0_20, 1, 0, 0, 0),        /* no values: dropped */
    };
    CHECK(psx_mod_immediate_set_sites(sites, 5) == 3, "three valid sites stored");
    CHECK(psx_mod_immediate_site_count() == 3, "count 3");
    CHECK(s_imm_sites[0].address == 0x00020000u && s_imm_sites[1].address == 0x00020010u &&
              s_imm_sites[2].address == 0x00020010u && s_imm_sites[1].expected < s_imm_sites[2].expected,
          "stored sorted by (physical address, word)");

    psx_mod_immediate_reset();
    (void)psx_mod_set_immediate_selector(1, 1);
    CHECK(psx_mod_immediate_patch(0x80020010u, SLTI_V0_V0_20) == 0x28420040u,
          "KSEG0 lookup, selector 1 -> 0x40");
    CHECK(psx_mod_immediate_patch(0x00020010u, SLTIU_T2_A1_20) == 0x2CAA0040u,
          "the other word at the shared address is its own site");
    CHECK(psx_mod_immediate_patch(0x80020010u, LBU_V0_V1_2E9) == LBU_V0_V1_2E9,
          "an unlisted word at a listed address is unchanged");
    CHECK(psx_mod_immediate_patch(0x80020000u, LBU_V0_V1_2E9) == LBU_V0_V1_2E9,
          "group 2 is still selector 0");
    (void)psx_mod_set_immediate_selector(2, 1);
    CHECK(psx_mod_immediate_patch(0x80020000u, LBU_V0_V1_2E9) == 0x906203E9u,
          "KSEG1 entry found through KSEG0, imm16 swapped");
    CHECK(psx_mod_immediate_patch(0x80020004u, LBU_V0_V1_2E9) == LBU_V0_V1_2E9,
          "between entries is unchanged");

    CHECK(psx_mod_immediate_set_sites(NULL, 5) == 0, "NULL list stores none");
    CHECK(psx_mod_immediate_set_sites(sites, -1) == 0, "negative count stores none");
    CHECK(psx_mod_immediate_patch(0x80020010u, SLTI_V0_V0_20) == SLTI_V0_V0_20,
          "empty store is unchanged");

    for (int i = 0; i < BIG_N; i++)
        big[i] = mk(0x80040000u + 4u * (uint32_t)i, SLTI_V0_V0_20, 3, 1, 0x40, 0);
    char log[512];
    capture_stderr(set_big, log, sizeof log);
    CHECK(psx_mod_immediate_site_count() == PSX_MOD_IMMEDIATE_SITES_MAX,
          "over-long list stored %d", psx_mod_immediate_site_count());
    CHECK(strstr(log, "keeping the first 4096") != NULL, "overflow not logged (got \"%s\")", log);
    (void)psx_mod_immediate_set_sites(NULL, 0);
}

/* ---- selectors ------------------------------------------------------------- */
static void test_selectors(void) {
    psx_mod_immediate_reset();
    CHECK(psx_mod_set_immediate_selector(1, 1) == 0, "no sites: the setter says so");
    CHECK(psx_mod_immediate_selector(1) == 1u, "the selector is still recorded");
    CHECK(!g_psx_mod_immediate_live, "no sites: interpreter gate stays off");
    const PSXModImmediateSite one = mk(0x80020000u, SLTI_V0_V0_20, 1, 2, 0x40, 0x80);
    (void)psx_mod_immediate_set_sites(&one, 1);
    CHECK(g_psx_mod_immediate_live, "sites with a nonzero selector: gate on");
    CHECK(psx_mod_set_immediate_selector(1, 0) == 1, "sites: the setter says so");
    CHECK(!g_psx_mod_immediate_live, "selector 0: gate off");
    CHECK(psx_mod_set_immediate_selector(64, 1) == 0 && psx_mod_set_immediate_selector(1, 16) == 0,
          "out-of-range group / selector refused");
    CHECK(psx_mod_immediate_select(1) == 0u && psx_mod_immediate_select(99) == 0u,
          "unset and out-of-range groups read 0");
    (void)psx_mod_set_immediate_selector(1, 2);
    CHECK(psx_mod_immediate_select(1) == 2u && g_psx_mod_immediate_live, "selector 2 live");
    psx_mod_immediate_reset();
    CHECK(psx_mod_immediate_select(1) == 0u && !g_psx_mod_immediate_live, "reset clears");
    /* A selector past the site's value count runs stock. */
    (void)psx_mod_set_immediate_selector(1, 5);
    CHECK(psx_mod_immediate_patch(0x80020000u, SLTI_V0_V0_20) == SLTI_V0_V0_20,
          "selector beyond the value count is stock");
    psx_mod_immediate_reset();
    (void)psx_mod_immediate_set_sites(NULL, 0);
}

int main(int argc, char **argv) {
    if (argc > 1) s_capture_path = argv[1];
    test_store();
    test_selectors();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS mod immediate store, selectors and patch");
    return 0;
}
