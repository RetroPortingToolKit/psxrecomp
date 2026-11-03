#ifndef PSX_FRAME_TRACE_H
#define PSX_FRAME_TRACE_H

/* Opt-in, fixed-capacity frame-time tracing for the stereo/OpenXR path.
 *
 * WHY THIS EXISTS
 * ---------------
 * VR stutter reports ("it hitches when enemies spawn", "fast head turns judder",
 * "one heavy frame causes a run of stutters") could not be attributed. The
 * renderer already smoothed a few averages, but only for a handful of stages,
 * they were never reset, and there was no per-frame history to correlate a
 * visible hitch with. This module is the measurement floor the VR optimization
 * program is built on: one record per stereo frame, monotonic, allocation-free
 * on the hot path, exported only on demand.
 *
 * DESIGN CONTRACT
 * ---------------
 *   * DISABLED BY DEFAULT (PSX_FRAME_TRACE=0). Every entry point returns
 *     immediately while disabled, so an instrumented build costs one branch.
 *   * NO per-frame heap allocation and NO file I/O on the record path. The ring
 *     is static; the record holds fixed-size fields only. Export writes into a
 *     caller buffer, outside the frame.
 *   * Stages are supplied by the caller in MICROSECONDS, measured with whatever
 *     host clock it already uses. The module never reads a clock for durations,
 *     only for the record's open timestamp, through one injectable function
 *     pointer. That keeps the module dependency-free and unit-testable.
 *   * CPU submission time is NOT GPU execution time. Stages named "xr_*" are
 *     wall-clock waits; nothing here claims GPU completion.
 *   * Emulation/main thread only, like the OpenXR lifecycle it observes.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-frame stages, in the order one stereo frame produces them. The header
 * order IS the ABI; keep frame_trace.c's name table in sync. */
enum {
    PSX_TRACE_GUEST_SIM = 0,    /* guest work outside the stereo pair */
    PSX_TRACE_PAIR_TOTAL,       /* both eyes, pair open to publish */
    PSX_TRACE_EYE_BEGIN_L,      /* VRAM backup + checkpoint */
    PSX_TRACE_EYE_GUEST_L,      /* the plugin's guest draw */
    PSX_TRACE_EYE_END_L,        /* GPU flush, capture, VRAM restore */
    PSX_TRACE_EYE_RESTORE_L,    /* machine state restore */
    PSX_TRACE_EYE_BEGIN_R,
    PSX_TRACE_EYE_GUEST_R,
    PSX_TRACE_EYE_END_R,
    PSX_TRACE_EYE_RESTORE_R,
    PSX_TRACE_XR_WAIT_FRAME,    /* xrWaitFrame */
    PSX_TRACE_XR_BEGIN_FRAME,   /* xrBeginFrame */
    PSX_TRACE_XR_ACQUIRE,       /* swapchain acquire (both eyes) */
    PSX_TRACE_XR_WAIT_IMAGE,    /* xrWaitSwapchainImage (both eyes) */
    PSX_TRACE_XR_COPY,          /* eye blit into the runtime image */
    PSX_TRACE_XR_RELEASE,       /* swapchain release (both eyes) */
    PSX_TRACE_XR_END_FRAME,     /* xrEndFrame */
    /* How old the submitted pose was: xrLocateViews return to xrEndFrame
     * return. This is the pose the compositor reprojects from, so it bounds
     * the correction a head turn can need. */
    PSX_TRACE_POSE_AGE,
    /* Host time a HUD capture transaction costs (psx_mod_render_capture_rgba).
     * The wrist HUD captures the same guest slice twice per frame, once per
     * matte colour, and both are full render transactions on the frame's own
     * thread, so this is frame time the pair stats never showed. */
    PSX_TRACE_HUD_CAPTURE,
    /* The capture's two halves, so the cost can be aimed at: the transaction is
     * the checkpoint plus the guest HUD draw plus its verify, the readback is
     * the blit out of the pair target and the synchronous glReadPixels. */
    PSX_TRACE_HUD_DRAW,
    PSX_TRACE_HUD_READBACK,
    /* The guest scene rebuild one eye replays, split by the call the game hook
     * makes, so a hitch can be aimed at the guest call that grew instead of at
     * the whole eye. These are PAIR totals: both eyes write the same slots.
     * Written by the hook through psx_mod_frame_trace_stage_begin/end, which
     * only accepts them inside a traced stereo eye.
     *   game_env       draw environment, GP0 clear, ordering-table setup
     *   game_world     the world/level passes
     *   game_text      the generic text renderer
     *   game_traverse  scene traversal: level groups and entities
     *   game_icon      the HUD icon
     *   game_submit    DrawOTag: walks the ordering table and submits it
     *   game_laser     the hook's own per-eye raycast and line (host work)
     *   game_select    the hook's own level visibility override (the LVTX
     *                  entry filter): the eye-view AABB test and the leaf
     *                  queue it writes. It runs INSIDE the guest traversal,
     *                  and game_traverse is that call's wall time, so the
     *                  guest's own traversal work is game_traverse minus
     *                  game_select. Per eye and summed over the pair like the
     *                  rest, and the only one that nests. */
    PSX_TRACE_GAME_ENV,
    PSX_TRACE_GAME_WORLD,
    PSX_TRACE_GAME_TEXT,
    PSX_TRACE_GAME_TRAVERSE,
    PSX_TRACE_GAME_ICON,
    PSX_TRACE_GAME_SUBMIT,
    PSX_TRACE_GAME_LASER,
    PSX_TRACE_GAME_SELECT,
    PSX_TRACE_STAGE_COUNT
};

