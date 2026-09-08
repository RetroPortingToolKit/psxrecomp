/* psx_netplay_auth.h -- Discord sign-in and the device key, for the launcher.
 *
 * Backs recomp-ui's optional `account_*` netplay callbacks. Everything here is
 * optional at runtime: a lobby server whose operator never configured Discord
 * answers 503 to a login start, and this reports "not available" so the
 * launcher draws no sign-in at all. A player who never signs in is a guest,
 * which is what the runtime has always been.
 *
 * Two credentials, and the difference matters:
 *
 *   netplay_secret  Long-lived, per device, written to a file beside the
 *                   config. NEVER sent anywhere. Proved by HMAC over a
 *                   server nonce, because this runtime has no TLS and a
 *                   permanent bearer token in cleartext would be captured
 *                   once and reused forever.
 *   session         Short-lived JWT, minted from that proof, and the only
 *                   thing that goes to the lobby (in `hello`).
 *
 * The secret file is what makes a browserless device work: sign in once on a
 * PC, and the file travels with a build installed on a handheld. It is a
 * password in a file -- do not put it in anything you distribute.
 */
#ifndef PSX_NETPLAY_AUTH_H
#define PSX_NETPLAY_AUTH_H

#ifdef __cplusplus
extern "C" {
#endif

/* Mirrors recomp-ui's RECOMP_LAUNCHER_ACCOUNT_* so this header does not
 * depend on the launcher's. */
enum {
    PSX_ACCOUNT_GUEST = 0,
    PSX_ACCOUNT_WAITING = 1,
    PSX_ACCOUNT_SIGNED_IN = 2,
    PSX_ACCOUNT_FAILED = 3
};

/* `ws_url` is the lobby URL the client is configured with; the HTTP endpoints
 * live on the same host and port. Safe to call again when the URL changes. */
void psx_account_init(const char *ws_url);

/* Call once per frame. Drives the worker handshake and, when a stored key
 * exists, redeems it for a session on first use. */
void psx_account_pump(void);

int  psx_account_available(void);
int  psx_account_login_begin(void);
int  psx_account_state(void);
const char *psx_account_handle(void);
const char *psx_account_username(void);
const char *psx_account_error(void);
int  psx_account_sign_out(void);
int  psx_account_set_handle(const char *handle);

/* The session for `hello`, or "" when this client is a guest. */
const char *psx_account_session(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_AUTH_H */
