/* Fast Loading engage/hold gate. See turbo_loads_gate.h. */
#include "turbo_loads_gate.h"

#define WINDOW_MASK ((UINT64_C(1) << TURBO_LOADS_WINDOW) - 1u)

static int s_engaged;

void turbo_loads_set_engaged(int engaged)
{
    s_engaged = engaged ? 1 : 0;
}

int turbo_loads_engaged(void)
{
    return s_engaged;
}

void turbo_loads_gate_reset(TurboLoadsGate *g)
{
    g->history = 0;
    g->engaged = 0;
    g->release = 0;
}

int turbo_loads_gate_window_reads(const TurboLoadsGate *g)
{
    int n = 0;
    for (uint64_t h = g->history; h; h &= h - 1u) ++n;
    return n;
}

int turbo_loads_gate_step(TurboLoadsGate *g, int data_read, int load_hold, int release_frames)
{
    g->history = ((g->history << 1) | (data_read ? 1u : 0u)) & WINDOW_MASK;
    if (!g->engaged) {
        if (turbo_loads_gate_window_reads(g) < TURBO_LOADS_WINDOW_READS)
            return 0;
        g->engaged = 1;
    }
    if (data_read || load_hold) {
        g->release = release_frames;
        return 1;
    }
    if (g->release-- > 0)
        return 1;
    turbo_loads_gate_reset(g);   /* a later short read starts from an empty window */
    return 0;
}
