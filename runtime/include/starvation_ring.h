/*
 * starvation_ring.h — Always-on diagnostic ring for capturing SIO/MMIO
 * state immediately before a TCP-starvation hang.
 *
 * Phase 1.0e-e2 debug aid. Scope: diagnostic-only. Records every SIO MMIO
 * event (TX_DATA write, RX read, STAT read, CTRL write) plus a snapshot
 * of all SIO bus state at that moment. Watchdog monitors host wall-clock
 * since last debug_server_poll; if it exceeds a threshold, the ring is
 * flushed to disk and the process aborts cleanly so the dump survives.
 *
 * NOT for permanent use. Disabled by setting STARVATION_RING_ENABLED=0.
 */
#ifndef PSXRECOMP_STARVATION_RING_H
#define PSXRECOMP_STARVATION_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef STARVATION_RING_ENABLED
#  ifdef PSX_NO_DEBUG_TOOLS
#    define STARVATION_RING_ENABLED 0
#  else
#    define STARVATION_RING_ENABLED 1
#  endif
#endif

#define STARVATION_RING_CAP (1 << 14)  /* 16K entries */

typedef enum {
    SR_EVT_NONE          = 0,
    SR_EVT_TX_DATA_WRITE = 1,
    SR_EVT_RX_DATA_READ  = 2,
    SR_EVT_STAT_READ     = 3,
    SR_EVT_CTRL_WRITE    = 4,
    SR_EVT_MODE_WRITE    = 5,
    SR_EVT_BAUD_WRITE    = 6,
    SR_EVT_SHIFT_START   = 7,
    SR_EVT_BUFFER_LOAD   = 8,
    SR_EVT_TX_DROPPED    = 9,
    SR_EVT_SHIFT_DONE    = 10,
    SR_EVT_ACK_FIRE      = 11,
    SR_EVT_SELECT_ASSERT = 12,
    SR_EVT_SELECT_DEASS  = 13,
    SR_EVT_RESET         = 14,
    SR_EVT_PC_SAMPLE     = 15,  /* periodic PC sample, no MMIO event */
} StarvationEventKind;

typedef struct {
    uint64_t seq;                /* monotonic sequence */
    uint64_t psx_cycle_count;    /* guest cycle clock at event */
    uint64_t host_us;            /* host monotonic time in microseconds */
    uint32_t current_func;       /* g_debug_current_func_addr */
    uint32_t last_store_pc;      /* g_debug_last_store_pc */
    uint16_t sio_ctrl;
    uint16_t sio_stat;
    uint8_t  tx_data;            /* on TX_DATA writes; else 0 */
    uint8_t  rx_data;            /* on RX_DATA reads; else 0 */
    uint8_t  kind;               /* StarvationEventKind */
    uint8_t  in_exception;
    uint8_t  shift_active;
    uint8_t  tx_buffered;
    uint8_t  pending_ack;
    uint8_t  bus_owner;          /* SioBusOwner */
    uint8_t  active_device;      /* DEV_NONE/PAD/MEMCARD */
    uint8_t  mc_state;
    uint8_t  pad_state;
    uint8_t  selected_slot;
    uint8_t  g_sio_timing_active;
    uint8_t  tx_rdy_visible;     /* (sio_stat & 1) at event time */
    uint8_t  tx_em_visible;      /* (sio_stat & 4) at event time */
    uint8_t  pad8;
    uint16_t pad16;
    int      shift_remaining;    /* cycles left on shifter */
    int      ack_remaining;      /* cycles left on ack */
    uint32_t bus_byte_index;
    uint32_t i_stat;
    uint32_t i_mask;
} StarvationEntry;

void starvation_ring_record(uint8_t kind, uint8_t tx, uint8_t rx,
                            uint16_t ctrl, uint16_t stat,
                            int shift_active, int shift_remaining,
                            int tx_buffered, int pending_ack,
                            int ack_remaining,
                            uint8_t bus_owner, uint32_t bus_byte_index,
                            uint8_t active_device, uint8_t mc_state,
                            uint8_t pad_state, uint8_t selected_slot,
                            int g_sio_timing_active);

/* Watchdog: call from debug_server_poll() to refresh the heartbeat.
 * If too much wall-time passes without a refresh AND the BIOS is
 * actively running (post-boot), the ring is dumped and the process
 * aborts. */
void starvation_watchdog_heartbeat(void);

/* Run watchdog check from a hot path (e.g. psx_advance_cycles). */
void starvation_watchdog_check(void);

