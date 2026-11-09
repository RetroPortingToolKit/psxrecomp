/* bios_phase_report.h — what the interpreter ran before game entry (PS1B-105).
 *
 * With a real BIOS intro (fast_boot false, the BIOS run as recompiled code)
 * the BIOS runs before the game: the kernel, then the shell, which
 * LoadRunShell copies from ROM 0x1FC18000 to RAM 0x30000. A shell with no
 * static seeds (SCPH-5552) runs in the dirty-RAM interpreter. The frame report
 * gives the speed of those frames; this line gives the count of what the
 * interpreter ran in them, by place:
 *
 *   kernel      RAM below 0x10000: the kernel and its RAM copy
 *   shell       RAM 0x30000-0x5AFFF: the profiles' shell window,
 *               ROM 0x1FC18000-0x1FC42FFF
 *   shell_tail  RAM 0x5B000-0x97FEF: the rest of LoadRunShell's 0x67FF0-byte
 *               copy (bios_hle.c)
 *   other       anywhere else
 *
 * The count is the interpreter's per-address table: one dispatch is one block
 * the interpreter started at that address, the run report's per_address
 * "hits". It is not the overlay loader's disp_interp, which counts only while
 * the loader is active and is 0 during the BIOS. The table is read once: at
 * the first game entry, or at exit when the game was never entered (no disc,
 * or a start that stopped in the BIOS). One line on stdout:
 *
 *   psxrecomp: bios phase: end=game_entry frame=1841 disp_interp=23086040
 *     kernel=367000 shell=22700000 shell_tail=0 other=19040
 *     shell_pcs=1544 shell_rom=0x1FC18000-0x1FC42FFC
 *
 * (one line in the output). end= is game_entry, exit_no_game_entry, or
 * latch_without_entry: the game-start latch was set without game entry (a
 * state load), so no BIOS phase was counted and the counts are left out.
 */
#ifndef PSXRECOMP_BIOS_PHASE_REPORT_H
#define PSXRECOMP_BIOS_PHASE_REPORT_H

#include <stddef.h>
#include <stdint.h>
#include "dirty_ram_interp.h"   /* DirtyRamPcEntry */

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BIOS_PHASE_KERNEL = 0,
    BIOS_PHASE_SHELL,
    BIOS_PHASE_SHELL_TAIL,
    BIOS_PHASE_OTHER,
    BIOS_PHASE_PLACES
};

#define BIOS_PHASE_KERNEL_HI     0x00010000u  /* DIRTY_RAM_KERNEL_WINDOW_END */
#define BIOS_PHASE_SHELL_RAM_LO  0x00030000u  /* shell copy target, shell_entry_phys */
#define BIOS_PHASE_SHELL_RAM_HI  0x0005B000u  /* end of the profiles' shell window */
#define BIOS_PHASE_SHELL_COPY_HI 0x00097FF0u  /* end of LoadRunShell's copy */
#define BIOS_PHASE_SHELL_ROM_LO  0x1FC18000u  /* ROM of RAM 0x30000 */

typedef struct {
    uint64_t disp[BIOS_PHASE_PLACES];  /* interpreted dispatches */
    uint32_t pcs[BIOS_PHASE_PLACES];   /* addresses with at least one */
    uint32_t lo[BIOS_PHASE_PLACES];    /* lowest such address (physical); 0 when none */
    uint32_t hi[BIOS_PHASE_PLACES];    /* highest such address; 0 when none */
} BiosPhaseCounts;

/* The place of a physical address. */
int bios_phase_place(uint32_t phys);

/* Sum a per-address table by place. Empty rows (pc 0) and rows with no hits
 * are skipped. `out` is overwritten. */
void bios_phase_count_table(const DirtyRamPcEntry *table, uint32_t size,
                            BiosPhaseCounts *out);

/* The stdout line, without the trailing newline. `c` may be NULL only for
 * end "latch_without_entry". Returns the length, or -1 when `cap` is too small
 * (then `out` holds an empty string). */
int bios_phase_format(char *out, size_t cap, const char *end, uint64_t frame,
                      const BiosPhaseCounts *c);

/* Runtime: called once at start-up (main.cpp). Sets fntrace's game-entry
 * hook and registers the exit print. */
void bios_phase_report_install(void);

/* Lines printed so far (0 or 1); for tests. */
int bios_phase_report_lines(void);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_BIOS_PHASE_REPORT_H */
