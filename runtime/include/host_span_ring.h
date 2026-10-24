#ifndef PSX_HOST_SPAN_RING_H
#define PSX_HOST_SPAN_RING_H
/* Always-on host wall-time rings: where an emulation-thread stall comes from.
 *
 * Two rings, both written continuously from process start in EVERY build
 * (Release too); probes only ever QUERY a window that already happened:
 *
 *  - frame stamps: one {guest frame, wall us} per guest frame (131072 frames,
 *    ~36 min at 60 Hz). Successive deltas are the per-frame wall time, so a
 *    hitch is visible headless as well as windowed (no GL present ring needed).
 *  - spans: every named blocking-capable runtime operation (loader, capture,
 *    publication commit, cache rescan, autocompile spawn/prepare/compile child)
 *    as {kind, start wall us, duration us, guest frame, thread class, arg}.
 *    The emulation thread's spans are the candidates for a hitch; background
 *    spans show the work that fed them.
 *
 * Wall time is microseconds since the first call in the process (monotonic).
 * Writers may be different threads. Per-slot ownership and publication sequence
 * validation prevent torn or overwritten rows. Contended samples are dropped;
 * diagnostics never wait for a reader to release a slot.
 *
 * Reading: TCP "host_span_ring" / "frame_wall_ring"; a title's hitch report
 * tool joins them (LegendOfDragoonRecomp tools/lod_hitch_report.py). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum HostSpanKind {
    HSPAN_NONE = 0,
    HSPAN_AUTOCAPTURE_TICK,   /* emu: overlay_autocapture_tick (whole)            */
    HSPAN_PROVIDER_POLL,      /* emu: code provider poll_main (whole)             */
    HSPAN_PUBLISH_COMMIT,     /* emu: overlay_loader_commit_published             */
    HSPAN_LOADER_RESCAN,      /* emu: overlay_loader_rescan                       */
    HSPAN_DLL_REGISTER,       /* emu: parse manifest + load_overlay_dll           */
    HSPAN_LAZY_LOAD,          /* emu: try_load_region (miss -> cache load)        */
    HSPAN_CAPTURE_ON_DMA,     /* emu: overlay_capture_on_dma (CD/DMA arrival)     */
    HSPAN_CAPTURE_BEFORE_DMA, /* emu: overlay_capture_before_dma (evidence save)  */
    HSPAN_AC_REQUEST,         /* emu: autocompile_request (spawn compiler)        */
    HSPAN_AC_DONE_JOIN,       /* emu: poll_main joining finished workers          */
    HSPAN_AC_PREPARE_IMAGE,   /* bg : publication preparer LoadLibrary            */
    HSPAN_AC_CHILD,           /* bg : compiler child process lifetime             */
    HSPAN_OVERLAY_DISPATCH,   /* emu: overlay_loader_dispatch slow path           */
    HSPAN_MOD_WORK,           /* emu: resident pack / mod host work               */
    HSPAN_FRAME_PRESENT,      /* emu: frontend present for the frame              */
    HSPAN_DISC_READ,          /* emu: one sector read from the disc image (arg lba) */
    HSPAN_HOST_EVENTS,        /* emu: pump and dispatch host window events */
    HSPAN_REWIND_CAPTURE,     /* emu: snapshot, store and thumbnail */
    HSPAN_DISPLAY_PROBE,      /* emu: host refresh/mode query */
    HSPAN_AUDIO_UPDATE,       /* emu: host audio pump */
    HSPAN_KIND_COUNT
} HostSpanKind;

typedef struct HostSpan {
    uint64_t t0_us;    /* start, wall us since process clock origin  */
    uint32_t dur_us;   /* duration (saturates at ~71 min)            */
    uint32_t frame;    /* guest frame when the span ended            */
    uint32_t arg;      /* kind-specific (address, ok flag, count)    */
    uint16_t kind;     /* HostSpanKind                               */
    uint16_t emu;      /* 1 = emulation thread, 0 = background       */
} HostSpan;

/* Wall microseconds since the process clock origin. */
uint64_t host_clock_us(void);

/* Open / close a span. begin returns the start stamp; end records it. */
uint64_t host_span_begin(void);
void host_span_end(HostSpanKind kind, uint64_t t0_us, uint32_t arg);
/* Record a span whose duration the caller already measured. */
void host_span_record(HostSpanKind kind, uint64_t t0_us, uint64_t dur_us, uint32_t arg);

uint64_t host_span_total(void);
int host_span_get(uint64_t seq, HostSpan *out);
const char *host_span_kind_name(unsigned kind);
/* 65536 spans; spans shorter than HOST_SPAN_MIN_US are not recorded (the per-frame
 * tick wrappers would otherwise flood the ring; a hitch is milliseconds). */
#define HOST_SPAN_RING_CAP 65536u
#define HOST_SPAN_MIN_US 50u

/* Stamp guest frame `frame` at the current wall time. Also marks the calling
 * thread as the emulation thread for span attribution. Call once per frame.
 * interp_insns is the low 32 bits of the cumulative count of guest instructions run by the
 * interpreter (successive differences = interpreter work in that frame); static_checks likewise
 * counts static-AOT dispatch checks (guest calls into statically compiled code), the work a native
 * stretch of guest code between two vblanks does. Each stamp also records the process's cumulative
 * page-fault count and disk-read volume (KiB), so a frame that waited on the OS (code or data pages
 * paged in from disk) is told apart from one that computed. */
void host_frame_stamp(uint32_t frame, uint32_t interp_insns, uint32_t static_checks);
uint64_t host_frame_stamp_total(void);
int host_frame_stamp_get(uint64_t seq, uint32_t *frame, uint64_t *t_us, uint32_t *interp_insns,
                         uint32_t *static_checks, uint32_t *page_faults, uint32_t *io_read_kb);
#define HOST_FRAME_STAMP_CAP 131072u

#ifdef __cplusplus
}
#endif
#endif
