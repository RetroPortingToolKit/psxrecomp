/* starvation_ring.c — diagnostic-only.
 *
 * Always-on ring of the last 16K SIO/MMIO events with full SIO state
 * snapshot at each event. Watchdog flushes the ring to a file and
 * aborts cleanly when host wall-clock since the last
 * debug_server_poll exceeds the threshold (TCP listener has stopped
 * draining the connection — symptom of dispatch-loop starvation).
 *
 * The dumped file is plain JSON-lines (one event per line) so it can
 * be inspected with grep/jq even after the process terminated. */

#include "starvation_ring.h"
#include "psx_bss.h"
#include "psx_cycles.h"
#include "psx_netplay.h"
#include "crash_trace.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static uint64_t host_us_now(void) {
    LARGE_INTEGER f, c;
    static LARGE_INTEGER freq = {0};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (uint64_t)((c.QuadPart * 1000000ULL) / freq.QuadPart);
}
#else
#include <sys/time.h>
static uint64_t host_us_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
#endif

extern uint32_t g_debug_current_func_addr;
extern uint32_t g_debug_last_store_pc;
extern uint32_t i_stat;
extern uint32_t i_mask;
extern int psx_get_in_exception(void);

#if STARVATION_RING_ENABLED

extern uint64_t s_frame_count;   /* debug_server.c */

static PSX_BSS StarvationEntry s_ring[STARVATION_RING_CAP];
static uint64_t        s_seq = 0;
/* Written by the emu thread AND the debug-server IO thread
 * (send_all_blocking); read by the emu thread's watchdog. Atomic so the
 * cross-thread store/load is single-copy atomic on every target. */
static _Atomic uint64_t s_last_heartbeat_us = 0;
static int             s_dump_done = 0;
/* Emu thread only: wall time of the previous watchdog check. */
static uint64_t        s_prev_check_us = 0;
/* Stamped only when the heartbeat runs ON the emu thread. The IO thread and the
 * freeze-dump thread also stamp s_last_heartbeat_us (the latter while the emu
 * thread is suspended), which would split or hide a host block. */
static _Atomic uint64_t s_emu_heartbeat_us = 0;
#ifdef _WIN32
static DWORD s_emu_tid = 0;
static int stall_on_emu_thread(void) { return !s_emu_tid || GetCurrentThreadId() == s_emu_tid; }
#else
static pthread_t s_emu_pthread;
static int s_emu_pthread_set = 0;
static int stall_on_emu_thread(void) { return !s_emu_pthread_set || pthread_equal(pthread_self(), s_emu_pthread); }
#endif

/* ---- Stall ring: two writers (sampler thread, emu thread), any reader.
 * A writer reserves a slot with fetch_add, invalidates its seq, fills it and
 * publishes seq last; readers copy and re-check seq around the copy. */
static PSX_BSS StallSample s_stall[STALL_RING_CAP];
static _Atomic uint64_t s_stall_seq = 0;
static _Atomic uint32_t s_stall_episode = 0;
static int s_stall_started = 0;
static unsigned s_stall_reports = 0;   /* emu thread only */
#define STALL_REPORT_MIN_US 1000000ull
#define STALL_REPORT_LIMIT  8u
#define MAX_FRAME_NAME      320

static void stall_record(uint32_t kind, uint64_t gap_us,
                         const uint64_t *frames, unsigned nframes) {
    uint64_t seq = atomic_fetch_add_explicit(&s_stall_seq, 1, memory_order_relaxed);
    StallSample *e = &s_stall[seq % STALL_RING_CAP];
    e->seq = UINT64_MAX;
    atomic_thread_fence(memory_order_release);
    e->host_us = host_us_now();
    e->gap_us = gap_us;
    e->psx_cycle_count = psx_get_cycle_count();
    e->frame_count = s_frame_count;
    e->episode = atomic_load_explicit(&s_stall_episode, memory_order_relaxed);
    e->current_func = g_debug_current_func_addr;
    e->kind = kind;
    if (nframes > STALL_MAX_FRAMES) nframes = STALL_MAX_FRAMES;
    e->nframes = nframes;
    for (unsigned i = 0; i < STALL_MAX_FRAMES; i++)
        e->frames[i] = (frames && i < nframes) ? frames[i] : 0;
    atomic_thread_fence(memory_order_release);
    e->seq = seq;
}

