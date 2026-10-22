#ifndef PSX_GTE_VIEW_H
#define PSX_GTE_VIEW_H
#include <stdint.h>
#include "mod_plugins.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Host enhancement ambient, checkpointed by render transactions. Identity
 * outside an explicitly scoped render callback; never writes guest TR regs. */
void gte_render_view_get(int32_t xyz[3]);
void gte_render_view_set(const int32_t xyz[3]);
void gte_render_pose_get(PSXModRenderView *view);
void gte_render_pose_set(const PSXModRenderView *view);
typedef struct GTEExecutionSnapshot GTEExecutionSnapshot;
GTEExecutionSnapshot *gte_execution_snapshot_create(void);
void gte_execution_snapshot_destroy(GTEExecutionSnapshot *snapshot);
int gte_execution_snapshot_capture(GTEExecutionSnapshot *snapshot);
int gte_execution_snapshot_bind(GTEExecutionSnapshot *snapshot);
void gte_execution_snapshot_unbind(void);
#ifdef __cplusplus
}
#endif
#endif
