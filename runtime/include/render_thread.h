/* render_thread.h — opt-in render thread core ([video] render_thread).
 *
 * The emulation thread records host draw work into a single-producer /
 * single-consumer command ring; the render thread owns the graphics context
 * and replays the ring in order. This header is backend-neutral: it knows
 * nothing about OpenGL. The OpenGL backend (gpu_gl_renderer.c) supplies the
 * command set (exec callback) and the context hand-off (ctx callback).
 *
 * Model (docs/RENDER_THREAD.md):
 *   - Records are appended with rt_cmd_begin()/rt_cmd_commit() and replayed in
 *     order by exec() on the render thread. A record never splits across the
 *     ring end; payloads are copied, so the producer may reuse its buffers.
 *   - rt_frame_end() closes a guest frame. At most max_frames closed frames may
 *     wait in the ring; past that the emulation thread blocks (backpressure).
 *     While replaying, rt_frames_ahead() counts the complete frames already
 *     recorded after the one being replayed, so the backend can skip
 *     presenting a frame it is far behind on.
 *   - rt_acquire() is the one sync point: it waits until every record has been
 *     executed, has the render thread release the context, and makes it
 *     current on the calling (emulation) thread. The emulation thread then
 *     holds the context ("held") until rt_release() hands it back. While held,
 *     the backend calls its functions directly, exactly as with no render
 *     thread at all.
 *   - No lock is held while exec() runs. Locks only guard sleeping/waking.
 *
 * Every function except the rt_on_render_thread() / rt_frames_ahead() /
 * rt_render_idle_ns()
 * queries is emulation-thread only. */
#ifndef PSX_RENDER_THREAD_H
#define PSX_RENDER_THREAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record header. size covers header + payload, rounded up to RT_ALIGN. */
typedef struct RtCmd {
    uint32_t size;
    uint16_t op;      /* backend opcode; 0 is reserved (padding/frame) */
    uint16_t flags;
    uint32_t payload; /* payload bytes actually used */
    uint32_t seq;     /* low 32 bits of the record sequence number */
} RtCmd;

#define RT_ALIGN 16u

/* exec: replay one record on the render thread. ctx: make the graphics
 * context current on the calling thread (current != 0) or release it
 * (current == 0; the backend flushes first). */
typedef void (*RtExecFn)(void *user, const RtCmd *cmd, const void *payload);
typedef void (*RtCtxFn)(void *user, int current);
/* tick (optional): timed work on the render thread while it holds the
 * context (frame generation). Called with the current time before a record
 * is executed and while the ring is empty, once the deadline it last
 * returned has passed; returns the next deadline (rt_now_ns clock), 0 for
 * none. An idle render thread sleeps no later than that deadline. */
typedef uint64_t (*RtTickFn)(void *user, uint64_t now_ns);

typedef struct RtConfig {
    size_t   ring_bytes;   /* power of two; >= 1 MiB */
    int      max_frames;   /* closed frames allowed in flight (>= 1) */
    RtExecFn exec;
    RtCtxFn  ctx;
    void    *user;
    RtTickFn tick;         /* optional (NULL) */
} RtConfig;

/* Start: the context must be current on the calling thread; on success it is
 * released here and made current on the new render thread. Returns 1 on
 * success, 0 (nothing started, context still current) on failure. */
int  rt_start(const RtConfig *cfg);
/* Stop: drain every record, join the thread and make the context current on
 * the calling thread again. Safe when not running. */
void rt_stop(void);
int  rt_running(void);
int  rt_on_render_thread(void);

/* Append a record with room for payload_bytes; returns the payload pointer
 * (16-byte aligned) or NULL when the record can never fit (larger than half
 * the ring) — the caller then acquires and runs the work directly. Blocks
 * while the ring is full. Must be followed by rt_cmd_commit() before any other
 * rt_* call. op 0 is reserved. */
void *rt_cmd_begin(uint16_t op, uint16_t flags, uint32_t payload_bytes);
void  rt_cmd_commit(void);

/* Close a guest frame: enqueue a frame marker, then block while more than
 * max_frames closed frames are unconsumed. */
void rt_frame_end(void);

/* Render-thread query while executing: how many complete frames are queued
 * after the frame whose record is being executed (0 = it is the newest). */
int  rt_frames_ahead(void);
/* Render-thread query: the consumer's running idle total (waiting for
 * records, or parked while the emulation thread holds the context). */
uint64_t rt_render_idle_ns(void);
/* Monotonic nanoseconds (the clock tick deadlines use). */
uint64_t rt_now_ns(void);
/* Any thread: running count of times the emulation thread blocked on the
 * in-flight bound or a full ring (it waited on the render thread). */
uint64_t rt_backpressure_events(void);
/* Render thread, from tick or exec: ask for tick at `deadline_ns` (earliest
 * wins over the one tick last returned). */
void rt_tick_at(uint64_t deadline_ns);

/* Sync point. Drain, then move the context to the calling thread (held).
 * reason is a static string kept in the acquire ring. No-op when already held
 * or not running. */
void rt_acquire(const char *reason);
/* Hand the context back to the render thread. No-op unless held. */
void rt_release(void);
int  rt_held(void);
/* Wait until every committed record has executed (context stays where it
 * is). Used by tests and by rt_acquire. */
void rt_drain(void);

/* Statistics (emulation thread). */
typedef struct RtStats {
    uint64_t records, bytes, frames_produced, frames_consumed;
    uint64_t acquires, releases, oversize;
    uint64_t backpressure_waits, ring_full_waits;
    uint64_t backpressure_ns, ring_full_ns, acquire_ns;
    uint64_t render_busy_ns, render_idle_ns;
    uint64_t ring_high_water;
    int      max_frames, running, held;
} RtStats;
void rt_get_stats(RtStats *out);

/* Ring of the most recent sync points (newest first). Returns the count. */
typedef struct RtAcquireEvent {
    uint64_t frame;      /* frames_produced at the time */
    uint64_t wait_ns;    /* drain + hand-off time */
    const char *reason;
} RtAcquireEvent;
int rt_acquire_events(RtAcquireEvent *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* PSX_RENDER_THREAD_H */