uint64_t stall_ring_total(void) {
    return atomic_load_explicit(&s_stall_seq, memory_order_acquire);
}

int stall_ring_get(uint64_t seq, StallSample *out) {
    uint64_t total = stall_ring_total();
    if (seq >= total || total - seq > STALL_RING_CAP) return 0;
    const StallSample *e = &s_stall[seq % STALL_RING_CAP];
    if (e->seq != seq) return 0;
    atomic_thread_fence(memory_order_acquire);
    *out = *e;
    atomic_thread_fence(memory_order_acquire);
    return e->seq == seq && out->seq == seq;
}

static const char *stall_kind_name(uint32_t k) {
    switch (k) {
    case STALL_EMU_SAMPLE:  return "EMU_SAMPLE";
    case STALL_SAMPLER_GAP: return "SAMPLER_GAP";
    case STALL_HOST_BLOCK:  return "HOST_BLOCK";
    case STALL_GUEST_ABORT: return "GUEST_ABORT";
    default: return "?";
    }
}

void stall_format_frame(uint64_t addr, char *out, size_t cap) {
    if (!out || !cap) return;
#ifdef _WIN32
    HMODULE mod = NULL;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &mod) && mod &&
        GetModuleFileNameA(mod, path, sizeof(path))) {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '\\' || *p == '/') base = p + 1;
        snprintf(out, cap, "%s+0x%llX", base,
                 (unsigned long long)(addr - (uint64_t)(uintptr_t)mod));
        return;
    }
#endif
    snprintf(out, cap, "0x%016llX", (unsigned long long)addr);
}

/* One JSON object per stall entry (no trailing newline). */
static void stall_fprint(FILE *f, const StallSample *s) {
    fprintf(f, "{\"stall_seq\":%llu,\"kind\":\"%s\",\"us\":%llu,\"gap_us\":%llu,"
               "\"episode\":%u,\"cyc\":%llu,\"frame\":%llu,\"func\":\"0x%08X\","
               "\"frames\":[",
            (unsigned long long)s->seq, stall_kind_name(s->kind),
            (unsigned long long)s->host_us, (unsigned long long)s->gap_us,
            s->episode, (unsigned long long)s->psx_cycle_count,
            (unsigned long long)s->frame_count, s->current_func);
    for (unsigned i = 0; i < s->nframes; i++) {
        char name[MAX_FRAME_NAME];
        stall_format_frame(s->frames[i], name, sizeof(name));
        fprintf(f, "%s\"%s\"", i ? "," : "", name);
    }
    fputs("]}", f);
}

static uint64_t heartbeat_load(void);

#ifdef _WIN32
static HANDLE s_stall_emu_thread = NULL;

/* Suspend, copy the live stack, resume, then unwind the COPY. Nothing that can
 * take a user-mode lock runs while the emu thread is suspended: a thread
 * stalled inside the loader (LoadLibrary of an overlay shard) holds the loader
 * lock that RtlLookupFunctionEntry may need, and suspending it there would
 * otherwise deadlock the sampler. */
static unsigned stall_capture(uint64_t *frames) {
    if (!s_stall_emu_thread) return 0;
    static uint8_t stack_copy[64 * 1024];
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(s_stall_emu_thread) == (DWORD)-1) return 0;
    if (!GetThreadContext(s_stall_emu_thread, &ctx)) {
        ResumeThread(s_stall_emu_thread);
        return 0;
    }
