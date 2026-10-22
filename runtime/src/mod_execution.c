/* Trusted plugin execution images, separate from savestates and guest state. */
#include "mod_execution.h"
#include "mod_plugins.h"
#include <stdlib.h>
#include <string.h>
#define CONTEXT_CAP 32
static PSXModExecutionState s_contexts[CONTEXT_CAP];
static size_t s_context_count;
static int s_sealed;
struct ModExecutionSnapshot {
    PSXModExecutionState contexts[CONTEXT_CAP];
    size_t count, bytes;
    unsigned char *data;
};
int mod_execution_registered(const char *id) {
    if (!id) return 0;
    for (size_t i = 0; i < s_context_count; ++i)
        if (!strcmp(s_contexts[i].plugin_id, id)) return 1;
    return 0;
}
int psx_mod_register_execution_state(const PSXModExecutionState *state) {
    if (!state || state->struct_size != sizeof(*state) || !state->plugin_id ||
        (state->bytes && (!state->capture || !state->bind)) || s_sealed || s_context_count == CONTEXT_CAP ||
        mod_execution_registered(state->plugin_id)) return 0;
    s_contexts[s_context_count++] = *state;
    return 1;
}
static size_t aligned_size(size_t bytes) {
    if (bytes > SIZE_MAX - 15u) abort();
    return (bytes + 15u) & ~(size_t)15u;
}
ModExecutionSnapshot *mod_execution_create(void) {
    ModExecutionSnapshot *snapshot = (ModExecutionSnapshot *)calloc(1, sizeof(*snapshot));
    if (!snapshot) return NULL;
    s_sealed = 1;
    snapshot->count = s_context_count;
    memcpy(snapshot->contexts, s_contexts, s_context_count * sizeof(s_contexts[0]));
    for (size_t i = 0; i < snapshot->count; ++i) {
        size_t bytes = aligned_size(snapshot->contexts[i].bytes);
        if (bytes > SIZE_MAX - snapshot->bytes) { free(snapshot); return NULL; }
        snapshot->bytes += bytes;
    }
    snapshot->data = (unsigned char *)calloc(1, snapshot->bytes ? snapshot->bytes : 1);
    if (!snapshot->data) { free(snapshot); return NULL; }
    return snapshot;
}
void mod_execution_destroy(ModExecutionSnapshot *snapshot) {
    if (snapshot) { free(snapshot->data); free(snapshot); }
}
void mod_execution_capture(ModExecutionSnapshot *snapshot) {
    size_t offset = 0;
    if (!snapshot || snapshot->count != s_context_count) abort();
    for (size_t i = 0; i < snapshot->count; ++i) {
        if (snapshot->contexts[i].capture) snapshot->contexts[i].capture(snapshot->data + offset);
        offset += aligned_size(snapshot->contexts[i].bytes);
    }
}
void mod_execution_bind(const ModExecutionSnapshot *snapshot) {
    size_t offset = 0;
    if (!snapshot) abort();
    for (size_t i = 0; i < snapshot->count; ++i) {
        if (snapshot->contexts[i].bind) snapshot->contexts[i].bind(snapshot->data + offset);
        offset += aligned_size(snapshot->contexts[i].bytes);
    }
}
