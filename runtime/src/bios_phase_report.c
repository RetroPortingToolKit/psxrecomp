/* bios_phase_report.c — what the interpreter ran before game entry (PS1B-105).
 * See bios_phase_report.h for the line and what it counts. */

#include "bios_phase_report.h"
#include "fntrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bios_phase_place(uint32_t phys)
{
    if (phys < BIOS_PHASE_KERNEL_HI) return BIOS_PHASE_KERNEL;
    if (phys >= BIOS_PHASE_SHELL_RAM_LO && phys < BIOS_PHASE_SHELL_RAM_HI) return BIOS_PHASE_SHELL;
    if (phys >= BIOS_PHASE_SHELL_RAM_HI && phys < BIOS_PHASE_SHELL_COPY_HI) return BIOS_PHASE_SHELL_TAIL;
    return BIOS_PHASE_OTHER;
}

void bios_phase_count_table(const DirtyRamPcEntry *table, uint32_t size,
                            BiosPhaseCounts *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    for (uint32_t i = 0; table && i < size; i++) {
        const DirtyRamPcEntry *e = &table[i];
        if (e->pc == 0 || e->hits == 0) continue;
        uint32_t phys = e->pc & 0x1FFFFFFFu;
        int p = bios_phase_place(phys);
        out->disp[p] += e->hits;
        if (out->pcs[p] == 0 || phys < out->lo[p]) out->lo[p] = phys;
        if (out->pcs[p] == 0 || phys > out->hi[p]) out->hi[p] = phys;
        out->pcs[p]++;
    }
}

int bios_phase_format(char *out, size_t cap, const char *end, uint64_t frame,
                      const BiosPhaseCounts *c)
{
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    int n;
    if (!c) {
        n = snprintf(out, cap, "psxrecomp: bios phase: end=%s frame=%llu",
                     end ? end : "?", (unsigned long long)frame);
    } else {
        uint64_t total = 0;
        for (int p = 0; p < BIOS_PHASE_PLACES; p++) total += c->disp[p];
        char rom[40];
        if (c->pcs[BIOS_PHASE_SHELL])
            snprintf(rom, sizeof(rom), "0x%08X-0x%08X",
                     (unsigned)(c->lo[BIOS_PHASE_SHELL] - BIOS_PHASE_SHELL_RAM_LO + BIOS_PHASE_SHELL_ROM_LO),
                     (unsigned)(c->hi[BIOS_PHASE_SHELL] - BIOS_PHASE_SHELL_RAM_LO + BIOS_PHASE_SHELL_ROM_LO));
        else
            snprintf(rom, sizeof(rom), "none");
        n = snprintf(out, cap,
                     "psxrecomp: bios phase: end=%s frame=%llu disp_interp=%llu "
                     "kernel=%llu shell=%llu shell_tail=%llu other=%llu "
                     "shell_pcs=%u shell_rom=%s",
                     end ? end : "?", (unsigned long long)frame,
                     (unsigned long long)total,
                     (unsigned long long)c->disp[BIOS_PHASE_KERNEL],
                     (unsigned long long)c->disp[BIOS_PHASE_SHELL],
                     (unsigned long long)c->disp[BIOS_PHASE_SHELL_TAIL],
                     (unsigned long long)c->disp[BIOS_PHASE_OTHER],
                     (unsigned)c->pcs[BIOS_PHASE_SHELL], rom);
    }
    if (n < 0 || (size_t)n >= cap) { out[0] = '\0'; return -1; }
    return n;
}

/* ---- runtime side ------------------------------------------------------ */

static int s_lines;

int bios_phase_report_lines(void) { return s_lines; }

static void print_line(const char *end, int with_counts)
{
    extern uint64_t s_frame_count;
    BiosPhaseCounts c;
    char line[384];
    if (with_counts)
        bios_phase_count_table(g_dirty_ram_pc_table, DIRTY_RAM_PC_TABLE_SIZE, &c);
    if (bios_phase_format(line, sizeof(line), end, s_frame_count,
                          with_counts ? &c : NULL) < 0)
        return;
    fprintf(stdout, "%s\n", line);
    fflush(stdout);
    s_lines++;
}

/* fntrace's game-entry hook: first thing at the latch's 0 -> 1, before the
 * entry's side effects. Only the first entry of a start is the BIOS phase's
 * end; a later one (the game range armed again) prints nothing. */
static void bios_phase_on_game_entry(void)
{
    if (s_lines) return;
    print_line("game_entry", 1);
}

static void bios_phase_on_exit(void)
{
    if (s_lines) return;
    if (fntrace_is_game_started()) print_line("latch_without_entry", 0);
    else print_line("exit_no_game_entry", 1);
}

void bios_phase_report_install(void)
{
    static int installed;
    if (installed) return;
    installed = 1;
    g_fntrace_game_entry_hook = bios_phase_on_game_entry;
    atexit(bios_phase_on_exit);
}