#if defined(_M_X64) || defined(__x86_64__)
    uint64_t sp = ctx.Rsp;
    size_t len = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((const void *)(uintptr_t)sp, &mbi, sizeof(mbi)) &&
        mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        uint64_t end = (uint64_t)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        len = (size_t)(end - sp);
        if (len > sizeof(stack_copy)) len = sizeof(stack_copy);
        memcpy(stack_copy, (const void *)(uintptr_t)sp, len);
    }
    ResumeThread(s_stall_emu_thread);

    const uint64_t lo = (uint64_t)(uintptr_t)stack_copy, hi = lo + len;
    const uint64_t delta = lo - sp;
    unsigned n = 0;
    ctx.Rsp += delta;
    if (ctx.Rbp >= sp && ctx.Rbp < sp + len) ctx.Rbp += delta;
    while (n < STALL_MAX_FRAMES && ctx.Rip) {
        frames[n++] = ctx.Rip;
        if (!len || ctx.Rsp < lo || ctx.Rsp + sizeof(DWORD64) > hi) break;
        uint64_t previous = ctx.Rsp;
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        if (fn) {
            void *handler = NULL;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx,
                             &handler, &establisher, NULL);
        } else {
            ctx.Rip = *(const DWORD64 *)(uintptr_t)ctx.Rsp;
            ctx.Rsp += sizeof(DWORD64);
        }
        /* Restored frame pointers hold real-stack addresses. */
        if (ctx.Rbp >= sp && ctx.Rbp < sp + len) ctx.Rbp += delta;
        if (ctx.Rsp <= previous || ctx.Rsp > hi) break;
    }
    return n;
#else
    ResumeThread(s_stall_emu_thread);
    frames[0] = ctx.Eip;
    return 1;
#endif
}
#else
static unsigned stall_capture(uint64_t *frames) { (void)frames; return 0; }
#endif

static void stall_sampler_loop(void) {
    uint64_t prev_tick = host_us_now(), last_sample = 0;
    int in_episode = 0;
    for (;;) {
#ifdef _WIN32
        Sleep((DWORD)(STALL_TICK_US / 1000ull));
#else
        usleep((useconds_t)STALL_TICK_US);
#endif
        uint64_t now = host_us_now();
        if (now > prev_tick && now - prev_tick >= STALL_TICK_US + STALL_SAMPLER_GAP_US)
            stall_record(STALL_SAMPLER_GAP, now - prev_tick, NULL, 0);
        prev_tick = now;
        uint64_t last = atomic_load_explicit(&s_emu_heartbeat_us, memory_order_relaxed);
        if (!last || now <= last || now - last < STALL_SAMPLE_AFTER_US) {
            in_episode = 0;
            continue;
        }
        if (!in_episode) {
            in_episode = 1;
            last_sample = 0;
            atomic_fetch_add_explicit(&s_stall_episode, 1, memory_order_relaxed);
        }
        if (last_sample && now - last_sample < STALL_SAMPLE_EVERY_US) continue;
        last_sample = now;
        uint64_t frames[STALL_MAX_FRAMES];
        unsigned n = stall_capture(frames);
        stall_record(STALL_EMU_SAMPLE, now - last, frames, n);
    }
}

#ifdef _WIN32
static DWORD WINAPI stall_sampler_main(LPVOID arg) { (void)arg; stall_sampler_loop(); return 0; }
#else
static void *stall_sampler_main(void *arg) { (void)arg; stall_sampler_loop(); return NULL; }
#endif

void stall_sampler_start(void) {
    if (s_stall_started) return;
#ifdef _WIN32
    s_emu_tid = GetCurrentThreadId();
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                         &s_stall_emu_thread, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                         THREAD_QUERY_INFORMATION, FALSE, 0))
        s_stall_emu_thread = NULL;
    HANDLE t = CreateThread(NULL, 0, stall_sampler_main, NULL, 0, NULL);
    if (!t) return;
    CloseHandle(t);
#else
    s_emu_pthread = pthread_self();
    s_emu_pthread_set = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, stall_sampler_main, NULL) != 0) return;
    pthread_detach(t);
#endif
    s_stall_started = 1;
}

/* Emu thread, on resuming from a long host-side block: persist the episode so
 * a playtester's run keeps the evidence without a TCP client attached. */
