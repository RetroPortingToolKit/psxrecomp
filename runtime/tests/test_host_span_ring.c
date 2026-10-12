/* test_host_span_ring.c - the always-on wall-time rings (host_span_ring.h):
 * span filter, emulation-thread attribution, ring wrap, frame stamps. */
#include "host_span_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

static _Atomic unsigned writers_done;
static void race_write(void) {
    for (unsigned i = 1; i <= HOST_SPAN_RING_CAP * 3u; ++i)
        host_span_record(HSPAN_DISC_READ, (uint64_t)i * 1000u, 50u + i, i);
    atomic_fetch_add(&writers_done, 1u);
}

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(unsigned ms) { Sleep(ms); }
typedef HANDLE Thr;
static DWORD WINAPI bg_main(LPVOID p);
static Thr thr_start(void) { return CreateThread(NULL, 0, bg_main, NULL, 0, NULL); }
static void thr_join(Thr t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); }
static DWORD WINAPI race_main(LPVOID p) { (void)p; race_write(); return 0; }
static Thr race_start(void) { return CreateThread(NULL, 0, race_main, NULL, 0, NULL); }
static DWORD WINAPI bg_main(LPVOID p) {
    (void)p;
    uint64_t t0 = host_span_begin();
    sleep_ms(3);
    host_span_end(HSPAN_AC_CHILD, t0, 7);
    return 0;
}
#else
#include <pthread.h>
#include <unistd.h>
static void sleep_ms(unsigned ms) { usleep(ms * 1000u); }
typedef pthread_t Thr;
static void *bg_main(void *p) {
    (void)p;
    uint64_t t0 = host_span_begin();
    sleep_ms(3);
    host_span_end(HSPAN_AC_CHILD, t0, 7);
    return NULL;
}
static Thr thr_start(void) { pthread_t t; pthread_create(&t, NULL, bg_main, NULL); return t; }
static void thr_join(Thr t) { pthread_join(t, NULL); }
static void *race_main(void *p) { (void)p; race_write(); return NULL; }
static Thr race_start(void) { pthread_t t; pthread_create(&t, NULL, race_main, NULL); return t; }
#endif

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

int main(void) {
    /* Frame stamps: monotonic, carry frame number and interpreter count. */
    host_frame_stamp(100, 11, 5);
    sleep_ms(2);
    host_frame_stamp(101, 25, 9);
    CHECK(host_frame_stamp_total() == 2);
    uint32_t fr = 0, ii = 0, ck = 0, pf = 0, io = 0; uint64_t t_a = 0, t_b = 0;
    CHECK(host_frame_stamp_get(0, &fr, &t_a, &ii, &ck, &pf, &io) && fr == 100 && ii == 11 && ck == 5);
    CHECK(host_frame_stamp_get(1, &fr, &t_b, &ii, &ck, &pf, &io) && fr == 101 && ii == 25 && ck == 9);
    CHECK(t_b >= t_a + 1500);                    /* ~2 ms apart */
    CHECK(!host_frame_stamp_get(2, &fr, &t_a, &ii, &ck, &pf, &io));   /* not written yet */

    /* A span shorter than HOST_SPAN_MIN_US is dropped; a long one is kept and tagged emu. */
    uint64_t s0 = host_span_total();
    host_span_record(HSPAN_PROVIDER_POLL, host_clock_us(), HOST_SPAN_MIN_US - 1, 1);
    CHECK(host_span_total() == s0);
    uint64_t t0 = host_span_begin();
    sleep_ms(2);
    host_span_end(HSPAN_LOADER_RESCAN, t0, 42);
    CHECK(host_span_total() == s0 + 1);
    HostSpan sp;
    CHECK(host_span_get(s0, &sp));
    CHECK(sp.kind == HSPAN_LOADER_RESCAN && sp.arg == 42 && sp.emu == 1 && sp.frame == 101);
    CHECK(sp.dur_us >= 1500 && sp.t0_us == t0);

    /* A span from another thread is attributed to the background. */
    Thr t = thr_start();
    thr_join(t);
    CHECK(host_span_total() == s0 + 2);
    CHECK(host_span_get(s0 + 1, &sp));
    CHECK(sp.kind == HSPAN_AC_CHILD && sp.emu == 0 && sp.arg == 7);

    /* Ring wrap: old sequence numbers age out, newest are readable. */
    for (unsigned i = 0; i < HOST_SPAN_RING_CAP + 10; i++)
        host_span_record(HSPAN_MOD_WORK, 1000 + i, 100, i);
    uint64_t total = host_span_total();
    CHECK(!host_span_get(s0, &sp));                       /* evicted */
    CHECK(host_span_get(total - 1, &sp) && sp.arg == HOST_SPAN_RING_CAP + 9);
    CHECK(host_span_get(total - HOST_SPAN_RING_CAP, &sp));  /* oldest still held */
    CHECK(!host_span_get(total, &sp));

    CHECK(host_span_kind_name(HSPAN_PUBLISH_COMMIT)[0] == 'p');
    CHECK(host_span_kind_name(9999)[0] == '?');

    /* Multiple writers wrap the ring while the emulation thread reads it.
     * A row may be unavailable, but an available row must be internally
     * consistent; reserving a sequence must not publish stale slot bytes. */
    uint64_t race_begin = host_span_total(), reads = 0;
    Thr a = race_start(), b = race_start();
    while (atomic_load(&writers_done) < 2u) {
        uint64_t end = host_span_total();
        uint64_t begin = end > 256u ? end - 256u : race_begin;
        if (begin < race_begin) begin = race_begin;
        for (uint64_t q = begin; q < end; ++q) {
            if (!host_span_get(q, &sp)) continue;
            CHECK(sp.kind == HSPAN_DISC_READ && sp.emu == 0);
            CHECK(sp.t0_us == (uint64_t)sp.arg * 1000u);
            CHECK(sp.dur_us == 50u + sp.arg);
            ++reads;
        }
    }
    thr_join(a); thr_join(b);
    CHECK(reads != 0);

    /* The frame ring wraps independently and preserves every counter. */
    for (unsigned i = 0; i < HOST_FRAME_STAMP_CAP + 2u; ++i)
        host_frame_stamp(i + 200u, i * 3u, i * 5u);
    total = host_frame_stamp_total();
    CHECK(!host_frame_stamp_get(0, NULL, NULL, NULL, NULL, NULL, NULL));
    CHECK(host_frame_stamp_get(total - 1, &fr, &t_a, &ii, &ck, &pf, &io));
    CHECK(fr == HOST_FRAME_STAMP_CAP + 201u);
    CHECK(ii == (HOST_FRAME_STAMP_CAP + 1u) * 3u && ck == (HOST_FRAME_STAMP_CAP + 1u) * 5u);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("host_span_ring_test: ok\n");
    return 0;
}
