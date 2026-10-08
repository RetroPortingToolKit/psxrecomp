#pragma once
#include "mod_plugins.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Main (emulation) thread only; sources are not sampled in headless mode or
 * netplay. begin_frame() starts a new frame: the first sample() per player
 * invokes the callback, later ones in the same frame reuse its result.
 * reset() detaches all sources but leaves a neutral release pending for each
 * port that had one. sample() returns 1 with *released=1 for that release
 * frame (source gone; the caller restores the port's real SIO state), 0 when
 * the port has no source and no release pending. */
void mod_controller_source_reset(void);
void mod_controller_source_begin_frame(void);
int mod_controller_source_sample(uint32_t player, PSXModControllerState *state, int *released);
int mod_controller_source_present(uint32_t player);
#ifdef __cplusplus
}
#endif
