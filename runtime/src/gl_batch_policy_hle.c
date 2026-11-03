/* Textured-batch grouping, HLE tier (gl_batch_policy.h).
 *
 * The dual-source draw (key 4) composites each fragment from its own
 * primitive's blend mode: opaque texels get destination factor zero, modes
 * 0/1/3 their PS1 factors. GL applies blending for the primitives of one draw
 * in submission order, so one draw of a painter-ordered run equals drawing
 * its primitives one at a time. That holds while no primitive in the run reads
 * what an earlier one wrote: the renderer flushes before a primitive samples
 * a texture page or CLUT the queue has drawn to (flush_pack_if_sampling), and
 * mask checking (whose stencil an opaque batch fixes up after its colour)
 * keeps the LLE classification here. */
#include "gl_batch_policy.h"

GlBatchClass gl_batch_policy_hle(int semi, int mask_check, int bank_batch) {
    if (!mask_check && semi != 2) {
        GlBatchClass c = {4, 0};
        return c;
    }
    return gl_batch_policy_lle(semi, mask_check, bank_batch);
}
