/* render_thread.c — render thread core. See render_thread.h and
 * docs/RENDER_THREAD.md.
 *
 * One producer (the emulation thread), one consumer (the render thread).
 * Positions are monotonic 64-bit byte counters; the ring index is pos & mask.
 * The consumer frees a record only after exec() returns, so a payload stays
 * valid for the whole replay. Locks only guard sleeping: the sleeper raises
 * its *_sleeping flag under the lock BEFORE re-checking its condition, and the
 * waker publishes its position (seq_cst) BEFORE reading that flag, so a wake
 * can never fall between a check and a wait. */

#include "render_thread.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
typedef SRWLOCK            rt_mutex;
typedef CONDITION_VARIABLE rt_cond;
static void mtx_init(rt_mutex *m) { InitializeSRWLock(m); }
static void mtx_lock(rt_mutex *m) { AcquireSRWLockExclusive(m); }
static void mtx_unlock(rt_mutex *m) { ReleaseSRWLockExclusive(m); }
static void cnd_init(rt_cond *c) { InitializeConditionVariable(c); }
static void cnd_wait(rt_cond *c, rt_mutex *m) { SleepConditionVariableSRW(c, m, INFINITE, 0); }
static void cnd_wait_ns(rt_cond *c, rt_mutex *m, uint64_t ns) {
    DWORD ms = (DWORD)((ns + 999999u) / 1000000u);
    SleepConditionVariableSRW(c, m, ms ? ms : 1, 0);
}
static void cnd_bcast(rt_cond *c) { WakeAllConditionVariable(c); }
static uint64_t now_ns(void) {
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
}
static void cpu_relax(void) { YieldProcessor(); }
#else
#  include <pthread.h>
#  include <sched.h>
#  if defined(__APPLE__)
#    include <pthread/qos.h>
#  endif
#  include <time.h>
typedef pthread_mutex_t rt_mutex;
typedef pthread_cond_t  rt_cond;
static void mtx_init(rt_mutex *m) { pthread_mutex_init(m, NULL); }
static void mtx_lock(rt_mutex *m) { pthread_mutex_lock(m); }
static void mtx_unlock(rt_mutex *m) { pthread_mutex_unlock(m); }
static void cnd_init(rt_cond *c) { pthread_cond_init(c, NULL); }
static void cnd_wait(rt_cond *c, rt_mutex *m) { pthread_cond_wait(c, m); }
static void cnd_wait_ns(rt_cond *c, rt_mutex *m, uint64_t ns) {
#  if defined(__APPLE__)
    struct timespec rel = { (time_t)(ns / 1000000000u), (long)(ns % 1000000000u) };
    pthread_cond_timedwait_relative_np(c, m, &rel);
#  else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_nsec + ns % 1000000000u;
    ts.tv_sec += (time_t)(ns / 1000000000u + t / 1000000000u);
    ts.tv_nsec = (long)(t % 1000000000u);
    pthread_cond_timedwait(c, m, &ts);
#  endif
}
static void cnd_bcast(rt_cond *c) { pthread_cond_broadcast(c); }
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void cpu_relax(void) {
#  if defined(__aarch64__)
    __asm__ __volatile__("yield");
#  elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#  endif
}
#endif

#define OP_PAD   0u      /* filler to the ring end; consumer skips it  */
#define F_FRAME  0x8000u /* op 0 with this flag: frame marker          */

/* Producer-side wake batching: a sleeping render thread is woken once this
 * much unread work is queued (frame ends, drains and acquires always wake). */
#define WAKE_BYTES   (128u * 1024u)
#define WAKE_RECORDS 256u
#define SPIN_LOOPS   4096   /* consumer polls this long before sleeping */

#define ACQ_RING 64

static struct {
    /* configuration */
    uint8_t *ring;
    uint64_t cap, mask;
    int      max_frames;
    RtExecFn exec;
    RtCtxFn  ctx;
    void    *user;
    RtTickFn tick;
    uint64_t tick_at;          /* consumer: next tick deadline (0 = none) */

