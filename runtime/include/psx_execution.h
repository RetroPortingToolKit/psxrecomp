#ifndef PSX_EXECUTION_H
#define PSX_EXECUTION_H
#include <stdint.h>
#include "psx_thread_local.h"
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;
typedef struct PsxExecutionCode {
    void (*function)(struct CPUState *);
    uint32_t resume_pc;
} PsxExecutionCode;
typedef void (*PsxExecutionDispatch)(struct CPUState *, uint32_t, uint32_t);
typedef void (*PsxExecutionAbort)(const char *, uint32_t);
/* A private execution supplies its sealed dispatch table and abort landing.
 * NULL selects the ordinary runtime. Never replace these on another thread. */
extern PSX_THREAD_LOCAL PsxExecutionDispatch g_psx_execution_dispatch;
extern PSX_THREAD_LOCAL PsxExecutionAbort g_psx_execution_abort;
#ifdef __cplusplus
}
#endif
#endif
