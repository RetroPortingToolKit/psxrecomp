#ifndef PSX_RENDER_PASS_H
#define PSX_RENDER_PASS_H

/* Host-timed render passes: runtime side of psx_mod_render_pass_plan() and
 * psx_mod_render_pass() (mod_plugins.h). See docs/RENDER_PASSES.md. */

#include <stdint.h>

#include "psx_cycle_freeze.h"   /* g_psx_render_pass_active, the time freeze */
#include "gpu_gl_renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Stores a pass may not make (memory.c render_pass_store): they would escape
 * the restore, so they are dropped and counted per device class. */
enum {
    RENDER_PASS_DROP_SPU = 0,
    RENDER_PASS_DROP_CD,
    RENDER_PASS_DROP_TIMER,
    RENDER_PASS_DROP_DMA,     /* DMA channels other than GPU (2) and OTC (6) */
    RENDER_PASS_DROP_GPU,     /* GP1 commands other than DMA mode / info */
    RENDER_PASS_DROP_OTHER,   /* SIO, MDEC, memory control, mod arenas, ... */
    RENDER_PASS_DROP_CLASSES
};
extern uint64_t g_render_pass_dropped_writes[RENDER_PASS_DROP_CLASSES];

typedef struct RenderPassFailure {
    const char *reason;       /* static name, NULL until the first refusal */
    uint64_t attempt, plan, guest_cycle;
    uint32_t status, alpha_q16, struct_size;
    uint16_t x, y, w, h;
    GLRenderPassBeginDiag gl;
} RenderPassFailure;

typedef struct RenderPassStats {
    uint64_t plans;           /* psx_mod_render_pass_plan calls that planned >= 1 */
    uint64_t planned;         /* phases planned (after shedding) */
    uint64_t wanted;          /* phases wanted before shedding */
    uint64_t refused;         /* plan calls that returned 0 while enabled */
    uint64_t pass_attempts;   /* all psx_mod_render_pass calls */
    uint64_t argument_refused, status_refused, begin_refused, checkpoint_refused;
    RenderPassFailure last_failure; /* latched across successes; session reset clears */
    uint64_t passes;          /* passes run to completion and presented */
    uint64_t aborted;         /* passes rolled back by a fault */
    uint64_t discarded;       /* passes whose plugin declined the image */
    uint64_t watchdog;        /* of which: guest-cycle watchdog overruns */
    uint64_t vram_leaks;      /* GPU writes outside the declared rect (dropped) */
    uint64_t nesting_repairs; /* aborts whose skipped exits the restore undid */
    char last_abort_detail[192]; /* skipped host exits; latched until session reset */
    uint64_t verify_checks;   /* PSX_RENDER_PASS_VERIFY comparisons */
    uint64_t verify_mismatch; /* ... that found a difference */
    uint64_t dropped[RENDER_PASS_DROP_CLASSES];
    double   last_pass_ms;    /* host time of the last pass (sandbox + draw) */
    double   avg_pass_ms;     /* smoothed */
    double   avg_begin_ms;    /* smoothed: VRAM backup + checkpoint */
    double   avg_guest_ms;    /* smoothed: the plugin's guest draw code */
    double   avg_end_ms;      /* smoothed: GPU flush, capture, VRAM restore */
    double   avg_restore_ms;  /* smoothed: machine state restore */
    uint64_t guest_cycles_last; /* guest cycles the last pass executed */
    int      disabled;        /* sticky: passes disabled after repeated faults */
    int      watchdog_flag;   /* internal: the current pass overran */
} RenderPassStats;

void render_pass_get_stats(RenderPassStats *out);
/* Clears counters and the sticky fault latch (new mod session). */
void render_pass_reset_session(void);

#ifdef __cplusplus
}
#endif

#endif
