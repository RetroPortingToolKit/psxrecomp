#ifndef PSX_REPLAY_IDENTITY_H
#define PSX_REPLAY_IDENTITY_H
/* The identity a player replay is recorded and played against: the
 * framework commit, the disc (boot serial and digest), the BIOS and the boot
 * mode. See replay_file.h for the wire form. */
#include "replay_file.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The running product: boot serial, disc image path (.cue or a single image)
 * and BIOS path. Clears the cached disc digest. */
void replay_identity_set_product(const char *disc_serial, const char *disc_path, const char *bios_path);
/* Fills `id` for the running product. The disc digest is taken once per
 * product and kept for the process. 1, or 0 with the reason in `why`. */
int replay_identity_current(ReplayIdentity *id, int call_hle, int boot_skip, char *why, size_t why_size);
/* The digest of one disc image path, CUE or FILE kind. NULL or the reason. */
const char *replay_identity_disc_digest(const char *path, uint32_t *kind, uint8_t out[32]);
/* "lle", "hle", "hle-calls" or "hle-boot". */
const char *replay_identity_boot_mode(int call_hle, int boot_skip);

/* The value of `key` in "key=value" lines, or "" when absent. */
void replay_identity_line_value(const char *lines, const char *key, char *out, size_t cap);

typedef enum {
    REPLAY_IDENTITY_REFUSE = 0,     /* another disc, BIOS or boot mode */
    REPLAY_IDENTITY_SAME_BUILD = 1,
    REPLAY_IDENTITY_OTHER_BUILD = 2 /* plays, with a warning */
} ReplayIdentityMatch;

/* Compares a replay's identity and product lines with the player's. The disc
 * serial and digest must match. The BIOS matches by the bios_crc32 product
 * line when both sides have one, otherwise by file stem (case-insensitive).
 * The boot mode must match. Another commit, or another exe_sha256 when both
 * sides name one, is another build. On a refusal `why` holds the reason. */
ReplayIdentityMatch replay_identity_check(const ReplayIdentity *recorded, const char *recorded_product,
                                          const ReplayIdentity *player, const char *player_product,
                                          char *why, size_t why_size);

#ifdef __cplusplus
}
#endif
#endif
