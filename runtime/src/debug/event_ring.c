/*
 * event_ring.c — always-on unified event-timeline ring. See event_ring.h.
 *
 * OBSERVABILITY ONLY. Records edges/events from the IRQ-decision path, I_STAT
 * raise sites, and DMA kick/complete, each tagged with cycle/pc/func/mode/
 * overlay-fn so two runs of the same route can be diffed for the first
 * interrupt/DMA ordering divergence.
 */
#include "event_ring.h"
#include "psx_cycles.h"

#include <stdio.h>
#include <string.h>

/* ---- runtime globals captured at record time ---- */
extern uint32_t i_stat;                     /* memory.c     */
extern uint32_t i_mask;                     /* memory.c     */
extern uint32_t g_debug_current_func_addr;  /* debug_server */
extern uint32_t g_debug_last_store_pc;      /* debug_server */

/* Execution-mode sources (see overlay_loader.c / dirty_ram_interp.c). */
extern uint32_t overlay_loader_get_inprogress(void); /* in-progress native ovl fn, 0 if none */
extern int      dirty_ram_interp_is_active(void);    /* 1 while interpreting a dirty block    */

static EventEntry s_ring[EVENT_RING_CAP];
static uint64_t   s_seq;   /* total events ever recorded (monotonic) */

static uint8_t current_mode(void) {
    if (overlay_loader_get_inprogress() != 0) return MODE_NATIVE_OVERLAY;
    if (dirty_ram_interp_is_active())         return MODE_INTERP;
    return MODE_STATIC;
}

void event_ring_record_aux(uint16_t kind, uint8_t detail, uint32_t aux) {
#if EVENT_RING_ENABLED
    EventEntry *e = &s_ring[s_seq & (EVENT_RING_CAP - 1u)];
    e->seq        = s_seq++;
    e->cycle      = psx_get_cycle_count();
    e->pc         = g_debug_last_store_pc;
    e->func       = g_debug_current_func_addr;
    e->overlay_fn = overlay_loader_get_inprogress();
    e->i_stat     = i_stat;
    e->i_mask     = i_mask;
    e->aux        = aux;
    e->kind       = kind;
    e->mode       = current_mode();
    e->detail     = detail;
#else
    (void)kind; (void)detail; (void)aux;
#endif
}

void event_ring_record(uint16_t kind, uint8_t detail) {
    event_ring_record_aux(kind, detail, 0u);
}

void event_ring_clear(void) {
    s_seq = 0;
    memset(s_ring, 0, sizeof(s_ring));
}

static const char *kind_name(uint16_t k) {
    switch (k) {
        case EV_IRQ_DELIVER:  return "IRQ_DELIVER";
        case EV_IRQ_GATE:     return "IRQ_GATE";
        case EV_ISTAT_RAISE:  return "ISTAT_RAISE";
        case EV_ISTAT_CHANGE: return "ISTAT_CHANGE";
        case EV_DMA_KICK:     return "DMA_KICK";
        case EV_DMA_DONE:     return "DMA_DONE";
        case EV_DMA_SCHED:    return "DMA_SCHED";
        case EV_ENQ:          return "ENQ";
        case EV_DEQ:          return "DEQ";
        case EV_MODE:         return "MODE";
        default:              return "NONE";
    }
}
static const char *mode_name(uint8_t m) {
    switch (m) {
        case MODE_STATIC:         return "STATIC";
        case MODE_NATIVE_OVERLAY: return "NATIVE_OVERLAY";
        case MODE_INTERP:         return "INTERP";
        default:                  return "UNKNOWN";
    }
}

static int format_entry(char *out, int cap, const EventEntry *e) {
    return snprintf(out, cap,
        "{\"seq\":%llu,\"cycle\":%llu,\"kind\":\"%s\",\"mode\":\"%s\","
        "\"pc\":\"0x%08X\",\"func\":\"0x%08X\",\"ovl\":\"0x%08X\","
        "\"istat\":\"0x%08X\",\"imask\":\"0x%08X\",\"detail\":%u,\"aux\":\"0x%08X\"}",
        (unsigned long long)e->seq, (unsigned long long)e->cycle,
        kind_name(e->kind), mode_name(e->mode),
        e->pc, e->func, e->overlay_fn, e->i_stat, e->i_mask,
        (unsigned)e->detail, e->aux);
}

int event_ring_dump_file(const char *path) {
    FILE *f = fopen(path ? path : "event_ring.json", "w");
    if (!f) return -1;
    uint64_t total = s_seq;
    uint64_t start = (total > EVENT_RING_CAP) ? (total - EVENT_RING_CAP) : 0;
    fputc('[', f);
    char buf[512];
    int first = 1, count = 0;
    for (uint64_t s = start; s < total; s++) {
        const EventEntry *e = &s_ring[s & (EVENT_RING_CAP - 1u)];
        format_entry(buf, (int)sizeof(buf), e);
        fprintf(f, "%s%s", first ? "" : ",\n", buf);
        first = 0; count++;
    }
    fputs("]\n", f);
    fclose(f);
    return count;
}

int event_ring_dump_json(char *out, int cap, int max_entries) {
    uint64_t total = s_seq;
    uint64_t want  = (max_entries > 0) ? (uint64_t)max_entries : 256;
    uint64_t start = (total > want) ? (total - want) : 0;
    int n = 0;
    n += snprintf(out + n, cap - n,
                  "{\"total\":%llu,\"events\":[", (unsigned long long)total);
    int first = 1;
    char buf[512];
    for (uint64_t s = start; s < total && n < cap - 600; s++) {
        const EventEntry *e = &s_ring[s & (EVENT_RING_CAP - 1u)];
        format_entry(buf, (int)sizeof(buf), e);
        n += snprintf(out + n, cap - n, "%s%s", first ? "" : ",", buf);
        first = 0;
    }
    n += snprintf(out + n, cap - n, "]}");
    return n;
}
