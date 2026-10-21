#ifndef PSX_INPUT_REPLAY_H
#define PSX_INPUT_REPLAY_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Optional host-input replay in every build. Text rows: frame count, hex pad
 * word (active low). Starts at the first VBlank after a disk state load.
 * No guest state is stored here; repeated reads of a frame are idempotent. */
int input_replay_load(const char *path);
void input_replay_on_load(uint64_t next_frame);
int input_replay_buttons(uint64_t frame);
uint64_t input_replay_frame(uint64_t frame); /* UINT64_MAX while inactive */
#ifdef __cplusplus
}
#endif
#endif
