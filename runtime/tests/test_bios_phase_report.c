/*
 * The BIOS phase's interpreted dispatches (PS1B-105).
 *
 * With a real BIOS intro the European shell (SCPH-5552, no shell seeds) runs
 * in the dirty-RAM interpreter, and nothing on stdout said how much: the
 * overlay loader's disp_interp is 0 while the loader is inactive. One line now
 * gives the interpreter's per-address hits by place, at game entry or at exit.
 *
 *   places        each boundary of kernel, shell, shell_tail and other;
 *   a table       hits, addresses and lowest/highest address per place; empty
 *                 rows and rows without hits ignored; a KSEG0 key counts by its
 *                 physical address;
 *   the line      the exact text, the shell's ROM range, "none" without shell
 *                 rows, the line without counts, -1 and an empty string on a
 *                 small buffer;
 *   the hook      install sets fntrace's game-entry hook; the first entry prints
 *                 one line from the live table; a second entry prints nothing.
 *
 * Build/run: ctest -R bios_phase_report_test
 */
#include "bios_phase_report.h"
#include "fntrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime symbols the module reads, owned here. */
DirtyRamPcEntry g_dirty_ram_pc_table[DIRTY_RAM_PC_TABLE_SIZE];
uint64_t s_frame_count;
void (*g_fntrace_game_entry_hook)(void);
static int s_started;
int fntrace_is_game_started(void) { return s_started; }

static int failures;

static void check(int condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); failures++; }
    else printf("ok: %s\n", message);
}

static void row(DirtyRamPcEntry *t, uint32_t i, uint32_t pc, uint64_t hits)
{
    memset(&t[i], 0, sizeof(t[i]));
    t[i].pc = pc;
    t[i].hits = hits;
    t[i].entry_hits = hits;
}

static void test_places(void)
{
    check(bios_phase_place(0x00000500u) == BIOS_PHASE_KERNEL, "0x500 is kernel");
    check(bios_phase_place(0x0000FFFCu) == BIOS_PHASE_KERNEL, "0xFFFC is kernel");
    check(bios_phase_place(0x00010000u) == BIOS_PHASE_OTHER, "0x10000 is other");
    check(bios_phase_place(0x0002FFFCu) == BIOS_PHASE_OTHER, "0x2FFFC is other");
    check(bios_phase_place(0x00030000u) == BIOS_PHASE_SHELL, "0x30000 is shell");
    check(bios_phase_place(0x0005AFFCu) == BIOS_PHASE_SHELL, "0x5AFFC is shell");
    check(bios_phase_place(0x0005B000u) == BIOS_PHASE_SHELL_TAIL, "0x5B000 is shell_tail");
    check(bios_phase_place(0x00097FECu) == BIOS_PHASE_SHELL_TAIL, "0x97FEC is shell_tail");
    check(bios_phase_place(0x00097FF0u) == BIOS_PHASE_OTHER, "0x97FF0 is other");
    check(bios_phase_place(0x001FFFFCu) == BIOS_PHASE_OTHER, "0x1FFFFC is other");
}

static DirtyRamPcEntry t[16];

static void test_table(void)
{
    BiosPhaseCounts c;
    memset(t, 0, sizeof(t));
    row(t, 0, 0x00000000u, 5);       /* empty slot: pc 0 */
    row(t, 1, 0x00000500u, 3);       /* kernel */
    row(t, 2, 0x80030000u, 10);      /* shell, KSEG0 key */
    row(t, 3, 0x000597E8u, 7);       /* shell */
    row(t, 4, 0x0005AFFCu, 2);       /* shell */
    row(t, 5, 0x0005B000u, 1);       /* shell_tail */
    row(t, 6, 0x00097FECu, 1);       /* shell_tail */
    row(t, 7, 0x00097FF0u, 4);       /* other */
    row(t, 8, 0x00010000u, 0);       /* no hits */
    bios_phase_count_table(t, 16, &c);
    check(c.disp[BIOS_PHASE_KERNEL] == 3 && c.pcs[BIOS_PHASE_KERNEL] == 1, "kernel: 3 hits at 1 address");
    check(c.disp[BIOS_PHASE_SHELL] == 19 && c.pcs[BIOS_PHASE_SHELL] == 3, "shell: 19 hits at 3 addresses");
    check(c.lo[BIOS_PHASE_SHELL] == 0x00030000u && c.hi[BIOS_PHASE_SHELL] == 0x0005AFFCu,
          "shell: lowest 0x30000 (from the KSEG0 key), highest 0x5AFFC");
    check(c.disp[BIOS_PHASE_SHELL_TAIL] == 2 && c.pcs[BIOS_PHASE_SHELL_TAIL] == 2, "shell_tail: 2 hits at 2 addresses");
    check(c.disp[BIOS_PHASE_OTHER] == 4 && c.pcs[BIOS_PHASE_OTHER] == 1,
          "other: 4 hits at 1 address; the empty slot and the row without hits are not counted");

    bios_phase_count_table(NULL, 16, &c);
    check(c.disp[BIOS_PHASE_SHELL] == 0 && c.pcs[BIOS_PHASE_SHELL] == 0 && c.lo[BIOS_PHASE_SHELL] == 0,
          "no table: every place 0");
}

