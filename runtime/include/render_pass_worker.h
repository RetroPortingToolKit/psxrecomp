#ifndef PSX_RENDER_PASS_WORKER_H
#define PSX_RENDER_PASS_WORKER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void render_pass_worker_stop(void);
/* Stop executing before replacing immutable activation tables. */
void render_pass_worker_quiesce(void);
void render_pass_worker_flush(void);
uint32_t render_pass_worker_take_plan(uint32_t *period);
typedef struct RenderPassWorkerStats {
    uint64_t published, completed, late, aborted, refused, dispatch_misses, native_calls;
    uint32_t last_address;
    const char *last_failure;
} RenderPassWorkerStats;
void render_pass_worker_stats(RenderPassWorkerStats *out);
#ifdef __cplusplus
}
#endif
#endif
