/* GPU frame timeline ring — see gpu_timeline.h. */
#include "gpu_timeline.h"
#include "psx_cycles.h"
#include "psx_cycle_freeze.h"

extern uint64_t s_frame_count;  /* debug_server.c */

static GpuTimelineEntry s_ring[GPU_TIMELINE_CAP];
static uint64_t s_total = 0;
static int s_frozen = 0;

void gpu_timeline_note(uint8_t kind, uint32_t a, uint32_t b) {
    if (g_psx_render_pass_active || s_frozen) return;
    GpuTimelineEntry *e = &s_ring[s_total & (GPU_TIMELINE_CAP - 1u)];
    e->cyc = psx_cycle_count;
    e->frame = (uint32_t)s_frame_count;
    e->a = a;
    e->b = b;
    e->kind = kind;
    s_total++;
}

void gpu_timeline_set_frozen(int frozen) { s_frozen = frozen ? 1 : 0; }

uint64_t gpu_timeline_total(void) { return s_total; }

int gpu_timeline_get(uint64_t seq, GpuTimelineEntry *out) {
    if (seq >= s_total || s_total - seq > GPU_TIMELINE_CAP) return 0;
    *out = s_ring[seq & (GPU_TIMELINE_CAP - 1u)];
    return 1;
}

const char *gpu_timeline_kind_name(uint8_t kind) {
    switch (kind) {
    case GTL_VBLANK:       return "vblank";
    case GTL_DISP_START:   return "disp_start";
    case GTL_DRAW_AREA:    return "draw_area";
    case GTL_LL_START:     return "ll_start";
    case GTL_LL_END:       return "ll_end";
    case GTL_PRESENT:      return "present";
    case GTL_PRESENT_SKIP: return "present_skip";
    case GTL_DISP_CAPTURE: return "disp_capture";
    case GTL_FILL:         return "fill";
    default:               return "?";
    }
}