static void stall_write_report(uint64_t block_us) {
    if (block_us < STALL_REPORT_MIN_US || s_stall_reports >= STALL_REPORT_LIMIT) return;
    char path[96];
    snprintf(path, sizeof(path), "psx_stall_report_%lld_%u.json",
             (long long)time(NULL), s_stall_reports++);
    FILE *f = fopen(path, "w");
    if (!f) return;
    uint64_t now = host_us_now();
    uint64_t from = now > block_us + STALL_SAMPLE_AFTER_US ? now - block_us - STALL_SAMPLE_AFTER_US : 0;
    fprintf(f, "{\"block_us\":%llu,\"resumed_us\":%llu,\"entries\":[\n",
            (unsigned long long)block_us, (unsigned long long)now);
    uint64_t total = stall_ring_total();
    uint64_t start = total > STALL_RING_CAP ? total - STALL_RING_CAP : 0;
    int first = 1;
    for (uint64_t seq = start; seq < total; seq++) {
        StallSample s;
        if (!stall_ring_get(seq, &s)) continue;
        if (s.host_us < from) continue;   /* only this block's window */
        if (!first) fputs(",\n", f);
        stall_fprint(f, &s);
        first = 0;
    }
    fputs("\n]}\n", f);
    fclose(f);
}

static uint64_t heartbeat_load(void) {
    return atomic_load_explicit(&s_last_heartbeat_us, memory_order_relaxed);
}

/* Watchdog threshold: 4 seconds of wall-clock without a heartbeat is
 * "TCP died, BIOS still running" territory. Real BIOS pad polling
 * fires VBlank ~60Hz so debug_server_poll runs at ~60Hz. 4 seconds
 * is well past any normal stall.
 *
 * Override with env PSX_STARVATION_TIMEOUT_US (microseconds; 0 disables the
 * watchdog) — needed while bringing up a renderer whose per-op submit model is
 * deliberately slow (a single heavy frame can legitimately exceed 4 s). */
#define STARVATION_TIMEOUT_US_DEFAULT (4 * 1000000ULL)
static uint64_t starvation_timeout_us(void) {
    static int resolved = 0;
    static uint64_t timeout = STARVATION_TIMEOUT_US_DEFAULT;
    if (!resolved) {
        const char *e = getenv("PSX_STARVATION_TIMEOUT_US");
        if (e && *e) timeout = strtoull(e, NULL, 10);
        resolved = 1;
    }
    return timeout;
}

/* Emu-thread activity (every watchdog check and every emu-thread heartbeat).
 * A gap between consecutive activities means the emu thread itself did not
 * run: a host-side block (OS/disk/driver/process pause, or long synchronous
 * host work) that has just ended. Menu/barrier loops stamp every few ms, so
 * parked-guest time never looks like a block. Returns the previous stamp. */
static uint64_t emu_activity(uint64_t now) {
    uint64_t prev = s_prev_check_us;
    s_prev_check_us = now;
    if (prev && now > prev && now - prev >= STALL_SAMPLE_AFTER_US) {
        stall_record(STALL_HOST_BLOCK, now - prev, NULL, 0);
        stall_write_report(now - prev);
    }
    return prev;
}

void starvation_watchdog_heartbeat(void) {
    uint64_t now = host_us_now();
    atomic_store_explicit(&s_last_heartbeat_us, now, memory_order_relaxed);
    if (stall_on_emu_thread()) {
        atomic_store_explicit(&s_emu_heartbeat_us, now, memory_order_relaxed);
        if (s_stall_started) (void)emu_activity(now);
    }
}

void starvation_ring_reset(void) {
    memset(s_ring, 0, sizeof(s_ring));
    s_seq = 0;
    atomic_store_explicit(&s_last_heartbeat_us, 0, memory_order_relaxed);
    atomic_store_explicit(&s_emu_heartbeat_us, 0, memory_order_relaxed);
    s_dump_done = 0;
    s_prev_check_us = 0;   /* the stall ring spans sessions on purpose */
}