    /* shared positions / counters */
    _Atomic uint64_t wpos, rpos;
    _Atomic uint64_t frames_produced, frames_consumed;
    _Atomic int      render_sleeping, emu_sleeping;
    _Atomic int      running;

    /* hand-off + lifecycle, guarded by mtx */
    rt_mutex mtx;
    rt_cond  cv_work, cv_emu;
    int      release_req, stop;
    _Atomic int released;      /* read unlocked by emu_wait's first check */

    /* producer-only */
    uint64_t pend_start, pend_size, pend_payload_off;
    uint16_t pend_op, pend_flags;
    uint32_t pend_payload;
    int      pending;
    uint64_t unwoken_bytes, unwoken_records;
    uint32_t seq;
    int      held;

    /* stats (producer unless noted) */
    uint64_t records, bytes, acquires, releases, oversize;
    uint64_t bp_waits, full_waits, bp_ns, full_ns, acq_ns, high_water;
    _Atomic uint64_t busy_ns, idle_ns;   /* consumer */
    _Atomic uint64_t bp_events;          /* producer writes, any thread reads */
    RtAcquireEvent acq_ring[ACQ_RING];
    uint64_t acq_n;

#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
} R;

#if defined(_MSC_VER)
static __declspec(thread) int t_render;
#else
static __thread int t_render;
#endif

int rt_running(void)          { return atomic_load(&R.running); }
int rt_on_render_thread(void) { return t_render; }
int rt_held(void)             { return R.held; }
uint64_t rt_render_idle_ns(void) { return atomic_load(&R.idle_ns); }
uint64_t rt_now_ns(void) { return now_ns(); }
uint64_t rt_backpressure_events(void) { return atomic_load(&R.bp_events); }
void rt_tick_at(uint64_t deadline_ns) {
    if (!t_render || !deadline_ns) return;
    if (!R.tick_at || deadline_ns < R.tick_at) R.tick_at = deadline_ns;
}

/* Consumer: run tick when its deadline has passed. */
static void run_tick(void) {
    if (!R.tick || !R.tick_at) return;
    uint64_t now = now_ns();
    if (now < R.tick_at) return;
    R.tick_at = 0;
    uint64_t next = R.tick(R.user, now);
    if (next && (!R.tick_at || next < R.tick_at)) R.tick_at = next;
    atomic_fetch_add(&R.busy_ns, now_ns() - now);
}

static inline uint64_t align_up(uint64_t n) {
    return (n + (RT_ALIGN - 1)) & ~(uint64_t)(RT_ALIGN - 1);
}

/* ---- consumer ------------------------------------------------------------ */

static void wake_emu(void) {
    if (atomic_load(&R.emu_sleeping)) {
        mtx_lock(&R.mtx);
        cnd_bcast(&R.cv_emu);
        mtx_unlock(&R.mtx);
    }
}

/* Called with mtx held when the ring is empty: hand the context over while
 * the emulation thread asks for it. Returns with mtx held. */
static void serve_release_locked(void) {
    if (!R.release_req) return;
    mtx_unlock(&R.mtx);
    R.ctx(R.user, 0);
    mtx_lock(&R.mtx);
    atomic_store(&R.released, 1);
    cnd_bcast(&R.cv_emu);
    while (R.release_req && !R.stop) cnd_wait(&R.cv_work, &R.mtx);
    atomic_store(&R.released, 0);
    if (R.stop) return;              /* the stopper owns the context now */
    mtx_unlock(&R.mtx);
    R.ctx(R.user, 1);
    mtx_lock(&R.mtx);
}

