#pragma once
#include "mod_plugins.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Storage and per-frame validation for psx_mod_set_pad_transform (stage 2b of
 * pad_external_input.h). Main (emulation) thread only.
 *
 * run(): 0 = no transform and no release pending (caller delivers the stock
 * pad). 1 = *out is what to deliver: the transform's validated output, the
 * stock pad when it declined, a neutral frame of the frame's type when its
 * output was invalid, or the one neutral release frame owed after a
 * detach/reset. `stock` is the pad stages 1-2 resolved, which the output
 * arrives pre-filled with; NULL = the frame itself. reset() detaches every transform, leaving that release owed.
 * initial_type(): the registered boot/hotplug type, or -1. */
int mod_pad_transform_run(uint32_t player, const PSXModPadFrame *frame,
                          const PSXModPadOutput *stock, PSXModPadOutput *out);
void mod_pad_transform_reset(void);
int mod_pad_transform_initial_type(uint32_t player);
#ifdef __cplusplus
}
#endif