void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat,
                            int shift_active, int shift_remaining,
                            int tx_buffered, int pending_ack,
                            int ack_remaining,
                            uint8_t bus_owner, uint32_t bus_byte_index,
                            uint8_t active_device, uint8_t mc_state,
                            uint8_t pad_state, uint8_t selected_slot,
                            int g_sio_timing_active) {
    StarvationEntry *e = &s_ring[s_seq & (STARVATION_RING_CAP - 1)];
    e->seq                  = s_seq++;
    e->psx_cycle_count      = psx_get_cycle_count();
    e->host_us              = host_us_now();
    e->current_func         = g_debug_current_func_addr;
    e->last_store_pc        = g_debug_last_store_pc;
    e->sio_ctrl             = ctrl;
    e->sio_stat             = stat;
    e->tx_data              = tx;
    e->rx_data              = rx;
    e->kind                 = kind;
    e->in_exception         = (uint8_t)psx_get_in_exception();
    e->shift_active         = (uint8_t)shift_active;
    e->tx_buffered          = (uint8_t)tx_buffered;
    e->pending_ack          = (uint8_t)pending_ack;
    e->bus_owner            = bus_owner;
    e->active_device        = active_device;
    e->mc_state             = mc_state;
    e->pad_state            = pad_state;
    e->selected_slot        = selected_slot;
    e->g_sio_timing_active  = (uint8_t)g_sio_timing_active;
    e->tx_rdy_visible       = (uint8_t)((stat >> 0) & 1);
    e->tx_em_visible        = (uint8_t)((stat >> 2) & 1);
    e->shift_remaining      = shift_remaining;
    e->ack_remaining        = ack_remaining;
    e->bus_byte_index       = bus_byte_index;
    e->i_stat               = i_stat;
    e->i_mask               = i_mask;
}

static const char *kind_name(uint8_t k) {
    switch (k) {
    case SR_EVT_TX_DATA_WRITE: return "TX_WRITE";
    case SR_EVT_RX_DATA_READ:  return "RX_READ";
    case SR_EVT_STAT_READ:     return "STAT_READ";
    case SR_EVT_CTRL_WRITE:    return "CTRL_WRITE";
    case SR_EVT_MODE_WRITE:    return "MODE_WRITE";
    case SR_EVT_BAUD_WRITE:    return "BAUD_WRITE";
    case SR_EVT_SHIFT_START:   return "SHIFT_START";
    case SR_EVT_BUFFER_LOAD:   return "BUFFER_LOAD";
    case SR_EVT_TX_DROPPED:    return "TX_DROPPED";
    case SR_EVT_SHIFT_DONE:    return "SHIFT_DONE";
    case SR_EVT_ACK_FIRE:      return "ACK_FIRE";
    case SR_EVT_SELECT_ASSERT: return "SELECT_ASSERT";
    case SR_EVT_SELECT_DEASS:  return "SELECT_DEASSERT";
    case SR_EVT_RESET:         return "RESET";
    case SR_EVT_PC_SAMPLE:     return "PC_SAMPLE";
    default: return "?";
    }
}

/* PC sampler. Records current PC + in_exc + i_stat/i_mask without
 * touching SIO state — used to localize busy-wait loops that don't
 * touch MMIO. Caller responsible for throttling (~every 1M cycles). */
void starvation_ring_pc_sample(void) {
    StarvationEntry *e = &s_ring[s_seq & (STARVATION_RING_CAP - 1)];
    e->seq                  = s_seq++;
    e->psx_cycle_count      = psx_get_cycle_count();
    e->host_us              = host_us_now();
    e->current_func         = g_debug_current_func_addr;
    e->last_store_pc        = g_debug_last_store_pc;
    e->sio_ctrl             = 0;
    e->sio_stat             = 0;
    e->tx_data              = 0;
    e->rx_data              = 0;
    e->kind                 = SR_EVT_PC_SAMPLE;
    e->in_exception         = (uint8_t)psx_get_in_exception();
    e->shift_active         = 0;
    e->tx_buffered          = 0;
    e->pending_ack          = 0;
    e->bus_owner            = 0;
    e->active_device        = 0;
    e->mc_state             = 0;
    e->pad_state            = 0;
    e->selected_slot        = 0;
    e->g_sio_timing_active  = 0;
    e->tx_rdy_visible       = 0;
    e->tx_em_visible        = 0;
    e->shift_remaining      = 0;
    e->ack_remaining        = 0;
    e->bus_byte_index       = 0;
    e->i_stat               = i_stat;
    e->i_mask               = i_mask;
}

