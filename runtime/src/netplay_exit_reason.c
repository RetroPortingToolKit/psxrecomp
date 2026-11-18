/* Why a netplay match ended, in words. See netplay_exit_reason.h. */
#include "netplay_exit_reason.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    const char *origin;
    const char *text;
} NetplayExitReason;

static const NetplayExitReason k_reasons[] = {
    { "netplay_boot_mismatch",
      "The match ended: the players' consoles started differently "
      "(different BIOS image or boot settings)." },
    { "netplay_peer_disconnect",
      "The match ended: the other player left or stopped responding." },
    { "netplay_load_failed",
      "The match ended: a player could not load the shared save state." },
    { "netplay_load_stall",
      "The match ended: loading the shared save state timed out." },
    { "netplay_link_stall",
      "The match ended: could not connect to the other player in time." },
    { "netplay_admit_stall",
      "The match ended: the players' games stopped running in step." },
};

const char *netplay_exit_reason_text(const char *origin)
{
    size_t i;
    if (!origin) return NULL;
    for (i = 0; i < sizeof(k_reasons) / sizeof(k_reasons[0]); ++i) {
        if (strcmp(k_reasons[i].origin, origin) == 0)
            return k_reasons[i].text;
    }
    return NULL;
}

/* What a refused bind means, from the system's error number. The numbers of
 * Windows, Linux and macOS do not overlap for these three causes. */
enum {
    BIND_ERROR_OTHER = 0,
    BIND_ERROR_IN_USE,       /* WSAEADDRINUSE 10048, EADDRINUSE 98 (Linux), 48 (macOS) */
    BIND_ERROR_NOT_ALLOWED,  /* WSAEACCES 10013, EACCES 13 */
    BIND_ERROR_NO_SUCH_ADDR  /* WSAEADDRNOTAVAIL 10049, EADDRNOTAVAIL 99 (Linux), 49 (macOS) */
};

static int bind_error_kind(int sys_error)
{
    switch (sys_error) {
    case 10048: case 98: case 48: return BIND_ERROR_IN_USE;
    case 10013: case 13:          return BIND_ERROR_NOT_ALLOWED;
    case 10049: case 99: case 49: return BIND_ERROR_NO_SUCH_ADDR;
    default:                      return BIND_ERROR_OTHER;
    }
}

/* The system's own text goes into a sentence only when it is short and plain
 * ASCII: a text in the system's language and code page is not shown as
 * broken letters, and a long one is not cut in the middle. */
static int sys_text_fits(const char *text)
{
    size_t n = 0;
    if (!text || !text[0]) return 0;
    for (; text[n]; ++n) {
        if ((unsigned char)text[n] < 0x20 || (unsigned char)text[n] > 0x7e)
            return 0;
    }
    return n <= 80;
}

