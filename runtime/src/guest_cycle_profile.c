/* guest_cycle_profile.c - always-on guest-cycle profile ring.
 *
 * Answers "where did the guest CPU spend its cycles between frame A and
 * frame B" from history, without arming anything. psx_cycles.c calls the
 * hook installed here every PSX_GCP_PERIOD guest cycles (default 4096, about
 * 8250 samples per guest second). Each sample records the code location in
 * every form the runtime tracks:
 *
 *   block   last compiled/interpreted basic-block leader (debug-tools builds;
 *           debug_server_cyc_observe runs at every block leader)
 *   fn      last guest function entered (g_psx_last_fn_entry; leaf-biased
 *           after a callee returns)
 *   ra      guest $ra, which names the caller of a running leaf and the
 *           function a just-returned callee came back to
 *   native  native overlay shard in progress (0 when none)
 *   disp    static-dispatch stamp (g_debug_current_func_addr)
 *   phase   backend executing (0 other, 1 interp, 2 native, 3 static, 4 gpu)
 *
 * `weight` is the number of sample periods the charge crossed: an idle skip
 * or a long stall that jumps many periods at once is credited to the code
 * that was waiting, so a frame-paced VSync wait shows up as wait cycles, not
 * as a gap.
 *
 * 512K entries x 40 bytes = 20 MB; at the default period that is about 63
 * guest seconds of history. Query with the gcp_ring debug command. */

#include "guest_cycle_profile.h"
#include "psx_bss.h"
#include "psx_cycles.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint64_t s_frame_count;
extern volatile uint32_t g_psx_last_fn_entry;
extern volatile uint32_t g_psx_last_block;
extern uint32_t g_debug_current_func_addr;
extern uint32_t debug_guest_ra(void);
extern uint32_t overlay_loader_native_inprogress(void);
extern int psx_exec_phase(void);
extern int psx_get_in_exception(void);

extern uint32_t psx_cycle_sample_period;
extern void (*psx_cycle_sample_hook)(uint64_t cycle, uint32_t periods);

#define GCP_CAP (1u << 19)

static PSX_BSS GcpSample s_gcp[GCP_CAP];
static uint64_t s_gcp_total = 0;

static void gcp_record(uint64_t cycle, uint32_t periods)
{
    GcpSample *e = &s_gcp[s_gcp_total & (GCP_CAP - 1u)];
    e->cycle  = cycle;
    e->frame  = (uint32_t)s_frame_count;
    e->block  = g_psx_last_block;
    e->fn     = g_psx_last_fn_entry;
    e->ra     = debug_guest_ra();
    e->native = overlay_loader_native_inprogress();
    e->disp   = g_debug_current_func_addr;
    e->weight = periods;
    e->phase  = (uint8_t)psx_exec_phase();
    e->flags  = (uint8_t)(psx_get_in_exception() ? GCP_FLAG_EXCEPTION : 0u);
    s_gcp_total++;
}

void guest_cycle_profile_install(void)
{
    uint32_t period = 4096u;
    const char *env = getenv("PSX_GCP_PERIOD");
    if (env && *env) period = (uint32_t)strtoul(env, NULL, 0);
    psx_cycle_sample_hook = gcp_record;
    psx_cycle_sample_period = period;
}

uint32_t guest_cycle_profile_period(void) { return psx_cycle_sample_period; }
uint64_t guest_cycle_profile_total(void) { return s_gcp_total; }
uint32_t guest_cycle_profile_capacity(void) { return GCP_CAP; }

int guest_cycle_profile_get(uint64_t seq, GcpSample *out)
{
    if (seq >= s_gcp_total) return 0;
    if (s_gcp_total - seq > GCP_CAP) return 0;
    *out = s_gcp[seq & (GCP_CAP - 1u)];
    return 1;
}

uint64_t guest_cycle_profile_oldest(void)
{
    return s_gcp_total > GCP_CAP ? s_gcp_total - GCP_CAP : 0;
}

int guest_cycle_profile_write_csv(const char *path, uint64_t frame_lo,
                                  uint64_t frame_hi, uint64_t *rows_out,
                                  uint64_t *weight_out)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fprintf(f, "seq,cycle,frame,weight,phase,flags,block,fn,ra,native,disp\n");
    uint64_t rows = 0, weight = 0;
    for (uint64_t s = guest_cycle_profile_oldest(); s < s_gcp_total; s++) {
        const GcpSample *e = &s_gcp[s & (GCP_CAP - 1u)];
        if (e->frame < frame_lo || e->frame > frame_hi) continue;
        fprintf(f, "%llu,%llu,%u,%u,%u,%u,%08X,%08X,%08X,%08X,%08X\n",
                (unsigned long long)s, (unsigned long long)e->cycle, e->frame,
                e->weight, e->phase, e->flags, e->block, e->fn, e->ra,
                e->native, e->disp);
        rows++;
        weight += e->weight;
    }
    fclose(f);
    if (rows_out) *rows_out = rows;
    if (weight_out) *weight_out = weight;
    return 1;
}
