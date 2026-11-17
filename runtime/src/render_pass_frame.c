/* Retired guest replay interpolation entry points. Kept as rejection-only
 * ABI adapters while consumers migrate; no guest callbacks are invoked. */
#include "mod_plugins.h"
int psx_mod_activate_render_pass_rate(const char *package, const char *feature,
    const char *option, uint32_t flip_mode) {
    (void)package; (void)feature; (void)option; (void)flip_mode;
    return 0;
}
uint32_t psx_mod_render_pass_frame(struct CPUState *cpu,
    const PSXModRenderPassFrame *frame, PSXModRenderPassFn fn, void *user) {
    (void)cpu; (void)frame; (void)fn; (void)user;
    return 0;
}
