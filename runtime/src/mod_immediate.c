/* mod_immediate.c - the [[mod_immediate.sites]] store and selectors
 * (mod_immediate.h). */
#include "mod_immediate.h"
#include "mod_plugins.h"

#include <stdio.h>
#include <stdlib.h>

int g_psx_mod_immediate_live = 0;

static PSXModImmediateSite s_imm_sites[PSX_MOD_IMMEDIATE_SITES_MAX];
static int s_imm_site_count = 0;
static uint8_t s_selector[PSX_MOD_IMMEDIATE_GROUPS];

static void imm_update_live(void) {
    int any = 0;
    for (int g = 0; g < PSX_MOD_IMMEDIATE_GROUPS; g++) any |= s_selector[g] != 0u;
    g_psx_mod_immediate_live = (any && s_imm_site_count > 0) ? 1 : 0;
}

static int imm_site_cmp(const void *a, const void *b) {
    const PSXModImmediateSite *x = (const PSXModImmediateSite *)a;
    const PSXModImmediateSite *y = (const PSXModImmediateSite *)b;
    if (x->address != y->address) return x->address < y->address ? -1 : 1;
    return (x->expected > y->expected) - (x->expected < y->expected);
}

int psx_mod_immediate_set_sites(const PSXModImmediateSite *sites, int count) {
    if (!sites || count < 0) count = 0;
    if (count > PSX_MOD_IMMEDIATE_SITES_MAX) {
        fprintf(stderr,
                "psxrecomp: [[mod_immediate.sites]] lists %d sites; keeping the "
                "first %d\n", count, PSX_MOD_IMMEDIATE_SITES_MAX);
        count = PSX_MOD_IMMEDIATE_SITES_MAX;
    }
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (sites[i].group >= PSX_MOD_IMMEDIATE_GROUPS || sites[i].count == 0u ||
            sites[i].count > PSX_MOD_IMMEDIATE_VALUES_MAX)
            continue;
        s_imm_sites[kept] = sites[i];
        s_imm_sites[kept].address &= 0x1FFFFFFFu;
        kept++;
    }
    if (kept > 1) qsort(s_imm_sites, (size_t)kept, sizeof s_imm_sites[0], imm_site_cmp);
    s_imm_site_count = kept;
    imm_update_live();
    return kept;
}

int psx_mod_immediate_site_count(void) { return s_imm_site_count; }

uint32_t psx_mod_immediate_select(uint32_t group) {
    return group < PSX_MOD_IMMEDIATE_GROUPS ? s_selector[group] : 0u;
}

void psx_mod_immediate_reset(void) {
    for (int g = 0; g < PSX_MOD_IMMEDIATE_GROUPS; g++) s_selector[g] = 0u;
    imm_update_live();
}

uint32_t psx_mod_immediate_patch(uint32_t pc, uint32_t insn) {
    const uint32_t phys = pc & 0x1FFFFFFFu;
    int lo = 0, hi = s_imm_site_count;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (s_imm_sites[mid].address < phys) lo = mid + 1;
        else hi = mid;
    }
    for (; lo < s_imm_site_count && s_imm_sites[lo].address == phys; lo++) {
        const PSXModImmediateSite *site = &s_imm_sites[lo];
        if (site->expected != insn) continue;
        const uint32_t sel = s_selector[site->group];
        if (sel == 0u || sel > site->count) return insn;
        return (insn & 0xFFFF0000u) | site->values[sel - 1u];
    }
    return insn;
}

/* mod_plugins.h. Process-wide host state: the session start resets it
 * (main.cpp reset_mod_owned_presentation), so only a session whose plan
 * activates a plugin that asks for it, never netplay, runs with a selector set. */
int psx_mod_set_immediate_selector(uint32_t group, uint32_t selector) {
    if (group >= PSX_MOD_IMMEDIATE_GROUPS || selector > PSX_MOD_IMMEDIATE_VALUES_MAX)
        return 0;
    s_selector[group] = (uint8_t)selector;
    imm_update_live();
    return s_imm_site_count > 0;
}

uint32_t psx_mod_immediate_selector(uint32_t group) {
    return psx_mod_immediate_select(group);
}
