#ifndef PSX_REPLAY_INPUT_SCOPE_H
#define PSX_REPLAY_INPUT_SCOPE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Static standalone-pad configuration and actual inputs representable by the
 * single-player replay format. Guest-driven mode negotiation is permitted. */
int replay_sio_input_contract(uint64_t *profile, char *why, size_t cap);
#ifdef __cplusplus
}
#endif
#endif