uint64_t starvation_ring_total(void) { return s_seq; }

int starvation_ring_get(uint64_t seq, StarvationEntry *out) {
    if (seq >= s_seq) return 0;                                /* not written yet */
    if (s_seq - seq > STARVATION_RING_CAP) return 0;           /* evicted */
    *out = s_ring[seq & (STARVATION_RING_CAP - 1)];
    if (out->seq != seq) return 0;                             /* raced an eviction */
    return 1;
}

void starvation_ring_dump(const char *path) {
    if (s_dump_done) return;  /* dump exactly once */
    s_dump_done = 1;
    const char *file = path ? path : "starvation_dump.jsonl";
    FILE *f = fopen(file, "w");
    if (!f) return;
    uint64_t total = s_seq;
    uint64_t avail = total < STARVATION_RING_CAP ? total : STARVATION_RING_CAP;
    {
        char net_arch[24];
        int max_players = 0, player_count = 0;
        (void)psx_netplay_diag_snapshot(net_arch, sizeof(net_arch),
                                        &max_players, &player_count);
        fprintf(f, "{\"meta\":{\"total\":%llu,\"shown\":%llu,"
                "\"last_heartbeat_us\":%llu,\"now_us\":%llu,"
                "\"psx_cycle_count\":%llu,"
                "\"current_func\":\"0x%08X\",\"last_store_pc\":\"0x%08X\","
                "\"in_exception\":%u,\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\","
                "\"net_arch\":\"%s\",\"max_players\":%d,\"player_count\":%d}}\n",
                (unsigned long long)total, (unsigned long long)avail,
                (unsigned long long)heartbeat_load(),
                (unsigned long long)host_us_now(),
                (unsigned long long)psx_get_cycle_count(),
                g_debug_current_func_addr, g_debug_last_store_pc,
                (unsigned)psx_get_in_exception(),
                i_stat, i_mask,
                net_arch, max_players, player_count);
    }
    uint64_t start = total > avail ? total - avail : 0;
    for (uint64_t i = start; i < total; i++) {
        StarvationEntry *e = &s_ring[i & (STARVATION_RING_CAP - 1)];
        fprintf(f,
                "{\"seq\":%llu,\"kind\":\"%s\",\"cyc\":%llu,\"us\":%llu,"
                "\"func\":\"0x%08X\",\"pc\":\"0x%08X\","
                "\"ctrl\":\"0x%04X\",\"stat\":\"0x%04X\","
                "\"tx\":\"0x%02X\",\"rx\":\"0x%02X\","
                "\"in_exc\":%u,\"shift_act\":%u,\"shift_rem\":%d,"
                "\"buf\":%u,\"ack_pend\":%u,\"ack_rem\":%d,"
                "\"owner\":%u,\"bbidx\":%u,"
                "\"dev\":%u,\"mc\":%u,\"pad\":%u,\"slot\":%u,"
                "\"gact\":%u,\"tx_rdy\":%u,\"tx_em\":%u,"
                "\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\"}\n",
                (unsigned long long)e->seq, kind_name(e->kind),
                (unsigned long long)e->psx_cycle_count,
                (unsigned long long)e->host_us,
                e->current_func, e->last_store_pc,
                e->sio_ctrl, e->sio_stat,
                e->tx_data, e->rx_data,
                e->in_exception, e->shift_active, e->shift_remaining,
                e->tx_buffered, e->pending_ack, e->ack_remaining,
                e->bus_owner, e->bus_byte_index,
                e->active_device, e->mc_state, e->pad_state, e->selected_slot,
                e->g_sio_timing_active, e->tx_rdy_visible, e->tx_em_visible,
                e->i_stat, e->i_mask);
    }
    /* Stall ring: sampled emu-thread stacks, sampler gaps and host blocks. */
    {
        uint64_t stall_total = stall_ring_total();
        uint64_t stall_start = stall_total > STALL_RING_CAP ? stall_total - STALL_RING_CAP : 0;
        for (uint64_t seq = stall_start; seq < stall_total; seq++) {
            StallSample s;
            if (!stall_ring_get(seq, &s)) continue;
            stall_fprint(f, &s);
            fputc('\n', f);
        }
    }
    fclose(f);
}

