#ifndef PSX_REPLAY_SESSION_H
#define PSX_REPLAY_SESSION_H
/* Player replay playback.
 *
 * A replay (replay_file.h) holds the P1 pad once per guest vblank, where it
 * starts (an anchor save state, or power-on with the memory cards the guest
 * booted with), the host settings that change guest timing, state digests
 * and an END checkpoint. Playback starts from the same place, feeds P1 from
 * the file at every vblank boundary, compares the digests on the way and the
 * END checkpoint (cycle and RAM) at the end, and reports whether the guest
 * stayed in sync.
 *
 * The module owns no guest or host state; main.cpp supplies the
 * replay_host_* hooks below (the unit test supplies stubs). */
#include <stddef.h>
#include <stdint.h>
#include "replay_file.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    REPLAY_IDLE = 0,
    REPLAY_LOADING,    /* the anchor load was requested */
    REPLAY_PLAYING
} ReplayState;

typedef enum {
    REPLAY_RESULT_NONE = 0,
    REPLAY_RESULT_IN_SYNC,
    REPLAY_RESULT_OUT_OF_SYNC,
    REPLAY_RESULT_TAKEN_OVER,
    REPLAY_RESULT_FAILED
} ReplayResult;

ReplayState replay_session_state(void);
ReplayResult replay_session_last_result(void);

/* Starts playing `path`. A power-on replay must start at the first vblank
 * of a cold boot (replay_host_at_power_on). Returns 1 when playback starts;
 * 0 with the reason on the OSD and stderr when it is refused. */
int replay_session_play_file(const char *path);

/* 1 while a replay drives P1. */
int replay_session_owns_p1(void);

/* Once per guest vblank, at the frame boundary, with the live P1 state the
 * host would deliver (sticks LX,LY,RX,RY). Returns 1 when the host must
 * deliver *out_buttons and out_sticks to P1 for the next frame instead. Live
 * input takes over once the pad has been neutral after the start. */
int replay_session_boundary(uint16_t live_buttons, const uint8_t live_sticks[4],
                            uint16_t *out_buttons, uint8_t out_sticks[4]);

/* State digests (netplay_state_digest.h) recorded every
 * REPLAY_DIGEST_INTERVAL frames. CORE and EXT decide the result; AV (GPU and
 * VRAM) is reported only, because renderer readback can fork VRAM while the
 * guest still matches. */
#define REPLAY_DIGEST_INTERVAL 60u
#define REPLAY_DIGEST_CORE 1u
#define REPLAY_DIGEST_AV   2u
#define REPLAY_DIGEST_AUX  4u
#define REPLAY_DIGEST_EXT  8u
/* 1 when the last playback's digests diverged: the first frame and the
 * parts that differed there. */
int replay_session_first_divergence(uint32_t *frame, unsigned *parts);
unsigned replay_session_digests_checked(void);

/* When set, every playback writes this JSON file when it ends: schema
 * "psxrecomp-replay-verdict/1", result (in_sync, diverged,
 * stopped_by_input, failed, incomplete), frames, first divergence, the
 * recorded and player builds and exe SHA-256s, power_on. NULL clears. */
void replay_session_set_verdict_path(const char *path);

/* Process exit, a disc change or a session restart: end a playback and put
 * the player's settings and cards back. */
void replay_session_shutdown(void);

/* ---- Host hooks (main.cpp; stubs in the unit test) ---- */
void replay_host_osd(const char *text, int ms);
/* The identity of the running product (replay_identity_current). */
int replay_host_identity(ReplayIdentity *id, char *why, size_t cap);
/* "key=value\n" lines naming this product: exe_sha256, codegen, bios_crc32,
 * renderer, platform. */
void replay_host_product(char *out, size_t cap);
/* 1 only at the first vblank boundary of a cold boot, before any save state
 * was loaded: the one place a power-on replay can start. */
int replay_host_at_power_on(void);
/* Load a state blob at the next safe boundary, without the save-state
 * toast. 1 when the load was staged. */
int replay_host_load_anchor(const void *data, size_t size);
/* 1 = loaded, -1 = failed, 0 = pending. */
int replay_host_take_load_result(void);
/* Switch to `settings` ("key=value" lines), keeping the player's to restore;
 * keys that could not be switched are listed in `differs`. */
void replay_host_settings_apply(const char *settings, char *differs, size_t cap);
void replay_host_settings_restore(void);
/* The guest sees these cards (2 x REPLAY_FILE_CARD_BYTES, slot order; only
 * the inserted ones are read). Its writes stay in memory. 0 on failure. */
int replay_host_cards_install(const uint8_t *images, uint32_t mask);
void replay_host_cards_restore(void);
/* Guest main RAM (2 MiB) and cycle count at the current boundary. */
const uint8_t *replay_host_ram(void);
uint64_t replay_host_cycle(void);
/* core, av, aux, ext; 0 when no digest is available. */
int replay_host_state_digest(uint32_t out[4]);

/* The "platform" product line: "<os>-<arch>". The same build on another
 * platform has another exe; playback says so instead of warning about a
 * different build. */
static inline const char *replay_platform_name(void)
{
#ifdef _WIN32
#define REPLAY_PLATFORM_OS "windows"
#elif defined __APPLE__
#define REPLAY_PLATFORM_OS "macos"
#elif defined __linux__
#define REPLAY_PLATFORM_OS "linux"
#else
#define REPLAY_PLATFORM_OS "unknown"
#endif
#if defined(__x86_64__) || defined(_M_X64)
#define REPLAY_PLATFORM_ARCH "x64"
#elif defined(__aarch64__) || defined(_M_ARM64)
#define REPLAY_PLATFORM_ARCH "arm64"
#else
#define REPLAY_PLATFORM_ARCH "unknown"
#endif
    return REPLAY_PLATFORM_OS "-" REPLAY_PLATFORM_ARCH;
}

#ifdef __cplusplus
}
#endif
#endif
