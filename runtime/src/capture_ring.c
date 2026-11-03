#include "capture_ring.h"
#include "boot_state.h"
#include "retcomm_rbengine/snap_ring.h"

typedef struct CaptureContext {
    const CPUState *cpu;
    uint32_t bios, entry;
} CaptureContext;

static int capture(void *opaque, uint32_t tick, uint8_t **data,
                    size_t *capacity, size_t *size) {
    const CaptureContext *ctx = (const CaptureContext *)opaque;
    (void)tick;
    return boot_state_capture_raw(ctx->cpu, ctx->bios, ctx->entry,
                                   data, capacity, size);
}

int psx_capture_ring_save(struct RbeSnapRing *ring, uint32_t tick,
                          const CPUState *cpu, uint32_t bios, uint32_t entry) {
    CaptureContext ctx = {cpu, bios, entry};
    return rbe_snap_ring_capture(ring, tick, capture, &ctx);
}
