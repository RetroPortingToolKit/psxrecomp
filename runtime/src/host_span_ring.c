/* host_span_ring.c - always-on wall-time frame + span rings. See host_span_ring.h. */
#include "host_span_ring.h"

#include <stdatomic.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
static uint64_t qpc_now_us(void) {
    static _Atomic uint64_t frequency;
    uint64_t freq = atomic_load_explicit(&frequency, memory_order_relaxed);
    LARGE_INTEGER c;
    if (!freq) {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        freq = (uint64_t)value.QuadPart;
        atomic_store_explicit(&frequency, freq, memory_order_relaxed);
    }
    QueryPerformanceCounter(&c);
    return ((uint64_t)c.QuadPart / freq) * 1000000ull +
           ((uint64_t)c.QuadPart % freq) * 1000000ull / freq;
}
static uint64_t this_thread_id(void) { return (uint64_t)GetCurrentThreadId(); }
/* Cumulative page faults and disk bytes read by this process. */
static void proc_counters(uint32_t *faults, uint32_t *io_kb) {
    PROCESS_MEMORY_COUNTERS pmc;
    IO_COUNTERS io;
    *faults = 0; *io_kb = 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) *faults = (uint32_t)pmc.PageFaultCount;
    if (GetProcessIoCounters(GetCurrentProcess(), &io)) *io_kb = (uint32_t)(io.ReadTransferCount >> 10);
}
#else
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
static uint64_t qpc_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
static uint64_t this_thread_id(void) { return (uint64_t)(uintptr_t)pthread_self(); }
static void proc_counters(uint32_t *faults, uint32_t *io_kb) {
    struct rusage ru;
    *faults = 0; *io_kb = 0;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        *faults = (uint32_t)(ru.ru_minflt + ru.ru_majflt);
        *io_kb = (uint32_t)(ru.ru_inblock / 2);   /* 512-byte blocks */
    }
}
#endif

static _Atomic uint64_t s_origin_us;
static _Atomic uint64_t s_emu_tid;
static _Atomic uint32_t s_cur_frame;

/* A claimed sequence is not necessarily published yet. Slot ownership protects
 * both payload and sequence from concurrent wrap/read/write. Never wait on a
 * diagnostic reader: contended samples are dropped and getters skip holes. */
typedef struct {
    _Atomic uint32_t busy;
    uint64_t published;
    HostSpan value;
} SpanSlot;
static SpanSlot s_spans[HOST_SPAN_RING_CAP];
static _Atomic uint64_t s_span_seq;

typedef struct { uint64_t t_us; uint32_t frame; uint32_t interp; uint32_t checks; uint32_t faults; uint32_t io_kb; uint32_t pad; } FrameStamp;
typedef struct {
    _Atomic uint32_t busy;
    uint64_t published;
    FrameStamp value;
} FrameSlot;
static FrameSlot s_frames[HOST_FRAME_STAMP_CAP];
static _Atomic uint64_t s_frame_seq;

static int slot_acquire(_Atomic uint32_t *busy) {
    uint32_t expected = 0;
    return atomic_compare_exchange_strong_explicit(busy, &expected, 1,
                                                   memory_order_acquire, memory_order_relaxed);
}

static void slot_release(_Atomic uint32_t *busy) {
    atomic_store_explicit(busy, 0, memory_order_release);
}

uint64_t host_clock_us(void) {
    uint64_t now = qpc_now_us();
    uint64_t origin = atomic_load_explicit(&s_origin_us, memory_order_relaxed);
    if (!origin) {
        uint64_t expect = 0;
        /* First caller fixes the origin; "now | 1" keeps "unset" (0) distinguishable. */
        uint64_t mine = now ? now : 1;
        if (atomic_compare_exchange_strong(&s_origin_us, &expect, mine))
            origin = mine;
        else
            origin = expect;
    }
    return now >= origin ? now - origin : 0;
}

uint64_t host_span_begin(void) { return host_clock_us(); }