/* Record flags. A hitch is the interesting case, so absent conditions are
 * explicit: a stale frame is one that published no new pair. */
enum {
    PSX_FRAME_TRACE_STEREO_PUBLISHED = 1u << 0,
    PSX_FRAME_TRACE_STALE_PAIR       = 1u << 1,
    PSX_FRAME_TRACE_SHED             = 1u << 2,
    PSX_FRAME_TRACE_FAILED           = 1u << 3,
    PSX_FRAME_TRACE_XR_EMPTY         = 1u << 4,
    PSX_FRAME_TRACE_SCENE_TRANSITION = 1u << 5,
    PSX_FRAME_TRACE_IO_EVENT         = 1u << 6,
    PSX_FRAME_TRACE_NO_TELEMETRY     = 1u << 7,
    /* A stereo eye rendered from the pair's shared baseline instead of taking
     * its own machine snapshot (see PR3's pair-scoped checkpoint reuse). */
    PSX_FRAME_TRACE_BASELINE_REUSED  = 1u << 8,
    /* The XR frame carried no new pair, so the retained one was submitted (or
     * nothing was, when resubmission is off). */
    PSX_FRAME_TRACE_XR_REPEAT        = 1u << 9
};

enum {
    PSX_FRAME_TRACE_NOTE = 24,     /* scene / IO tag */
    PSX_FRAME_TRACE_REASON = 24    /* failure reason text */
};

typedef struct PSXFrameTraceRecord {
    uint64_t seq;              /* monotonic record id (starts at 1) */
    uint64_t open_ms;          /* host monotonic ms at open */
    uint64_t guest_frame;      /* guest frame counter, 0 if unknown */
    uint64_t guest_cycle;      /* guest cycle at open */
    uint64_t stereo_attempt;   /* stereo attempt id (0 if none) */
    uint64_t pair_id;          /* published pair id (0 if none) */
    uint64_t pair_age_ms;      /* ms since the last published pair (0 if none) */
    uint32_t stages_us[PSX_TRACE_STAGE_COUNT];
    uint32_t shed_skipped;     /* pair attempts shed immediately before this frame */
    uint32_t shed_streak_ms;   /* how long that shed streak had lasted */
    uint32_t prims;            /* guest draw primitives (0 = not measured) */
    uint32_t textured_prims;   /* of which textured */
    /* The same two counters per eye. `prims`/`textured_prims` above keep their
     * meaning (the last eye of the pair wrote them); a change to what ONE eye
     * selects needs these, because the pair's total hides which eye moved. */
    uint32_t prims_l, prims_r;
    uint32_t tex_prims_l, tex_prims_r;
    /* Guest cycles each eye's replay executed, the machine work the eye cost in
     * the emulated CPU rather than in host submission. 0 = not measured. */
    uint32_t eye_cycles_l, eye_cycles_r;
    uint32_t vram_leaks;       /* GPU writes outside the pass rect this eye */
    uint64_t tex_upload_bytes; /* texture bytes uploaded (0 = not measured) */
    /* Microseconds between this submission's XrFrameState.predictedDisplayTime
     * and the previous one, from the runtime's clock. Divided by the session's
     * target_period_us this is how many granted display periods one presented
     * pair consumed, which is the cadence the compositor actually saw. 0 on the
     * first submission of a session or when there was no frame this record. */
    uint32_t xr_display_delta_us;
    uint32_t flags;            /* PSX_FRAME_TRACE_* */
    int32_t  eye_failed;       /* last eye that failed, -1 if none */
    uint8_t  source;           /* 1 = stereo pair, 2 = native render */
    uint8_t  xr_submit_fresh;  /* 1 = carried a new pair, 0 = repeat/none */
    uint8_t  reserved[2];
    char     note[PSX_FRAME_TRACE_NOTE];
    char     failure[PSX_FRAME_TRACE_REASON];
} PSXFrameTraceRecord;