void starvation_watchdog_check(void) {
    if (s_dump_done) return;
    /* Load the heartbeat BEFORE sampling the clock. The IO thread stamps it
     * concurrently; with the reads in the other order a stamp landing in
     * between made now < last and the unsigned gap wrapped past any
     * threshold (see starvation_watchdog_stale). */
    uint64_t last = heartbeat_load();
    uint64_t now = host_us_now();
    uint64_t prev = emu_activity(now);
    if (last == 0) return;                 /* not initialized yet */
    uint64_t timeout = starvation_timeout_us();
    if (timeout == 0) return;              /* watchdog disabled (renderer bring-up) */
    if (starvation_watchdog_stale(last, now, timeout)) {
        if (starvation_watchdog_host_block(prev, now, timeout)) {
            /* Already over: the emu thread is running again (emu_activity
             * recorded the block). Aborting here would only kill a recovered
             * game (2026-10-09 FF7 world-map->battle). */
            starvation_watchdog_heartbeat();
            return;
        }
        stall_record(STALL_GUEST_ABORT, now - last, NULL, 0);
        starvation_ring_dump(NULL);
        /* Abort cleanly so the dump file is preserved and the user knows
         * the runtime starved. Tag the exit so psx_last_run_report.json
         * says "starvation_watchdog" instead of atexit/unknown. */
        fprintf(stderr, "starvation_watchdog: %llu us without heartbeat — "
                "ring dumped to starvation_dump.jsonl, aborting\n",
                (unsigned long long)(now - last));
        fflush(stderr);
        psx_crash_trace_set_exit_origin("starvation_watchdog");
        exit(2);
    }
}

#else

void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat,
                            int shift_active, int shift_remaining,
                            int tx_buffered, int pending_ack,
                            int ack_remaining,
                            uint8_t bus_owner, uint32_t bus_byte_index,
                            uint8_t active_device, uint8_t mc_state,
                            uint8_t pad_state, uint8_t selected_slot,
                            int g_sio_timing_active) {
    (void)kind; (void)tx; (void)rx; (void)ctrl; (void)stat;
    (void)shift_active; (void)shift_remaining;
    (void)tx_buffered; (void)pending_ack; (void)ack_remaining;
    (void)bus_owner; (void)bus_byte_index;
    (void)active_device; (void)mc_state; (void)pad_state;
    (void)selected_slot; (void)g_sio_timing_active;
}
void starvation_watchdog_heartbeat(void) {}
void starvation_watchdog_check(void) {}
void starvation_ring_dump(const char *path) { (void)path; }
void starvation_ring_reset(void) {}
void starvation_ring_pc_sample(void) {}
uint64_t starvation_ring_total(void) { return 0; }
int starvation_ring_get(uint64_t seq, StarvationEntry *out) { (void)seq; (void)out; return 0; }
void stall_sampler_start(void) {}
uint64_t stall_ring_total(void) { return 0; }
int stall_ring_get(uint64_t seq, StallSample *out) { (void)seq; (void)out; return 0; }
void stall_format_frame(uint64_t addr, char *out, size_t cap) {
    if (out && cap) snprintf(out, cap, "0x%016llX", (unsigned long long)addr);
}

#endif /* STARVATION_RING_ENABLED */