/* Pure staleness decision behind starvation_watchdog_check(), exposed so a
 * unit test can pin it without linking the ring.
 *
 * The heartbeat is stamped from two threads (emu thread per vblank, and the
 * debug-server IO thread on every send() chunk), while the check runs on the
 * emu thread. A stamp landing between the check's clock sample and its
 * heartbeat load makes `last > now`; the old open-coded `now - last` wrapped
 * to ~1.8e19, beat any threshold, and exited a healthy runtime (BoF3
 * 2026-09-02: 402 us of real staleness tripped a 4 s watchdog). Callers must
 * therefore load `last` BEFORE sampling `now`, and this function refuses a
 * negative gap by construction. Returns 1 when the watchdog should fire.
 * `timeout == 0` disables it; `last == 0` means "no heartbeat yet". */
static inline int starvation_watchdog_stale(uint64_t last, uint64_t now,
                                            uint64_t timeout) {
    if (last == 0 || timeout == 0) return 0;
    if (now <= last) return 0;
    return (now - last) > timeout;
}

/* Host-block decision behind starvation_watchdog_check(). The check runs on
 * the emu thread every ~64K guest cycles, so consecutive checks are normally
 * milliseconds apart. When the heartbeat is stale AND the emu thread itself
 * did not reach a check for at least half the timeout, the wall time went to
 * a host-side block (OS/disk/driver/process pause) that is already over: the
 * thread is running again. That is recorded in the stall ring and the game
 * continues. Guest-side starvation (checks keep running, heartbeat does not)
 * still fires. `prev_check == 0` means no previous check. */
static inline int starvation_watchdog_host_block(uint64_t prev_check, uint64_t now,
                                                 uint64_t timeout) {
    if (prev_check == 0 || timeout == 0 || now <= prev_check) return 0;
    return (now - prev_check) >= timeout / 2u;
}

/* ---- Stall sampler (always-on, in memory) --------------------------------
 * A host thread ticks every STALL_TICK_US. While the emu-thread heartbeat is
 * older than STALL_SAMPLE_AFTER_US it records the emu thread's native stack
 * (Windows; POSIX records the gap without frames) every STALL_SAMPLE_EVERY_US.
 * It also records its OWN scheduling gaps: when the sampler did not run
 * either, the whole process (or the machine) was paused, not the emu thread.
 * The emu thread records HOST_BLOCK when it resumes from such a block. Query
 * with TCP `stall_ring`; every starvation dump appends the ring. No disk I/O
 * happens on the sampler thread, so a disk stall cannot hide the evidence. */
#define STALL_RING_CAP        256u
#define STALL_MAX_FRAMES      24u
#define STALL_TICK_US         50000ull
#define STALL_SAMPLE_AFTER_US 500000ull
#define STALL_SAMPLE_EVERY_US 250000ull
#define STALL_SAMPLER_GAP_US  250000ull

typedef enum {
    STALL_EMU_SAMPLE = 1,   /* emu thread stale; its stack at this moment */
    STALL_SAMPLER_GAP = 2,  /* the sampler itself was not scheduled */
    STALL_HOST_BLOCK = 3,   /* emu thread resumed after a host-side block */
    STALL_GUEST_ABORT = 4,  /* guest-side starvation: watchdog aborted */
} StallKind;

typedef struct {
    uint64_t seq;
    uint64_t host_us;
    uint64_t gap_us;          /* heartbeat age (EMU/ABORT), own gap (SAMPLER), block length (HOST) */
    uint64_t psx_cycle_count;
    uint64_t frame_count;
    uint32_t episode;         /* increments for each new emu-stale episode */
    uint32_t current_func;
    uint32_t kind;            /* StallKind */
    uint32_t nframes;
    uint64_t frames[STALL_MAX_FRAMES];  /* absolute host return addresses */
} StallSample;

/* Start the sampler; call once ON the emulation thread (it is the thread
 * sampled). Idempotent. */
void     stall_sampler_start(void);
uint64_t stall_ring_total(void);
int      stall_ring_get(uint64_t seq, StallSample *out);
/* "module+0xRVA" for a host address (Windows), else "0x...". */
void     stall_format_frame(uint64_t addr, char *out, size_t cap);

/* Manual dump-to-file (for testing / TCP probe). Filename can be NULL. */
void starvation_ring_dump(const char *path);

/* Rematch / session_reboot: clear ring + watchdog so a prior match cannot
 * poison the next dump or leave s_dump_done set. */
void starvation_ring_reset(void);

/* Periodic PC sample. Caller responsible for throttling. */
void starvation_ring_pc_sample(void);

/* Ring query (TCP `starv_ring`): total events recorded so far, and a copy
 * of the entry with sequence number `seq` (0 = first ever). Returns 1 and
 * fills *out when `seq` is still resident in the ring; 0 when evicted or
 * not yet written. Same-thread with the writers (emu/main thread), so the
 * copy is tear-free. */
uint64_t starvation_ring_total(void);
int      starvation_ring_get(uint64_t seq, StarvationEntry *out);

#ifdef __cplusplus
}
#endif

#endif /* PSXRECOMP_STARVATION_RING_H */
