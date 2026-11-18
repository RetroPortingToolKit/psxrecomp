/* netplay_lan_version.h — the build and game checks for LAN / Direct IP rooms.
 *
 * The online lobby server refuses a join whose game_version differs from the
 * room's. A LAN / Direct IP room has no server, and its JOIN carried no
 * version, so two builds of different framework commits could meet there and
 * desync. The JOIN tail now ends with the guest's lobby version (line 7, after
 * the three BIOS lines, the two memory-card lines and the BIOS CRC), and the
 * host applies the server's rule.
 */
#ifndef NETPLAY_LAN_VERSION_H
#define NETPLAY_LAN_VERSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The JOIN tail line that carries the version, counted from 0. */
#define NETPLAY_LAN_VERSION_LINE 6

/* Copy the version line of a JOIN tail into out. out is "" when the tail is
 * NULL or too short (a guest from before this check). Does not modify tail. */
void netplay_lan_join_version(const char *tail, char *out, size_t out_cap);

/* 1 when a guest that sent guest_version may join a host on host_version.
 * The online server's rule: a missing or blank version counts as "dev", and
 * the two must then be equal. So release builds of one commit meet, dev builds
 * meet each other and older guests, and nothing else does. */
int netplay_lan_version_ok(const char *host_version, const char *guest_version);

/* What the guest is told when the host refuses it for this reason. */
#define NETPLAY_LAN_VERSION_GUEST_TEXT \
    "That room runs another build of the game. Both players need the same release."
/* What the host is told. */
#define NETPLAY_LAN_VERSION_HOST_TEXT \
    "A player with another build of the game tried to join. Both players need the same release."

/* The game check. The online server also refuses a join whose game_name is
 * not the room's; a LAN JOIN carried no name, so any two titles on the same
 * BIOS and framework commit could be seated together. The tail's next line
 * (line 8) is the guest's lobby game name, the title's [game] name. */
#define NETPLAY_LAN_GAME_LINE 7

/* Copy the game-name line of a JOIN tail into out; "" when the tail is NULL or
 * ends before it. Control characters become '?'; other bytes are kept, so a
 * UTF-8 name survives. Does not modify tail. */
void netplay_lan_join_game(const char *tail, char *out, size_t out_cap);

/* 1 when the two game names are the same, ignoring surrounding blanks. A
 * missing or blank name never matches: a guest that sends no game line is
 * refused, as a guest that sends another version is. The same test decides
 * whether the same-machine room file belongs to this title. */
int netplay_lan_game_ok(const char *host_game, const char *guest_game);

/* A guest that sends no game line at all is a build from before this check,
 * so both sentences name that case too. */
#define NETPLAY_LAN_GAME_GUEST_TEXT \
    "That room is for a different game, or the host runs a newer build. " \
    "Both players need the same game and release."
#define NETPLAY_LAN_GAME_HOST_TEXT \
    "A player with a different game, or an older build, tried to join. " \
    "Both players need the same game and release."

#ifdef __cplusplus
}
#endif

#endif /* NETPLAY_LAN_VERSION_H */