void netplay_start_failure_text(int start_rc, int has_netplay,
                                const char *bind_hostport,
                                const char *peer_hostport, int bind_probe,
                                int sys_error, const char *sys_text,
                                char *out, size_t cap)
{
    char address[64] = "";
    char system[112] = "";
    const char *port = "";
    const char *colon;
    if (!out || !cap) return;
    out[0] = '\0';
    if (!bind_hostport) bind_hostport = "";
    if (!peer_hostport) peer_hostport = "";
    /* "address:port": the port is what follows the last colon. No address
     * means every address of this computer. */
    colon = strrchr(bind_hostport, ':');
    if (colon && (size_t)(colon - bind_hostport) < sizeof(address)) {
        memcpy(address, bind_hostport, (size_t)(colon - bind_hostport));
        address[colon - bind_hostport] = '\0';
        port = colon + 1;
        if (!address[0]) snprintf(address, sizeof(address), "0.0.0.0");
    }
    /* "(system error N)" or "(system error N: the system's text)". */
    if (sys_text_fits(sys_text))
        snprintf(system, sizeof(system), "(system error %d: %s)", sys_error, sys_text);
    else
        snprintf(system, sizeof(system), "(system error %d)", sys_error);
    if (!has_netplay) {
        snprintf(out, cap, "This build has no netplay: it was built without "
                           "the netplay library.");
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_FAILED &&
               bind_error_kind(sys_error) == BIND_ERROR_NOT_ALLOWED) {
        /* Access denied. The system does not hand the port out: a port range
         * that Windows keeps for Hyper-V or WSL, a port another program
         * holds for itself alone on Windows, or a port below 1024 elsewhere.
         * "Another program holds it" would send the player looking for a
         * program that may not exist; another port helps in every case. */
        snprintf(out, cap,
                 "Netplay could not open UDP port %.5s on %.15s (system error "
                 "%d). The operating system does not allow this port. Choose "
                 "another port and start again.",
                 port, address, sys_error);
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_FAILED &&
               bind_error_kind(sys_error) == BIND_ERROR_IN_USE) {
        snprintf(out, cap,
                 "Netplay could not open UDP port %.5s on %.15s (system error "
                 "%d). Another program or the operating system holds that "
                 "port. Choose another port and start again.",
                 port, address, sys_error);
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_FAILED &&
               bind_error_kind(sys_error) == BIND_ERROR_NO_SUCH_ADDR) {
        /* No port is held and another port does not help: the listen address
         * is not an address of this computer (a copied command line, a
         * network that is down). */
        snprintf(out, cap,
                 "Netplay could not listen on %.15s (system error %d). That "
                 "address is not one of this computer's addresses. Check the "
                 "listen address and start again.",
                 address, sys_error);
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_FAILED) {
        /* Any other answer: the number and the system's text, no cause. */
        snprintf(out, cap, "Netplay could not open UDP port %.5s on %.15s %s.",
                 port, address, system);
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_NO_SOCKET) {
        snprintf(out, cap, "Netplay could not open a network socket %s.", system);
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_BAD_ADDRESS) {
        snprintf(out, cap,
                 "Netplay could not start: \"%.60s\" is not an address and a "
                 "port to listen on.", bind_hostport);
    } else if (start_rc == -3 && bind_probe == NETPLAY_BIND_PEER_BAD) {
        snprintf(out, cap,
                 "Netplay could not start: the other player's address "
                 "\"%.60s\" could not be used.", peer_hostport);
    } else if (start_rc == -3) {
        /* The listen address opened and the peer address reads well, or
         * nothing was tried: no cause is known. */
        snprintf(out, cap,
                 "Netplay could not start its connection on %.60s.",
                 bind_hostport);
    } else if (start_rc == -4) {
        /* Five causes share this code (a missing peer or relay setting, a
         * relay start that failed, a build without the relay library); the
         * code does not say which. */
        snprintf(out, cap, "Netplay could not start the online connection.");
    } else {
        snprintf(out, cap, "Netplay could not start (code %d).", start_rc);
    }
}

int netplay_boot_mode_settle(const NetplayBootMode *game, NetplayBootMode *mode)
{
    int changed;
    if (!game || !mode) return 0;
    /* Compare as on/off: a caller may hand in any non-zero value for on. */
    changed = (!game->bios_hle != !mode->bios_hle) ||
              (!game->keep_intro != !mode->keep_intro) ||
              (!game->fast_boot != !mode->fast_boot);
    mode->bios_hle   = game->bios_hle ? 1 : 0;
    mode->keep_intro = game->keep_intro ? 1 : 0;
    mode->fast_boot  = game->fast_boot ? 1 : 0;
    return changed;
}

int netplay_boot_mismatch_final(uint32_t mismatch_since_ms, uint32_t now_ms,
                                uint32_t grace_ms)
{
    if (!mismatch_since_ms) return 0;
    return (uint32_t)(now_ms - mismatch_since_ms) >= grace_ms;
}