void host_span_record(HostSpanKind kind, uint64_t t0_us, uint64_t dur_us, uint32_t arg) {
    if (dur_us < HOST_SPAN_MIN_US) return;
    uint64_t n = atomic_fetch_add_explicit(&s_span_seq, 1, memory_order_relaxed);
    SpanSlot *slot = &s_spans[n % HOST_SPAN_RING_CAP];
    if (!slot_acquire(&slot->busy)) return;
    /* A suspended older writer must not replace a newer lap of this slot. */
    if (slot->published > n) { slot_release(&slot->busy); return; }
    HostSpan *s = &slot->value;
    s->t0_us = t0_us;
    s->dur_us = dur_us > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dur_us;
    s->frame = atomic_load_explicit(&s_cur_frame, memory_order_relaxed);
    s->arg = arg;
    s->kind = (uint16_t)kind;
    s->emu = this_thread_id() == atomic_load_explicit(&s_emu_tid, memory_order_relaxed);
    slot->published = n + 1;
    slot_release(&slot->busy);
}

void host_span_end(HostSpanKind kind, uint64_t t0_us, uint32_t arg) {
    uint64_t now = host_clock_us();
    host_span_record(kind, t0_us, now >= t0_us ? now - t0_us : 0, arg);
}

uint64_t host_span_total(void) { return atomic_load_explicit(&s_span_seq, memory_order_relaxed); }

int host_span_get(uint64_t seq, HostSpan *out) {
    uint64_t total = host_span_total();
    if (!out || seq >= total || total - seq > HOST_SPAN_RING_CAP) return 0;
    SpanSlot *slot = &s_spans[seq % HOST_SPAN_RING_CAP];
    if (!slot_acquire(&slot->busy)) return 0;
    const int ready = slot->published == seq + 1;
    if (ready) *out = slot->value;
    slot_release(&slot->busy);
    return ready;
}

const char *host_span_kind_name(unsigned kind) {
    static const char *const names[HSPAN_KIND_COUNT] = {
        "none", "autocapture_tick", "provider_poll", "publish_commit", "loader_rescan",
        "dll_register", "lazy_load", "capture_on_dma", "capture_before_dma", "ac_request", "ac_done_join",
        "ac_prepare_image", "ac_child", "overlay_dispatch", "mod_work", "frame_present", "disc_read",
    };
    return kind < HSPAN_KIND_COUNT ? names[kind] : "?";
}

void host_frame_stamp(uint32_t frame, uint32_t interp_insns, uint32_t static_checks) {
    atomic_store_explicit(&s_emu_tid, this_thread_id(), memory_order_relaxed);
    atomic_store_explicit(&s_cur_frame, frame, memory_order_relaxed);
    uint64_t n = atomic_fetch_add_explicit(&s_frame_seq, 1, memory_order_relaxed);
    FrameSlot *slot = &s_frames[n % HOST_FRAME_STAMP_CAP];
    if (!slot_acquire(&slot->busy)) return;
    if (slot->published > n) { slot_release(&slot->busy); return; }
    FrameStamp *f = &slot->value;
    f->t_us = host_clock_us();
    f->frame = frame;
    f->interp = interp_insns;
    f->checks = static_checks;
    proc_counters(&f->faults, &f->io_kb);
    slot->published = n + 1;
    slot_release(&slot->busy);
}

uint64_t host_frame_stamp_total(void) { return atomic_load_explicit(&s_frame_seq, memory_order_relaxed); }

int host_frame_stamp_get(uint64_t seq, uint32_t *frame, uint64_t *t_us, uint32_t *interp_insns,
                         uint32_t *static_checks, uint32_t *page_faults, uint32_t *io_read_kb) {
    uint64_t total = host_frame_stamp_total();
    if (seq >= total || total - seq > HOST_FRAME_STAMP_CAP) return 0;
    FrameSlot *slot = &s_frames[seq % HOST_FRAME_STAMP_CAP];
    if (!slot_acquire(&slot->busy)) return 0;
    if (slot->published != seq + 1) { slot_release(&slot->busy); return 0; }
    const FrameStamp *f = &slot->value;
    if (frame) *frame = f->frame;
    if (t_us) *t_us = f->t_us;
    if (interp_insns) *interp_insns = f->interp;
    if (static_checks) *static_checks = f->checks;
    if (page_faults) *page_faults = f->faults;
    if (io_read_kb) *io_read_kb = f->io_kb;
    slot_release(&slot->busy);
    return 1;
}