static void test_line(void)
{
    BiosPhaseCounts c;
    char out[384];
    bios_phase_count_table(t, 16, &c);
    int n = bios_phase_format(out, sizeof(out), "game_entry", 1841, &c);
    const char *want =
        "psxrecomp: bios phase: end=game_entry frame=1841 disp_interp=28 "
        "kernel=3 shell=19 shell_tail=2 other=4 "
        "shell_pcs=3 shell_rom=0x1FC18000-0x1FC42FFC";
    check(n == (int)strlen(want) && strcmp(out, want) == 0, "the line with counts and the shell's ROM range");
    if (strcmp(out, want) != 0) fprintf(stderr, "  got:  %s\n  want: %s\n", out, want);

    memset(&c, 0, sizeof(c));
    c.disp[BIOS_PHASE_KERNEL] = 9;
    bios_phase_format(out, sizeof(out), "exit_no_game_entry", 620, &c);
    check(strcmp(out, "psxrecomp: bios phase: end=exit_no_game_entry frame=620 disp_interp=9 "
                      "kernel=9 shell=0 shell_tail=0 other=0 shell_pcs=0 shell_rom=none") == 0,
          "no shell rows: shell_rom=none");

    bios_phase_format(out, sizeof(out), "latch_without_entry", 77, NULL);
    check(strcmp(out, "psxrecomp: bios phase: end=latch_without_entry frame=77") == 0,
          "the line without counts");

    char small[40];
    memset(small, 'x', sizeof(small));
    n = bios_phase_format(small, sizeof(small), "game_entry", 1841, &c);
    check(n == -1 && small[0] == '\0', "small buffer: -1 and an empty string");
    n = bios_phase_format(out, sizeof(out), "latch_without_entry", 77, NULL);
    char exact[64];
    check(n > 0 && n < (int)sizeof(exact), "the short line fits the test buffer");
    check(bios_phase_format(exact, (size_t)n + 1, "latch_without_entry", 77, NULL) == n,
          "one byte more than the text fits");
    check(bios_phase_format(exact, (size_t)n, "latch_without_entry", 77, NULL) == -1,
          "exactly the text, without room for its end, does not");
}

static void test_hook(void)
{
    memset(g_dirty_ram_pc_table, 0, sizeof(g_dirty_ram_pc_table));
    row(g_dirty_ram_pc_table, 123, 0x000597E8u, 5133281);
    row(g_dirty_ram_pc_table, 456, 0x00000C80u, 10);
    check(g_fntrace_game_entry_hook == NULL, "no hook before install");
    bios_phase_report_install();
    check(g_fntrace_game_entry_hook != NULL, "install sets fntrace's game-entry hook");
    check(bios_phase_report_lines() == 0, "nothing printed before game entry");
    s_frame_count = 1841;
    s_started = 1;
    g_fntrace_game_entry_hook();
    check(bios_phase_report_lines() == 1, "game entry prints one line");
    g_fntrace_game_entry_hook();
    check(bios_phase_report_lines() == 1, "a second entry prints nothing");
}

int main(void)
{
    test_places();
    test_table();
    test_line();
    test_hook();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
