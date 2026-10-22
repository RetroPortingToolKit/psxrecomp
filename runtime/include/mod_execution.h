#ifndef PSX_MOD_EXECUTION_H
#define PSX_MOD_EXECUTION_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct ModExecutionSnapshot ModExecutionSnapshot;
void mod_execution_seal(void);
ModExecutionSnapshot *mod_execution_create(void);
void mod_execution_destroy(ModExecutionSnapshot *snapshot);
void mod_execution_capture(ModExecutionSnapshot *snapshot);
void mod_execution_bind(const ModExecutionSnapshot *snapshot);
int mod_execution_registered(const char *plugin_id);
#ifdef __cplusplus
}
#endif
#endif
