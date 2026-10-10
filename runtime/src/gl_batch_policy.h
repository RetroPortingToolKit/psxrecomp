#ifndef GL_BATCH_POLICY_H
#define GL_BATCH_POLICY_H

/* Textured-batch grouping for gpu_gl_renderer.c, a build-time tier
 * (runtime.cmake PSX_GL_BATCH_IMPL = LLE | HLE; recomp-template HLE.md).
 *
 * Contract, both tiers: classify one textured primitive from its blend mode
 * (semi: -1 opaque, 0..3 the PS1 modes), the destination mask-check state
 * (GP0(E6h) bit 1) and whether an opted-in immutable texture bank already
 * permits dual-source batching. Returns its batch key and whether it must be
 * drawn alone. Keys: -1 opaque passes; 0..3 the two-pass STP split with that
 * blend; 4 the single-pass dual-source draw, which blends modes 0/1/3 per
 * fragment and gives opaque texels (STP=0 or an opaque primitive) destination
 * factor zero. The renderer still splits batches on every other key (mask-set,
 * filter, texture window, backdrop gate, depth mode, texture source) and
 * flushes before any primitive samples VRAM a queued draw has written.
 * Either tier draws in submission (painter) order and produces the LLE pixels.
 *
 * LLE: the reference. Every semi-transparent primitive is drawn alone.
 * HLE: opaque primitives and modes 0/1/3 share one painter-ordered
 *      dual-source draw while mask checking is off. GL blends the primitives
 *      of one draw in order, so the result equals drawing them one by one.
 *      Mode 2 (subtractive) and mask-checked draws keep the LLE behaviour. */
typedef struct {
    int key;
    int isolate;
} GlBatchClass;

GlBatchClass gl_batch_policy_lle(int semi, int mask_check, int bank_batch);
GlBatchClass gl_batch_policy_hle(int semi, int mask_check, int bank_batch);

/* The tier this build selected. */
GlBatchClass gl_batch_classify(int semi, int mask_check, int bank_batch);
const char *gl_batch_policy_name(void);

#endif
