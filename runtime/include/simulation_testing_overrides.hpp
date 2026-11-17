#pragma once
#include <cstdlib>

namespace psx_simulation_testing {
/* Configured sources only. Several core resolvers latch lazily; do not call
 * them or change the process environment while describing a session. Presence
 * conservatively excludes these developer policies from qualified profiles,
 * including an explicit value of 0. This is not an arbitrary environment scan. */
inline constexpr const char *OverrideNames[]={
    "PSX_FORCE_INTERP","PSX_LOAD_DELAY","PSX_MMIO_WAIT","PSX_POLL_PROOF",
    "PSX_IDLE_SKIP","PSX_SHELLWIN_INTERP","PSX_KERNEL_BLESS","PSX_KERNEL_PATCH_RANGES",
    "PSX_TOMB_CARD_EVCB_PROTECT","PSX_EVENT_STEP_CONSERVATIVE","PSX_RB_CD_BISECT",
    "PSX_ND_SIB_OT_BATCH","PSX_ND_SIB_OT_AVSZ","PSX_ND_SIB_OT_BIAS","PSX_ND_SIB_OT_IDX",
    "PSX_FIBER_STACK_KB","PSX_FAIL_FAST_UNKNOWN_DISPATCH"
};
inline const char *first_configured_override(){
    for(const char *name:OverrideNames)if(std::getenv(name))return name;
    return nullptr;
}
}
