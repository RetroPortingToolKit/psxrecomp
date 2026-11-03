/* Binds the textured-batch tier this build selected (gl_batch_policy.h).
 * runtime.cmake defines PSX_GL_BATCH_HLE from PSX_GL_BATCH_IMPL; both tiers
 * are always compiled so the LLE floor keeps building. */
#include "gl_batch_policy.h"

#if PSX_GL_BATCH_HLE
GlBatchClass gl_batch_classify(int semi, int mask_check, int bank_batch) {
    return gl_batch_policy_hle(semi, mask_check, bank_batch);
}
const char *gl_batch_policy_name(void) { return "HLE"; }
#else
GlBatchClass gl_batch_classify(int semi, int mask_check, int bank_batch) {
    return gl_batch_policy_lle(semi, mask_check, bank_batch);
}
const char *gl_batch_policy_name(void) { return "LLE"; }
#endif
