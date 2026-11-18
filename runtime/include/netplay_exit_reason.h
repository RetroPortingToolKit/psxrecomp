/* netplay_exit_reason.h — why a netplay match ended, in words.
 *
 * A match that ends early returns every player to the lobby. The runtime knew
 * why (netplay_soft_exit's origin) but only printed it to stdout, so players
 * saw the lobby again with no reason. These map an origin to one sentence for
 * the launcher's status line, and decide when a boot-digest mismatch is final.
 */
#ifndef NETPLAY_EXIT_REASON_H
#define NETPLAY_EXIT_REASON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The player-facing sentence for a netplay_soft_exit origin, or NULL when the
 * player ended the match themselves (window close, Escape) or the origin is
 * unknown. */
const char *netplay_exit_reason_text(const char *origin);

/* The rollback boot digests of the two peers are both known and different.
 * The peers booted differently, and waiting does not fix that; before this change
 * only the generic 20 s admit-stall watchdog ended it. The mismatch is final
 * once the same pair of digests has held for grace_ms (the short wait lets a
 * stale peer digest be replaced, and the peer reach the same verdict).
 * mismatch_since_ms is 0 when there is no mismatch. */
int netplay_boot_mismatch_final(uint32_t mismatch_since_ms, uint32_t now_ms,
                                uint32_t grace_ms);

#define NETPLAY_BOOT_MISMATCH_GRACE_MS 3000u

/* Why a netplay start failed, in one sentence. The start reported
 * every failure as "built without recomp-net, or bind/peer invalid": a player
 * whose system held the port read first that the build had no netplay.
 * `start_rc` is psx_netplay_start's result, `has_netplay` whether the build
 * has the netplay library. For a LAN start (-3) the caller does the library's
 * steps again and passes what it found: `bind_probe` and, for a refused bind
 * or socket, the system's error number and text. The sentence says a cause
 * only when the system's answer names one:
 *   address in use (Windows 10048, Linux 98, macOS 48): another program or
 *     the system holds the port, choose another port;
 *   access denied (Windows 10013, POSIX 13): the system does not allow the
 *     port (a range Windows keeps for Hyper-V or WSL, a port below 1024
 *     elsewhere), choose another port;
 *   address not available (Windows 10049, Linux 99, macOS 49): the listen
 *     address is not one of this computer's, check the listen address;
 *   any other number: the number and the system's text, no cause.
 * The peer address is named only when the listen address opened and the peer
 * address cannot be read. The build is named only when it has no netplay.
 * Every sentence fits the lobby client's last_error. */
enum {
    NETPLAY_BIND_NOT_TRIED   = -1, /* not a LAN start, or a build without the library */
    NETPLAY_BIND_OK          = 0,  /* listen address opened, peer address reads well */
    NETPLAY_BIND_FAILED      = 1,  /* the system refused the bind */
    NETPLAY_BIND_BAD_ADDRESS = 2,  /* the listen address is not "address:port", or its name is unknown */
    NETPLAY_BIND_NO_SOCKET   = 3,  /* the system made no socket */
    NETPLAY_BIND_PEER_BAD    = 4   /* listen address opened; the peer address cannot be read */
};
#define NETPLAY_START_FAILURE_CAP 192
void netplay_start_failure_text(int start_rc, int has_netplay,
                                const char *bind_hostport,
                                const char *peer_hostport, int bind_probe,
                                int sys_error, const char *sys_text,
                                char *out, size_t cap);

/* The peers of a match must boot the BIOS the same way. The boot has three
 * inputs, each resolved per player: the game's game.toml, then the player's
 * settings.toml, then the launcher. Two peers of one build that differed
 * started a match and forked for good at sim 22, with nothing said. A match
 * therefore boots with the game.toml values, which both peers share.
 * Sets `*mode` to `*game`. Returns 1 when that changed a value (the player had
 * chosen otherwise, and is shown NETPLAY_BOOT_MODE_NOTICE), 0 when it did not. */
typedef struct NetplayBootMode {
    int bios_hle;    /* [runtime] bios_hle */
    int keep_intro;  /* [runtime] bios_hle_keep_intro */
    int fast_boot;   /* [runtime] fast_boot (alias of the boot skip) */
} NetplayBootMode;
#define NETPLAY_BOOT_MODE_NOTICE \
    "Netplay: this match uses the game's own BIOS setting."
int netplay_boot_mode_settle(const NetplayBootMode *game, NetplayBootMode *mode);

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_EXIT_REASON_H */
