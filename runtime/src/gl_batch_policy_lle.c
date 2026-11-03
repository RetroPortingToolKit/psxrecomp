/* Textured-batch grouping, LLE tier (gl_batch_policy.h). */
#include "gl_batch_policy.h"

GlBatchClass gl_batch_policy_lle(int semi, int mask_check, int bank_batch) {
    GlBatchClass c;
    /* Dual-source (4) carries semi modes 0/1/3 with mask checking off; opaque
     * keeps its own key. Mixing opaque+semi under one key needs every queued
     * primitive to composite in order, which this tier does not rely on. */
    if (semi < 0)
        c.key = -1;
    else if (!mask_check && semi != 2)
        c.key = 4;
    else
        c.key = semi;
    /* STP draw-ORDER correctness. The conservative two-pass path draws pass
     * 1 = every prim's STP=0 texels then pass 2 = every prim's STP=1 texels,
     * so a behind prim's semi texels would overwrite a front prim's opaque
     * texels (Tomba AP-block / CTR intro flaps). Isolate EVERY
     * semi-transparent textured prim: drain the open batch, draw this prim
     * alone, let opaque prims keep batching. Cost is one draw per semi prim.
     * An opted-in immutable bank may batch the single-pass dual-source cases. */
    c.isolate = semi >= 0 && !bank_batch;
    return c;
}
