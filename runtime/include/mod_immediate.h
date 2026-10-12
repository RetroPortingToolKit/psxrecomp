/* mod_immediate.h - [[mod_immediate.sites]]: instruction immediates a trusted
 * mod selects at run time.
 *
 * A title lists guarded I-type instructions in game.toml:
 *
 *   [mod_immediate]
 *   sites = [
 *     { address = "0x80023500", expected = "0x28820020", group = 1, values = [64, 128] },
 *   ]
 *
 * Selector 0 (the default, restored at every session start) runs the original
 * instruction. Selector N runs the same instruction with values[N-1] as its
 * 16-bit immediate. A mod picks the selector of each group at activation with
 * psx_mod_set_immediate_selector (mod_plugins.h).
 *
 * The instruction bytes in RAM never change, so static AOT code, cached overlays
 * and the interpreter keep matching their original-bytes guards: generated code
 * switches on psx_mod_immediate_select(group) per listed instruction, and the
 * dirty-RAM interpreter substitutes the word with psx_mod_immediate_patch.
 * docs/config_schema.md "Mod immediates" has the config rules.
 */
#ifndef PSX_MOD_IMMEDIATE_H
#define PSX_MOD_IMMEDIATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_MOD_IMMEDIATE_GROUPS 64
#define PSX_MOD_IMMEDIATE_VALUES_MAX 15
#define PSX_MOD_IMMEDIATE_SITES_MAX 4096

typedef struct PSXModImmediateSite {
    uint32_t address;   /* any segment; stored physical */
    uint32_t expected;  /* the complete original instruction word */
    uint32_t group;     /* 0..63 */
    uint32_t count;     /* 1..PSX_MOD_IMMEDIATE_VALUES_MAX */
    uint16_t values[PSX_MOD_IMMEDIATE_VALUES_MAX]; /* imm16 for selector 1..count */
} PSXModImmediateSite;

/* Nonzero while a site exists and any selector is nonzero: the interpreter's
 * per-instruction gate. */
extern int g_psx_mod_immediate_live;

/* The title's sites (game config). Copied, sorted by (address, word) and capped
 * at PSX_MOD_IMMEDIATE_SITES_MAX. Returns the number kept. */
int psx_mod_immediate_set_sites(const PSXModImmediateSite *sites, int count);
int psx_mod_immediate_site_count(void);

/* Selector of `group`; 0 for an out-of-range group. Called by generated code. */
uint32_t psx_mod_immediate_select(uint32_t group);
/* Session reset: every selector back to 0 (stock instructions). */
void psx_mod_immediate_reset(void);

/* The instruction the interpreter executes at `pc`: `insn` itself unless a
 * listed site with that exact word has a nonzero selector. */
uint32_t psx_mod_immediate_patch(uint32_t pc, uint32_t insn);

#ifdef __cplusplus
}
#endif

#endif
