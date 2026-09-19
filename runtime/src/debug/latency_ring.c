/* latency_ring.c — always-on input->photon latency ring.  See latency_ring.h. */

#include "latency_ring.h"
#include "psx_sdl.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define LAT_RING_SIZE 4096   /* power of two; ~68s at 60Hz */

typedef struct {
    uint64_t ts[LAT_STAGE_COUNT];
    uint64_t frame;
} LatSlot;

static LatSlot   s_ring[LAT_RING_SIZE];
static uint64_t  s_head;            /* number of frames begun (head = s_head-1) */
static char      s_backend[16] = "?";
static int       s_present_mode = 1;

static uint64_t qpc(void) { return SDL_GetPerformanceCounter(); }
static double   qpc_freq(void) {
    double f = (double)SDL_GetPerformanceFrequency();
    return f > 0.0 ? f : 1.0;
}

void latency_ring_frame_begin(void) {
    uint64_t idx = s_head % LAT_RING_SIZE;
    LatSlot *s = &s_ring[idx];
    memset(s, 0, sizeof(*s));
    s->frame = s_head;
    s->ts[LAT_INPUT] = qpc();
    s_head++;
}

void latency_ring_mark(LatencyStage stage) {
    if (stage <= LAT_INPUT || stage >= LAT_STAGE_COUNT) return;
    if (s_head == 0) return;                     /* no frame begun yet */
    LatSlot *s = &s_ring[(s_head - 1) % LAT_RING_SIZE];
    s->ts[stage] = qpc();
}

void latency_ring_restamp_input(void) {
    if (s_head == 0) return;
    s_ring[(s_head - 1) % LAT_RING_SIZE].ts[LAT_INPUT] = qpc();
}

void latency_ring_set_backend(const char *name) {
    if (!name) return;
    strncpy(s_backend, name, sizeof(s_backend) - 1);
    s_backend[sizeof(s_backend) - 1] = '\0';
}

void latency_ring_set_present_mode(int mode) { s_present_mode = mode; }

/* Collect a delta series (in microseconds) over the last `window` completed
 * frames into `out`, returning the count. `a`/`b` are the two stages whose
 * difference forms each sample; `prev_a` true means use stage `a` of the
 * PREVIOUS frame (for frame_period = INPUT[n] - INPUT[n-1]). Only frames whose
 * required stamps are all present contribute. */
static int collect(double *out, int cap, int window,
                   LatencyStage a, LatencyStage b, int prev_a) {
    double inv_us = 1.0e6 / qpc_freq();
    int n = 0;
    if (s_head == 0) return 0;
    uint64_t total = s_head;
    uint64_t take = window > 0 && (uint64_t)window < total ? (uint64_t)window : total;
    uint64_t start = total - take;
    for (uint64_t f = start; f < total && n < cap; f++) {
        const LatSlot *cur = &s_ring[f % LAT_RING_SIZE];
        if (cur->frame != f) continue;            /* evicted/torn */
        uint64_t t_b = cur->ts[b];
        uint64_t t_a;
        if (prev_a) {
            if (f == 0) continue;
            const LatSlot *pv = &s_ring[(f - 1) % LAT_RING_SIZE];
            if (pv->frame != f - 1) continue;
            t_a = pv->ts[a];
        } else {
            t_a = cur->ts[a];
        }
        if (t_a == 0 || t_b == 0 || t_b < t_a) continue;
        out[n++] = (double)(t_b - t_a) * inv_us;
    }
    return n;
}

static int dcmp(const void *p, const void *q) {
    double a = *(const double *)p, b = *(const double *)q;
    return (a > b) - (a < b);
}

