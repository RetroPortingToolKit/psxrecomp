#pragma once
#include <stdint.h>
#include "psx_thread_local.h"
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;
typedef struct PSXProjectionHistory PSXProjectionHistory;
typedef struct PSXProjectionStats {
    uint32_t captured, matched, changed, unmatched, discontinuities, overflow, replayed;
} PSXProjectionStats;
/* Capture the final model/view transforms that the game actually projects.
 * Replay uses the same guest draw code, with interpolated transforms only for
 * identified vertices. Unmatched vertices and camera cuts keep current state.
 * A history belongs to one game drawing section; emulation thread only. */
PSXProjectionHistory* psx_projection_create(uint32_t capacity);
void psx_projection_destroy(PSXProjectionHistory* history);
void psx_projection_invalidate(PSXProjectionHistory* history);
void psx_projection_reset_session(void);
void psx_projection_capture_begin(PSXProjectionHistory* history);
void psx_projection_capture_end(PSXProjectionHistory* history, uint32_t ticks,
                                 double max_translation_per_tick);
void psx_projection_replay_begin(PSXProjectionHistory* history, uint32_t alpha_q16);
void psx_projection_replay_end(PSXProjectionHistory* history);
void psx_projection_stats(const PSXProjectionHistory* history, PSXProjectionStats* out);
/* GTE bridge: before RTPS/RTPT imports its registers. Returns nonzero only
 * when a render pass temporarily replaced rotation/translation. The bridge
 * restores those eight control registers after exporting command results. */
int psx_projection_command(struct CPUState* cpu, uint32_t command);
extern PSX_THREAD_LOCAL int (*g_psx_projection_command)(struct CPUState*, uint32_t);
#ifdef __cplusplus
}
#endif
