#ifndef PSXRECOMP_GTE_NCLIP_STATS_H
#define PSXRECOMP_GTE_NCLIP_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Precise-NCLIP observability (widescreen). While [widescreen] precise_nclip
 * is active, every NCLIP also computes the backface sign from the UNCLAMPED
 * projection (the PGXP sub-pixel shadow, validated against each SXY word;
 * precise_nclip arms that transport by itself, independent of the visual
 * corrections). Guest MAC0 stays native; only configured branch sites
 * ([widescreen.cull] nclip_exact_sites) consume the precise sign.
 *
 * Always-on tables, keyed by the last guest store PC before each NCLIP (a
 * producer locator inside the issuing emitter) and by branch PC (exact-site
 * consumers), so a title can find which emitters
 * lose triangles to the +/-1024 screen clamp before listing any site. */

typedef struct {
    uint32_t func;        /* locator: last guest store PC before the NCLIP */
    uint32_t nclips;      /* NCLIPs with a precise sign available */
    uint32_t disagree;    /* precise sign != native MAC0 sign */
    uint32_t saturated;   /* any of the three SXY entries hit the clamp */
    uint32_t last_frame;
} GteNclipFuncStat;

typedef struct {
    uint32_t pc;          /* configured exact-NCLIP branch PC */
    uint32_t evals;
    uint32_t flips;       /* branch decision changed by the precise sign */
    uint32_t fallbacks;   /* no precise sign for this MAC0 -> native */
    uint32_t last_frame;
} GteNclipSiteStat;

#define GTE_NCLIP_STAT_CAP 256

void gte_nclip_precise_stats(uint64_t *hits, uint64_t *fallbacks,
                             uint64_t *disagreements);
int  gte_nclip_func_stats(GteNclipFuncStat *out, int max);
int  gte_nclip_site_stats(GteNclipSiteStat *out, int max);
void gte_nclip_stats_reset(void);

/* Branch consumer for exact-NCLIP sites: the sign (-1/0/1) the branch should
 * test. The precise sign when it belongs to `native_mac0` (the value the
 * guest just read from MAC0), else the native sign. */
int32_t gte_nclip_exact_sign(int32_t native_mac0, uint32_t pc);

/* Opt-in thin-face rescue for exact-word BLEZ consumers. Unlike the separate
 * horizontal-saturation policy, this only changes an architectural zero to a
 * validated positive winding. Registration is host-side; the existing overlay
 * ws_nclip_branch callback and its ABI remain unchanged. */
void psx_mod_set_native_wide_nclip_zero_sites(const uint32_t *addresses,
                                             const uint32_t *expected, int count);
int gte_nclip_zero_positive(int32_t native_mac0, uint32_t pc);

/* Kill switch for the exact-site consumer (default on): off makes every exact
 * site test the native sign, for same-build/same-state A/B and diagnosis. */
void gte_nclip_exact_set_enabled(int on);
int  gte_nclip_exact_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