/* Append "name":{mean,min,max,p50,p95,n} stats for a delta series to buf. */
static int stat_obj(char *buf, int cap, const char *name,
                    double *vals, int n) {
    double mn = 0, mx = 0, sum = 0, p50 = 0, p95 = 0, mean = 0;
    if (n > 0) {
        qsort(vals, n, sizeof(double), dcmp);
        mn = vals[0]; mx = vals[n - 1];
        for (int i = 0; i < n; i++) sum += vals[i];
        mean = sum / n;
        p50 = vals[(int)(n * 0.50)];
        p95 = vals[n > 1 ? (int)(n * 0.95) : 0];
    }
    return snprintf(buf, cap,
        "\"%s\":{\"mean_us\":%.1f,\"min_us\":%.1f,\"max_us\":%.1f,"
        "\"p50_us\":%.1f,\"p95_us\":%.1f,\"n\":%d}",
        name, mean, mn, mx, p50, p95, n);
}

int latency_ring_summary_json(char *buf, int buf_size, int window) {
    static double tmp[LAT_RING_SIZE];
    if (window <= 0) window = 240;
    int off = 0;
    off += snprintf(buf + off, buf_size - off,
                    "{\"backend\":\"%s\",\"present_mode\":%d,\"frames\":%llu,",
                    s_backend, s_present_mode, (unsigned long long)s_head);

    int n;
    n = collect(tmp, LAT_RING_SIZE, window, LAT_INPUT, LAT_INPUT, 1);
    off += stat_obj(buf + off, buf_size - off, "frame_period", tmp, n);
    off += snprintf(buf + off, buf_size - off, ",");

    n = collect(tmp, LAT_RING_SIZE, window, LAT_INPUT, LAT_SWAP_BEGIN, 0);
    off += stat_obj(buf + off, buf_size - off, "input_to_swap", tmp, n);
    off += snprintf(buf + off, buf_size - off, ",");

    n = collect(tmp, LAT_RING_SIZE, window, LAT_SWAP_BEGIN, LAT_SWAP_END, 0);
    off += stat_obj(buf + off, buf_size - off, "swap_block", tmp, n);
    off += snprintf(buf + off, buf_size - off, ",");

    n = collect(tmp, LAT_RING_SIZE, window, LAT_INPUT, LAT_PACED, 0);
    off += stat_obj(buf + off, buf_size - off, "input_to_paced", tmp, n);

    off += snprintf(buf + off, buf_size - off, "}");
    return off;
}

int latency_ring_dump_json(char *buf, int buf_size, int max_frames) {
    double inv_us = 1.0e6 / qpc_freq();
    if (max_frames <= 0) max_frames = 120;
    int off = 0;
    off += snprintf(buf + off, buf_size - off, "[");
    if (s_head == 0) { off += snprintf(buf + off, buf_size - off, "]"); return off; }
    uint64_t total = s_head;
    uint64_t take = (uint64_t)max_frames < total ? (uint64_t)max_frames : total;
    uint64_t start = total - take;
    uint64_t base = 0;
    int first = 1;
    for (uint64_t f = start; f < total; f++) {
        const LatSlot *s = &s_ring[f % LAT_RING_SIZE];
        if (s->frame != f) continue;
        if (first) { base = s->ts[LAT_INPUT]; first = 0; }
        double ti  = (double)(s->ts[LAT_INPUT]      - base) * inv_us;
        double tp  = s->ts[LAT_PACED]      ? (double)(s->ts[LAT_PACED]      - base) * inv_us : -1;
        double tsb = s->ts[LAT_SWAP_BEGIN] ? (double)(s->ts[LAT_SWAP_BEGIN] - base) * inv_us : -1;
        double tse = s->ts[LAT_SWAP_END]   ? (double)(s->ts[LAT_SWAP_END]   - base) * inv_us : -1;
        off += snprintf(buf + off, buf_size - off,
            "%s{\"f\":%llu,\"input\":%.1f,\"paced\":%.1f,\"swap_begin\":%.1f,\"swap_end\":%.1f}",
            (f == start ? "" : ","), (unsigned long long)s->frame, ti, tp, tsb, tse);
        if (buf_size - off < 128) break;
    }
    off += snprintf(buf + off, buf_size - off, "]");
    return off;
}
