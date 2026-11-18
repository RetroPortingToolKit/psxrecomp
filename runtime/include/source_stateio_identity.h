#ifndef PSX_SOURCE_STATEIO_IDENTITY_H
#define PSX_SOURCE_STATEIO_IDENTITY_H

/* Checkpoint identity dimensions. A state saved by one executable, under one
 * configuration, over one input route means nothing in another, so all three
 * are part of the manifest (source_tas_stateio.h) and all three are checked at
 * resume.
 *
 * The configuration is the whole resolved PSX_* environment minus an explicit
 * allow-list (source_tas_stateio_env_allowed_to_differ), not a hand-written
 * flag list, so a newly added model variable is covered without anyone having
 * to remember it. */
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Lowercase hex SHA-256 of a file. 0, with out[0] = 0, when it cannot be read. */
int source_stateio_file_sha256(const char *path, char out[65]);
/* Both computed once per process: the executable and PSX_INPUT_ROUTE_FILE cannot
 * change during a run, and hashing them is the slow part of a capture. */
int source_stateio_exe_sha256(char out[65]);
int source_stateio_route_sha256(char out[65]);
/* SHA-256 over the sorted identity-relevant PSX_* NAME=value entries. */
int source_stateio_config_digest_hex(char out[65]);
/* 1 when a stored manifest digest equals the current configuration's. */
int source_stateio_config_digest_accepts(const char *stored);
#ifdef __cplusplus
}
#endif

#endif