#ifdef _WIN32
static DWORD WINAPI render_main(LPVOID arg)
#else
static void *render_main(void *arg)
#endif
{
    (void)arg;
    t_render = 1;
    R.ctx(R.user, 1);
    uint64_t r = atomic_load(&R.rpos);
    int have_ctx = 1;
    for (;;) {
        run_tick();
        uint64_t w = atomic_load_explicit(&R.wpos, memory_order_acquire);
        if (r == w) {
            int spun = 0;
            while (spun < SPIN_LOOPS &&
                   atomic_load_explicit(&R.wpos, memory_order_acquire) == r) {
                cpu_relax();
                spun++;
            }
            if (atomic_load_explicit(&R.wpos, memory_order_acquire) != r) continue;
            uint64_t t0 = now_ns();
            mtx_lock(&R.mtx);
            atomic_store(&R.render_sleeping, 1);
            while (atomic_load(&R.wpos) == r && !R.stop && !R.release_req) {
                if (R.tick_at) {   /* timed work due: sleep no later than it */
                    uint64_t now = now_ns();
                    if (now >= R.tick_at) break;
                    cnd_wait_ns(&R.cv_work, &R.mtx, R.tick_at - now);
                } else {
                    cnd_wait(&R.cv_work, &R.mtx);
                }
            }
            atomic_store(&R.render_sleeping, 0);
            if (R.release_req && atomic_load(&R.wpos) == r) {
                serve_release_locked();
                if (R.stop) have_ctx = 0;
            }
            int done = R.stop && atomic_load(&R.wpos) == r;
            mtx_unlock(&R.mtx);
            atomic_fetch_add(&R.idle_ns, now_ns() - t0);
            if (done) break;
            continue;
        }
        uint64_t t0 = now_ns();
        const RtCmd *c = (const RtCmd *)(R.ring + (r & R.mask));
        if (c->op == OP_PAD) {
            if (c->flags & F_FRAME)
                atomic_fetch_add(&R.frames_consumed, 1);
        } else {
            R.exec(R.user, c, (const uint8_t *)c + sizeof(RtCmd));
        }
        r += c->size;
        atomic_store(&R.rpos, r);
        atomic_fetch_add(&R.busy_ns, now_ns() - t0);
        wake_emu();
    }
    if (have_ctx) R.ctx(R.user, 0);
    t_render = 0;
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* ---- producer ------------------------------------------------------------ */

static void wake_render(int force) {
    if (!force && R.unwoken_bytes < WAKE_BYTES && R.unwoken_records < WAKE_RECORDS)
        return;
    R.unwoken_bytes = R.unwoken_records = 0;
    if (atomic_load(&R.render_sleeping)) {
        mtx_lock(&R.mtx);
        cnd_bcast(&R.cv_work);
        mtx_unlock(&R.mtx);
    }
}

/* Block until pred() holds; the render thread wakes us after every record. */
typedef int (*Pred)(uint64_t arg);
static void emu_wait(Pred pred, uint64_t arg) {
    if (pred(arg)) return;
    wake_render(1);
    mtx_lock(&R.mtx);
    atomic_store(&R.emu_sleeping, 1);
    while (!pred(arg)) cnd_wait(&R.cv_emu, &R.mtx);
    atomic_store(&R.emu_sleeping, 0);
    mtx_unlock(&R.mtx);
}

static int pred_space(uint64_t need_end) {   /* need_end: wpos after the write */
    return need_end - atomic_load(&R.rpos) <= R.cap;
}
static int pred_drained(uint64_t w) { return atomic_load(&R.rpos) == w; }
static int pred_frames(uint64_t limit) {
    return atomic_load(&R.frames_produced) - atomic_load(&R.frames_consumed) <= limit;
}
static int pred_released(uint64_t unused) { (void)unused; return atomic_load(&R.released); }

void *rt_cmd_begin(uint16_t op, uint16_t flags, uint32_t payload_bytes) {
    if (!rt_running() || R.pending || op == OP_PAD) return NULL;
    uint64_t size = align_up(sizeof(RtCmd) + (uint64_t)payload_bytes);
    if (size > R.cap / 2) { R.oversize++; return NULL; }
    uint64_t w = atomic_load_explicit(&R.wpos, memory_order_relaxed);
    uint64_t off = w & R.mask;
    uint64_t pad = (off + size > R.cap) ? R.cap - off : 0;
    if (!pred_space(w + pad + size)) {
        uint64_t t0 = now_ns();
        R.full_waits++;
        emu_wait(pred_space, w + pad + size);
        R.full_ns += now_ns() - t0;
        atomic_fetch_add(&R.bp_events, 1);
    }
    if (pad) {
        RtCmd *p = (RtCmd *)(R.ring + off);
        p->size = (uint32_t)pad; p->op = OP_PAD; p->flags = 0; p->payload = 0; p->seq = 0;
        w += pad;
        off = 0;
    }
    RtCmd *c = (RtCmd *)(R.ring + off);
    c->size = (uint32_t)size;
    c->op = op;
    c->flags = flags;
    c->payload = payload_bytes;
    c->seq = R.seq++;
    R.pend_start = w;
    R.pend_size = size;
    R.pending = 1;
    return (uint8_t *)c + sizeof(RtCmd);
}

void rt_cmd_commit(void) {
    if (!R.pending) return;
    R.pending = 0;
    uint64_t end = R.pend_start + R.pend_size;
    atomic_store(&R.wpos, end);
    R.records++;
    R.bytes += R.pend_size;
    uint64_t depth = end - atomic_load(&R.rpos);
    if (depth > R.high_water) R.high_water = depth;
    R.unwoken_bytes += R.pend_size;
    R.unwoken_records++;
    wake_render(0);
}

static void commit_marker(uint16_t flags) {
    uint64_t w = atomic_load_explicit(&R.wpos, memory_order_relaxed);
    uint64_t off = w & R.mask;
    uint64_t size = sizeof(RtCmd);
    if (!pred_space(w + size)) emu_wait(pred_space, w + size);
    RtCmd *c = (RtCmd *)(R.ring + off);   /* 16 bytes always fit before the end */
    c->size = (uint32_t)size; c->op = OP_PAD; c->flags = flags; c->payload = 0; c->seq = R.seq++;
    atomic_store(&R.wpos, w + size);
}

void rt_frame_end(void) {
    if (!rt_running()) return;
    if (R.held) {
        /* Nothing is queued while the context is held; count the frame as
         * produced and consumed so the in-flight bound stays meaningful. */
        atomic_fetch_add(&R.frames_produced, 1);
        atomic_fetch_add(&R.frames_consumed, 1);
        return;
    }
    atomic_fetch_add(&R.frames_produced, 1);
    commit_marker(F_FRAME);
    wake_render(1);
    if (!pred_frames((uint64_t)R.max_frames)) {
        uint64_t t0 = now_ns();
        R.bp_waits++;
        emu_wait(pred_frames, (uint64_t)R.max_frames);
        R.bp_ns += now_ns() - t0;
        atomic_fetch_add(&R.bp_events, 1);
    }
}

int rt_frames_ahead(void) {
    int64_t d = (int64_t)(atomic_load(&R.frames_produced) -
                          atomic_load(&R.frames_consumed)) - 1;
    return d < 0 ? 0 : (int)d;
}

void rt_drain(void) {
    if (!rt_running() || R.held) return;
    uint64_t w = atomic_load(&R.wpos);
    emu_wait(pred_drained, w);
}

void rt_acquire(const char *reason) {
    if (!rt_running() || R.held) return;
    uint64_t t0 = now_ns();
    rt_drain();
    mtx_lock(&R.mtx);
    R.release_req = 1;
    cnd_bcast(&R.cv_work);
    mtx_unlock(&R.mtx);
    emu_wait(pred_released, 0);
    R.ctx(R.user, 1);
    R.held = 1;
    uint64_t dt = now_ns() - t0;
    R.acquires++;
    R.acq_ns += dt;
    RtAcquireEvent *e = &R.acq_ring[R.acq_n++ % ACQ_RING];
    e->frame = atomic_load(&R.frames_produced);
    e->wait_ns = dt;
    e->reason = reason ? reason : "?";
}

void rt_release(void) {
    if (!rt_running() || !R.held) return;
    R.ctx(R.user, 0);
    mtx_lock(&R.mtx);
    R.release_req = 0;
    cnd_bcast(&R.cv_work);
    mtx_unlock(&R.mtx);
    R.held = 0;
    R.releases++;
}

/* ---- lifecycle ----------------------------------------------------------- */

int rt_start(const RtConfig *cfg) {
    if (rt_running() || !cfg || !cfg->exec || !cfg->ctx) return 0;
    size_t cap = cfg->ring_bytes;
    if (cap < (1u << 20) || (cap & (cap - 1))) return 0;
    uint8_t *ring = (uint8_t *)malloc(cap);
    if (!ring) return 0;
    memset(&R, 0, sizeof(R));
    R.ring = ring;
    R.cap = cap;
    R.mask = cap - 1;
    R.max_frames = cfg->max_frames < 1 ? 1 : cfg->max_frames;
    R.exec = cfg->exec;
    R.ctx = cfg->ctx;
    R.user = cfg->user;
    R.tick = cfg->tick;
    R.tick_at = 0;
    mtx_init(&R.mtx);
    cnd_init(&R.cv_work);
    cnd_init(&R.cv_emu);
    /* The context leaves this thread before the render thread takes it. */
    R.ctx(R.user, 0);
    atomic_store(&R.running, 1);
#ifdef _WIN32
    R.thread = CreateThread(NULL, 0, render_main, NULL, 0, NULL);
    int ok = R.thread != NULL;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
#  if defined(__APPLE__)
    /* A plain pthread gets the default QoS and, on a busy host, efficiency
     * cores. The render thread is on the frame's critical path, like the
     * emulation (main) thread, so it asks for the same class. */
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
#  endif
    int ok = pthread_create(&R.thread, &attr, render_main, NULL) == 0;
    pthread_attr_destroy(&attr);
#endif
    if (!ok) {
        atomic_store(&R.running, 0);
        R.ctx(R.user, 1);
        free(R.ring);
        R.ring = NULL;
        return 0;
    }
    return 1;
}

void rt_stop(void) {
    if (!rt_running()) return;
    int was_held = R.held;
    if (!was_held) rt_drain();
    mtx_lock(&R.mtx);
    R.stop = 1;
    R.release_req = 0;
    cnd_bcast(&R.cv_work);
    mtx_unlock(&R.mtx);
#ifdef _WIN32
    WaitForSingleObject(R.thread, INFINITE);
    CloseHandle(R.thread);
#else
    pthread_join(R.thread, NULL);
#endif
    atomic_store(&R.running, 0);
    R.held = 0;
    if (!was_held) R.ctx(R.user, 1);
    free(R.ring);
    R.ring = NULL;
}

void rt_get_stats(RtStats *o) {
    memset(o, 0, sizeof(*o));
    o->records = R.records;
    o->bytes = R.bytes;
    o->frames_produced = atomic_load(&R.frames_produced);
    o->frames_consumed = atomic_load(&R.frames_consumed);
    o->acquires = R.acquires;
    o->releases = R.releases;
    o->oversize = R.oversize;
    o->backpressure_waits = R.bp_waits;
    o->ring_full_waits = R.full_waits;
    o->backpressure_ns = R.bp_ns;
    o->ring_full_ns = R.full_ns;
    o->acquire_ns = R.acq_ns;
    o->render_busy_ns = atomic_load(&R.busy_ns);
    o->render_idle_ns = atomic_load(&R.idle_ns);
    o->ring_high_water = R.high_water;
    o->max_frames = R.max_frames;
    o->running = rt_running();
    o->held = R.held;
}

int rt_acquire_events(RtAcquireEvent *out, int cap) {
    int n = 0;
    for (uint64_t i = 0; i < R.acq_n && i < ACQ_RING && n < cap; i++)
        out[n++] = R.acq_ring[(R.acq_n - 1 - i) % ACQ_RING];
    return n;
}
