#pragma once
#include "mod_plugins.h"
#ifdef __cplusplus
extern "C" {
#endif
void mod_controller_source_reset(void);
int mod_controller_source_sample(uint32_t player, PSXModControllerState *state);
int mod_controller_source_present(uint32_t player);
#ifdef __cplusplus
}
#endif