/* Injectable monotonic millisecond clock. NULL (the default) records 0; the
 * runtime installs psx_host_mono_ms(). Tests install a fake. */
typedef uint64_t (*PSXFrameTraceClock)(void);
void psx_frame_trace_set_clock(PSXFrameTraceClock fn);

/* Enables/disables capture. Disabling keeps the ring (read it, then reset).
 * Reads PSX_FRAME_TRACE / PSX_FRAME_TRACE_CAP once. */
void psx_frame_trace_configure_from_env(void);
void psx_frame_trace_set_enabled(int on);
int  psx_frame_trace_enabled(void);

/* Clears the ring, counters and any open record. */
void psx_frame_trace_reset(void);

/* Opens a record for the frame that is starting. Returns 1 if this call opened
 * it, 0 if capture is off or a record is already open (the caller must then NOT
 * commit). Cost is a branch while disabled. */
int psx_frame_trace_open(void);
/* Commits the open record, stamping pair age from the last published pair.
 * No-op when nothing is open. */
void psx_frame_trace_commit(void);

/* Adds `us` microseconds to `stage` of the open record. Ignores unknown stages;
 * saturates instead of wrapping. Cheap while closed. */
void psx_frame_trace_add(uint32_t stage, uint64_t us);

/* Per-frame annotations (open record). Each is a no-op when closed. */
void psx_frame_trace_ids(uint64_t guest_frame, uint64_t guest_cycle);
void psx_frame_trace_attempt(uint64_t attempt_id, uint32_t shed_skipped,
                             uint32_t shed_streak_ms);
void psx_frame_trace_result(int published, uint64_t pair_id, int eye_failed,
                            const char *failure);
void psx_frame_trace_source(uint8_t source);
/* Draw counters for `eye` (PSX_MOD_EYE_LEFT/0 or RIGHT/1). Also fills `prims`
 * and `textured_prims`, which therefore remain the last eye's values. */
void psx_frame_trace_draw(int eye, uint32_t prims, uint32_t textured);
/* Guest cycles one eye's replay executed (the transaction's own delta). */
void psx_frame_trace_eye_cycles(int eye, uint32_t cycles);
void psx_frame_trace_gpu(uint64_t tex_upload_bytes, uint32_t vram_leaks);
/* XR cadence annotation (open record): the runtime's predicted-display-time
 * delta for this submission and whether it carried a newly rendered pair. A
 * repeat also sets PSX_FRAME_TRACE_XR_REPEAT. No-op when closed. */
void psx_frame_trace_xr_cadence(uint32_t display_delta_us, int submit_fresh);
void psx_frame_trace_flag(uint32_t flag);
void psx_frame_trace_note(const char *tag);
void psx_frame_trace_io(const char *tag);   /* note + IO_EVENT flag */

/* Session-level metadata, emitted once in the export header rather than per
 * record: the OpenXR runtime actually in use and the target display period in
 * microseconds (0 = unknown, e.g. no headset). Without these a trace cannot be
 * interpreted later, because the same pair time is over budget at 90 Hz and
 * comfortably under it at 60 Hz. Change-checked internally, so calling it every
 * frame is a strcmp; it is a no-op while capture is off. */
void psx_frame_trace_session(const char *runtime, uint32_t target_period_us);

/* Queries. `capacity` is the caller's array length; returns how many records
 * were written (oldest first, at most the newest `capacity`). */
uint32_t psx_frame_trace_read(PSXFrameTraceRecord *out, uint32_t capacity);
uint32_t psx_frame_trace_capacity(void);
uint64_t psx_frame_trace_committed(void);   /* records committed since reset */
uint64_t psx_frame_trace_dropped(void);     /* records overwritten (ring wrap) */
uint64_t psx_frame_trace_missing(void);     /* commits with no stage at all */

/* Exports. Each returns the number of bytes the full text needs (like
 * snprintf); the buffer is always NUL-terminated when cap > 0. `max_records`
 * (0 = all retained) bounds the newest records emitted. */
int psx_frame_trace_to_json(char *buf, int cap, uint32_t max_records);
int psx_frame_trace_to_csv(char *buf, int cap, uint32_t max_records);
/* Chrome trace-event JSON (Perfetto compatible): one span per stage. */
int psx_frame_trace_to_chrome_json(char *buf, int cap, uint32_t max_records);

const char *psx_frame_trace_stage_name(uint32_t stage);
/* Renders the set bits as "a|b|c" into buf; always NUL-terminated. */
const char *psx_frame_trace_flag_names(uint32_t flags, char *buf, int cap);

#ifdef __cplusplus
}
#endif

#endif /* PSX_FRAME_TRACE_H */
