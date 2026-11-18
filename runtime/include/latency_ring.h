#ifndef PSX_LATENCY_RING_H
#define PSX_LATENCY_RING_H

/* Always-on input->photon latency ring.
 *
 * Every present cycle (one sdl_vblank_present call) stamps a QPC timestamp at
 * a few fixed stages.  The ring records continuously from process start so a
 * probe can QUERY a window of recent frames after the fact (never arm-then-
 * capture).  It is backend-agnostic: the GL and Vulkan present paths both mark
 * the same SWAP_BEGIN/SWAP_END boundary, so their latency is directly
 * comparable.  Query through the debug server ("latency" command).
 *
 * The emulation thread begins slots. Render-thread present records carry the
 * originating slot, so a delayed swap cannot stamp a later input frame.
 * Atomic fields allow best-effort diagnostic reads without a data race. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LAT_INPUT = 0,    /* pad state sampled into SIO for this cycle           */
    LAT_PACED,        /* frame_pacer_wait() returned (wall-clock cap done)   */
    LAT_SWAP_BEGIN,   /* immediately before the backend present/swap         */
    LAT_SWAP_END,     /* immediately after the present/swap returned         */
    LAT_STAGE_COUNT
} LatencyStage;

/* Begin a new frame slot and stamp LAT_INPUT.  Call once per cycle, at the
 * point input is sampled.  Advances the ring head. */
void latency_ring_frame_begin(void);

/* Stamp the current frame slot at `stage` (no-op for LAT_INPUT — use
 * latency_ring_frame_begin for that). */
void latency_ring_mark(LatencyStage stage);
/* Stable input slot to retain in an asynchronous present record. UINT64_MAX
 * means none. A replay override is thread-local; clear it after that present. */
uint64_t latency_ring_current_frame(void);
void latency_ring_replay_frame(uint64_t frame);
int latency_ring_mark_frame(uint64_t frame,LatencyStage stage);

/* Re-stamp the current slot's LAT_INPUT to now.  Used by the low-latency
 * present path, which re-samples the pad AFTER the pacer wait: the input the
 * CPU actually reads is this later one, so input_to_swap must measure from it.
 * (With this active, input_to_paced is negative and drops out of the summary.) */
void latency_ring_restamp_input(void);

/* Note which present backend produced the most recent swaps ("opengl",
 * "software", "vulkan").  Reported in the summary so a comparison is labelled. */
void latency_ring_set_backend(const char *name);

/* Note the effective present mode / swap interval for the summary label
 * (e.g. 1 = vsync, 0 = immediate, -1 = adaptive; other backends pass their
 * own code).  Purely descriptive. */
void latency_ring_set_present_mode(int mode);

/* JSON summary over the last `window` completed frames (clamped): mean/min/max/
 * p50/p95 in microseconds of frame_period, input->swap, and swap_block (the
 * present/swap blocking time — the key vsync/queue-latency signal).  Writes a
 * brace-delimited object into buf; returns bytes written (excluding NUL). */
int latency_ring_summary_json(char *buf, int buf_size, int window);

/* JSON array of the last `max_frames` raw frame records (timestamps in
 * microseconds relative to the oldest reported frame). */
int latency_ring_dump_json(char *buf, int buf_size, int max_frames);

#ifdef __cplusplus
}
#endif

#endif /* PSX_LATENCY_RING_H */
