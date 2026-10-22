/* Concurrent worker capture never becomes a second ring producer. */
#include "render_thread.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define CHECK(c) do { if (!(c)) abort(); } while (0)
static RtCommandList *list;
static atomic_int phase, executed;
static unsigned live_next, private_next;
static jmp_buf overflow;
static void fail_capture(const char *reason) {
    CHECK(!strcmp(reason, "command_capacity")); longjmp(overflow, 1);
}
static void consume_private(void *user, const RtCmd *cmd, const void *payload) {
    (void)user;
    CHECK(rt_on_render_thread());
    CHECK(cmd->op == 7 && cmd->payload == sizeof(uint32_t));
    CHECK(*(const uint32_t *)payload == private_next++);
}
static void consume(void *user, const RtCmd *cmd, const void *payload) {
    (void)user;
    if (cmd->op == 1) {
        CHECK(*(const uint32_t *)payload == live_next++);
    } else {
        CHECK(cmd->op == 2 && atomic_load_explicit(&phase, memory_order_acquire) == 2);
        rt_list_execute(list, consume_private, NULL);
        CHECK(private_next == 4096);
        atomic_store_explicit(&executed, 1, memory_order_release);
    }
}
static void context(void *user, int current) { (void)user; (void)current; }
static void *worker(void *user) {
    (void)user;
    rt_capture_begin(list, fail_capture);
    CHECK(rt_capture_active());
    atomic_store_explicit(&phase, 1, memory_order_release);
    for (uint32_t i = 0; i < 4096; ++i) {
        uint32_t *p = rt_cmd_begin(7, 0, sizeof(i));
        CHECK(p); *p = i; rt_cmd_commit();
    }
    rt_capture_end();
    CHECK(!rt_capture_active() && rt_list_bytes(list) == 4096 * 32u);
    atomic_store_explicit(&phase, 2, memory_order_release);
    return NULL;
}
int main(void) {
    RtConfig cfg = {0};
    cfg.ring_bytes = 1u << 20; cfg.max_frames = 2;
    cfg.exec = consume; cfg.ctx = context;
    CHECK(rt_start(&cfg));
    list = rt_list_create(1u << 20); CHECK(list);
    pthread_t thread; CHECK(!pthread_create(&thread, NULL, worker, NULL));
    while (!atomic_load_explicit(&phase, memory_order_acquire)) { }
    for (uint32_t i = 0; i < 16000; ++i) {
        uint32_t *p = rt_cmd_begin(1, 0, sizeof(i));
        CHECK(p); *p = i; rt_cmd_commit();
    }
    CHECK(!pthread_join(thread, NULL));
    CHECK(rt_cmd_begin(2, 0, 0)); rt_cmd_commit();
    rt_drain();
    CHECK(live_next == 16000 && atomic_load_explicit(&executed, memory_order_acquire));
    RtStats stats; rt_get_stats(&stats); CHECK(stats.records == 16001);
    rt_stop();
    /* Overflow abandons the extra; it cannot acquire or block the live ring. */
    RtCommandList *small = rt_list_create(32); CHECK(small);
    rt_capture_begin(small, fail_capture);
    if (!setjmp(overflow)) {
        CHECK(rt_cmd_begin(1, 0, 4)); rt_cmd_commit();
        rt_cmd_begin(1, 0, 4); abort();
    }
    rt_capture_end(); CHECK(rt_list_bytes(small) == 32);
    rt_list_destroy(small); rt_list_destroy(list);
    return 0;
}
