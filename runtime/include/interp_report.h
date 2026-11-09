/* interp_report.h — the run report's "interp_detail" object (PS1B-391).
 *
 * A release product has no debug server. Its run report gave one number for
 * interpreted dispatches above the kernel window and could not say which
 * addresses ran interpreted, whether they lie in the boot text or in overlay
 * RAM, whether the text guard refused them, or why the overlay loader had no
 * native unit. This object says it, from counters the runtime keeps anyway.
 *
 * The builder only reads. It takes no lock and allocates nothing, because the
 * report is also written on the crash path. */
#ifndef PSXRECOMP_INTERP_REPORT_H
#define PSXRECOMP_INTERP_REPORT_H

#include <stdint.h>
#include "dirty_ram_interp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Why overlay_loader_dispatch found no native unit for a target. One counter
 * per reason and per place (above the kernel window, inside it). */
enum {
    PSX_MISS_CACHED = 0,      /* a miss the loader already knew: nothing valid here since the last change */
    PSX_MISS_NO_UNIT,         /* no compiled unit covers the address */
    PSX_MISS_STALE_BYTES,     /* a unit covers it, but none matches the live code bytes */
    PSX_MISS_OUTSIDE_WINDOW,  /* clean boot text or another address the loader does not serve */
    PSX_MISS_DEVICE_TOUCH,    /* the unit matches but touches a device: it never runs natively */
    PSX_MISS_NATIVE_OFF,      /* native execution is off, or this function is blocked */
    PSX_MISS_DIFF_GATE,       /* held back by the native/interpreter comparison (development) */
    PSX_MISS_RANK,            /* held back by the native rank filter (debug tools only) */
    PSX_MISS_BAD_ENTRY,       /* the unit ran and refused a foreign interior entry */
    PSX_MISS_MOD_ENTRY_HOOK,   /* a trusted mod owns the entry */
    PSX_INTERP_MISS_REASONS
};

#define PSX_INTERP_REPORT_HOTTEST 32

/* The places of an address, in the report's order: kernel, text, overlay. */
#define PSX_INTERP_PLACES 3

/* What the interpreter ran before the game was entered (PS1B-513).
 *
 * The place of an address is decided from the game's text range. Code that
 * ran before the game's boot executable was loaded is not the game's: with a
 * real BIOS intro the BIOS shell runs from RAM 0x30000 and up, which lies in
 * the game's text or overlay range. At game entry the rows of the per-address
 * table are added here, by place, and their counts are cleared; each row
 * keeps its address. So the report's by_place and hottest describe what ran
 * after game entry, and by_place plus this is what ran since power-on. */
typedef struct {
    int      game_started;              /* 1 once game entry moved the rows here */
    uint64_t frame;                     /* the frame of the last such move */
    uint32_t addresses[PSX_INTERP_PLACES];
    uint64_t hits[PSX_INTERP_PLACES], entries[PSX_INTERP_PLACES];
    uint64_t interpreter[5];          /* blocks, insns, aborts, yields, handoffs at entry */
    uint64_t miss[2][PSX_INTERP_MISS_REASONS];
} PsxInterpPreload;

typedef struct {
    /* Text image guard (memory.c). */
    int      guard_armed;
    uint32_t guard_lo, guard_hi;        /* physical range of the reference image */
    uint64_t native_blocked;
    uint32_t diverged_pages;
    uint64_t exact_mismatches;
    uint32_t exact_last[5];             /* range lo, range len, first differing address, live byte, reference byte */
    /* Where an address lies: [0, kernel_end) kernel window, [text_lo, text_hi) boot text, else overlay RAM. */
    uint32_t kernel_end, text_lo, text_hi;
    /* Interpreter totals (dirty_ram_interp.c). */
    uint64_t blocks_run, insns_run, aborts, guard_yields, native_handoffs;
    /* Per-address table of interpreted block entries; pc 0 marks an empty slot. */
    const DirtyRamPcEntry *table;
    uint32_t table_size;
    /* Loader misses by reason: [0] above the kernel window, [1] inside it. */
    uint64_t miss[2][PSX_INTERP_MISS_REASONS];
    /* The rows moved out of the table at game entry. */
    PsxInterpPreload preload;
} PsxInterpReportInput;

const char *psx_interp_miss_reason_name(int reason);

/* Game entry: add every row of the table that has hits to `pre`, by the place
 * of its address, then clear the row's counts and caller fields. The row keeps
 * its address, so no probe chain of the table breaks. Called once per game
 * entry, on the emulation thread; a later entry adds to the same sums. */
void psx_interp_preload_close(DirtyRamPcEntry *table, uint32_t table_size,
                              uint32_t kernel_end, uint32_t text_lo, uint32_t text_hi,
                              uint64_t frame, PsxInterpPreload *pre);

/* Save the first game-entry counter snapshot. Later entries preserve it. */
void psx_interp_preload_counters(PsxInterpPreload *pre,
                                 const uint64_t interpreter[5],
                                 const uint64_t miss[2][PSX_INTERP_MISS_REASONS]);

/* Return the post-entry share without unsigned wraparound. */
uint64_t psx_interp_after_entry(uint64_t total, uint64_t preload, int game_started);

/* The runtime's own table (dirty_ram_interp.c): dirty_ram_pc_table_game_load
 * runs psx_interp_preload_close on it at game entry (fntrace.c), and
 * dirty_ram_pc_table_preload copies the sums for the run report. */
void dirty_ram_pc_table_game_load(void);
void dirty_ram_pc_table_preload(PsxInterpPreload *out);

/* Write the object to `out` (a JSON object, no trailing newline). Returns its
 * length, or -1 when `cap` is too small; `out` then holds no partial object. */
int psx_interp_report_json(char *out, int cap, const PsxInterpReportInput *in);

#ifdef __cplusplus
}
#endif

#endif
