/* psx_netplay_auth.c -- see psx_netplay_auth.h.
 *
 * All network work happens on one worker thread; the launcher only ever reads
 * flags this file publishes. That is the same shape the lobby client already
 * uses for its off-thread connect, and it is why a login cannot stall a frame.
 */

#include "psx_netplay_auth.h"
#include "psx_sha256.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <process.h>
#include <windows.h>
#define close closesocket
typedef HANDLE auth_thread_t;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef pthread_t auth_thread_t;
#endif

#define SECRET_MAX 96
#define SESSION_MAX 1024
#define NAME_MAX_LEN 64
#define ID_MAX 64

/* What the worker is doing. The launcher polls psx_account_state(). */
enum { JOB_NONE = 0, JOB_LOGIN, JOB_SESSION, JOB_SET_HANDLE, JOB_SIGN_OUT };

static struct {
    char host[192];
    int  port;

    /* Published to the launcher. Written by the worker, read on the UI
     * thread; single-writer plus aligned word stores, which is what the
     * lobby client already relies on for its connect flags. */
    volatile int state;       /* PSX_ACCOUNT_* */
    volatile int available;   /* -1 unknown, 0 no logins here, 1 yes */
    volatile int busy;

    char secret[SECRET_MAX];
    char session[SESSION_MAX];
    char player_id[ID_MAX];
    char handle[NAME_MAX_LEN];
    char username[NAME_MAX_LEN];
    char error[192];

    /* Login in flight. */
    char pair_code[128];
    char pending_handle[NAME_MAX_LEN];

    int job;
    int thread_valid;
    auth_thread_t thread;
    int initialized;
    int tried_stored_key;
} g;

/* ---- small helpers ------------------------------------------------------ */

static void set_err(const char *e) {
    snprintf(g.error, sizeof(g.error), "%s", e ? e : "");
}

/* Minimal extraction for the flat, server-generated JSON these endpoints
 * return. Not a parser: it is the same shape psx_lobby_client.c already uses,
 * and the bodies come from our own server. */
static int json_str(const char *json, const char *key, char *out, size_t cap) {
    char pat[64];
    const char *p;
    size_t o = 0;
    if (!json || !out || cap == 0) return 0;
    out[0] = '\0';
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 0;
    ++p;
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '"') return 0;
    ++p;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) {
            ++p;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            default:  out[o++] = *p;   break;
            }
            ++p;
            continue;
        }
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0;
}

/* Escape for a JSON string body. Same rules as the lobby client's escaper:
 * quote and backslash escaped, control characters dropped. */
static void json_esc(const char *in, char *out, size_t cap) {
    size_t o = 0;
    if (!out || cap == 0) return;
    while (in && *in && o + 2 < cap) {
        unsigned char c = (unsigned char)*in++;
        if (c == '"' || c == '\\') {
            if (o + 3 >= cap) break;
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static void hex_encode(const unsigned char *in, size_t n, char *out) {
    static const char H[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; ++i) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 15];
    }
    out[n * 2] = '\0';
}

/* HMAC-SHA256, keyed by the hex verifier string, over the nonce. The server
 * computes the identical thing (secrets::expected_proof). */
static void hmac_sha256_hex(const char *key, const char *msg, char out_hex[65]) {
    unsigned char k[64], inner[32], outer[32], pad[64];
    unsigned char tmp[96];
    psx_sha256_ctx c;
    size_t klen = strlen(key), i;

    memset(k, 0, sizeof(k));
    if (klen > 64) {
        psx_sha256_compute((const unsigned char *)key, klen, k);
    } else {
        memcpy(k, key, klen);
    }

    for (i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x36;
    psx_sha256_init(&c);
    psx_sha256_update(&c, pad, 64);
    psx_sha256_update(&c, (const unsigned char *)msg, strlen(msg));
    psx_sha256_final(&c, inner);

    for (i = 0; i < 64; ++i) pad[i] = k[i] ^ 0x5c;
    memcpy(tmp, pad, 64);
    memcpy(tmp + 64, inner, 32);
    psx_sha256_compute(tmp, 96, outer);
    hex_encode(outer, 32, out_hex);
}

/* ---- the secret file ---------------------------------------------------- */

static void secret_path(char *out, size_t cap) {
    /* Beside the other runtime config (keybinds.ini, config.ini), so a build
     * copied to another device carries it the same way. */
    snprintf(out, cap, "netplay_secret");
}

static void secret_load(void) {
    char path[256];
    FILE *f;
    size_t n;
    secret_path(path, sizeof(path));
    g.secret[0] = '\0';
    g.player_id[0] = '\0';
    f = fopen(path, "rb");
    if (!f) return;
    n = fread(g.secret, 1, sizeof(g.secret) - 1, f);
    g.secret[n] = '\0';
    fclose(f);
    /* Format: "<player_id> <secret>\n". Tolerate a trailing newline. */
    {
        char *sp = strchr(g.secret, ' ');
        char *nl;
        if (sp) {
            size_t idn = (size_t)(sp - g.secret);
            if (idn >= sizeof(g.player_id)) idn = sizeof(g.player_id) - 1;
            memcpy(g.player_id, g.secret, idn);
            g.player_id[idn] = '\0';
            memmove(g.secret, sp + 1, strlen(sp + 1) + 1);
        }
        nl = strpbrk(g.secret, "\r\n");
        if (nl) *nl = '\0';
    }
}

static void secret_store(const char *player_id, const char *secret) {
    char path[256];
    FILE *f;
    secret_path(path, sizeof(path));
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "%s %s\n", player_id ? player_id : "", secret ? secret : "");
    fclose(f);
#if !defined(_WIN32)
    /* A password in a file: keep it to the owner. */
    (void)chmod(path, S_IRUSR | S_IWUSR);
#endif
    snprintf(g.secret, sizeof(g.secret), "%s", secret ? secret : "");
    snprintf(g.player_id, sizeof(g.player_id), "%s", player_id ? player_id : "");
}

static void secret_forget(void) {
    char path[256];
    secret_path(path, sizeof(path));
    remove(path);
    g.secret[0] = '\0';
    g.player_id[0] = '\0';
}

/* ---- blocking HTTP POST (worker thread only) ---------------------------- */

/* Returns the HTTP status, or <0 on a transport failure. Body is copied into
 * `body` (truncated to fit), which is enough for these small JSON replies. */
static int http_post(const char *path, const char *json, char *body, size_t cap) {
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16], req[2048];
    char buf[4096];
    int fd = -1, status = -1;
    size_t total = 0;
    const char *hdr_end;

    if (body && cap) body[0] = '\0';
    snprintf(portstr, sizeof(portstr), "%d", g.port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(g.host, portstr, &hints, &res) != 0 || !res) return -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return -2;

    snprintf(req, sizeof(req),
             "POST %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n%s",
             path, g.host, g.port, (int)strlen(json), json);
    if (send(fd, req, (int)strlen(req), 0) < 0) {
        close(fd);
        return -3;
    }

    for (;;) {
        int n = (int)recv(fd, buf + total, (int)(sizeof(buf) - 1 - total), 0);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= sizeof(buf) - 1) break;
    }
    close(fd);
    buf[total] = '\0';
    if (total < 12) return -4;
    status = atoi(buf + 9); /* "HTTP/1.1 NNN ..." */
    hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end && body && cap) {
        snprintf(body, cap, "%s", hdr_end + 4);
    }
    return status;
}

/* ---- one challenge/proof round ------------------------------------------ */

/* Fills `session` and the display fields. Returns 1 on success. The key never
 * leaves this function: only the HMAC does. */
static int prove_and_get_session(void) {
    char body[2048], reqbuf[512], nonce[128], verifier_hex[65], proof[65];
    unsigned char digest[32];
    char id_esc[128];
    int st;

    if (!g.secret[0] || !g.player_id[0]) return 0;

    json_esc(g.player_id, id_esc, sizeof(id_esc));
    snprintf(reqbuf, sizeof(reqbuf), "{\"player_id\":\"%s\"}", id_esc);
    st = http_post("/auth/challenge", reqbuf, body, sizeof(body));
    if (st != 200 || !json_str(body, "nonce", nonce, sizeof(nonce))) return 0;

    psx_sha256_compute((const unsigned char *)g.secret, strlen(g.secret), digest);
    hex_encode(digest, 32, verifier_hex);
    hmac_sha256_hex(verifier_hex, nonce, proof);

    snprintf(reqbuf, sizeof(reqbuf),
             "{\"player_id\":\"%s\",\"nonce\":\"%s\",\"proof\":\"%s\"}",
             id_esc, nonce, proof);
    st = http_post("/auth/session", reqbuf, body, sizeof(body));
    if (st != 200) return 0;
    json_str(body, "session", g.session, sizeof(g.session));
    json_str(body, "handle", g.handle, sizeof(g.handle));
    json_str(body, "discord_username", g.username, sizeof(g.username));
    return g.session[0] ? 1 : 0;
}

/* ---- opening the browser ------------------------------------------------ */

/* The one thing the launcher cannot do for us: hand a URL to the desktop.
 * There is no SDL_OpenURL in this runtime, so this is per platform. A failure
 * is not fatal -- the player can still be told to open the page themselves. */
static void account_open_url(const char *url) {
    if (!url || !url[0]) return;
#if defined(_WIN32)
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    {
        char cmd[1200];
#if defined(__APPLE__)
        const char *opener = "open";
#else
        const char *opener = "xdg-open";
#endif
        /* The URL comes from our own lobby server, but it still goes through a
         * shell, so refuse anything that is not a plain http(s) URL rather
         * than trusting the source. */
        const char *q;
        if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return;
        for (q = url; *q; ++q) {
            if (*q == '\'' || *q == '"' || *q == '`' || *q == '$' || *q == '\\' ||
                *q == ';' || *q == '|' || *q == '&' || *q == '<' || *q == '>' ||
                (unsigned char)*q < 0x20) {
                fprintf(stderr, "psx_account: refusing to open a suspicious URL\n");
                return;
            }
        }
        snprintf(cmd, sizeof(cmd), "%s '%s' >/dev/null 2>&1 &", opener, url);
        if (system(cmd) != 0) {
            fprintf(stderr, "psx_account: could not open a browser; visit:\n  %s\n", url);
        }
    }
#endif
}

/* ---- worker ------------------------------------------------------------- */

static void job_login(void) {
    char body[2048], url[1024];
    int st, tries;

    st = http_post("/auth/discord/start", "{}", body, sizeof(body));
    if (st == 503) {
        g.available = 0;
        set_err("This lobby server does not offer Discord sign-in.");
        g.state = PSX_ACCOUNT_FAILED;
        return;
    }
    if (st != 200 || !json_str(body, "url", url, sizeof(url)) ||
        !json_str(body, "code", g.pair_code, sizeof(g.pair_code))) {
        set_err("Could not reach the lobby server to start the sign-in.");
        g.state = PSX_ACCOUNT_FAILED;
        return;
    }
    g.available = 1;
    account_open_url(url);

    /* Poll until the server has an answer. The pairing code expires server
     * side after ten minutes; stop a little before that rather than spinning
     * forever if the player closed the tab. */
    for (tries = 0; tries < 300; ++tries) {
        char req[256];
        char code_esc[160];
        json_esc(g.pair_code, code_esc, sizeof(code_esc));
        snprintf(req, sizeof(req), "{\"code\":\"%s\"}", code_esc);
        st = http_post("/auth/discord/poll", req, body, sizeof(body));
        if (st == 200) {
            char secret[SECRET_MAX], pid[ID_MAX];
            json_str(body, "session", g.session, sizeof(g.session));
            json_str(body, "handle", g.handle, sizeof(g.handle));
            json_str(body, "discord_username", g.username, sizeof(g.username));
            if (json_str(body, "netplay_secret", secret, sizeof(secret)) &&
                json_str(body, "player_id", pid, sizeof(pid))) {
                /* Returned exactly once. Store it now or it is gone. */
                secret_store(pid, secret);
            }
            g.state = PSX_ACCOUNT_SIGNED_IN;
            return;
        }
        if (st == 202) {
#if defined(_WIN32)
            Sleep(2000);
#else
            struct timespec ts; ts.tv_sec = 2; ts.tv_nsec = 0;
            nanosleep(&ts, NULL);
#endif
            continue;
        }
        set_err("Discord sign-in did not complete. Try again.");
        g.state = PSX_ACCOUNT_FAILED;
        return;
    }
    set_err("Sign-in timed out. Try again.");
    g.state = PSX_ACCOUNT_FAILED;
}

static void job_session(void) {
    if (prove_and_get_session()) {
        g.state = PSX_ACCOUNT_SIGNED_IN;
    } else {
        /* A stored key that no longer works (revoked, or a different server):
         * drop it and fall back to guest rather than nagging every launch. */
        secret_forget();
        g.session[0] = '\0';
        g.state = PSX_ACCOUNT_GUEST;
    }
}

static void job_set_handle(void) {
    char body[1024], req[768], esc[160], id_esc[128], nonce[128];
    char verifier_hex[65], proof[65];
    unsigned char digest[32];
    int st;

    if (!g.secret[0] || !g.player_id[0]) {
        set_err("Sign in before changing your name.");
        return;
    }
    json_esc(g.player_id, id_esc, sizeof(id_esc));
    snprintf(req, sizeof(req), "{\"player_id\":\"%s\"}", id_esc);
    if (http_post("/auth/challenge", req, body, sizeof(body)) != 200 ||
        !json_str(body, "nonce", nonce, sizeof(nonce))) {
        set_err("Could not reach the lobby server.");
        return;
    }
    psx_sha256_compute((const unsigned char *)g.secret, strlen(g.secret), digest);
    hex_encode(digest, 32, verifier_hex);
    hmac_sha256_hex(verifier_hex, nonce, proof);

    json_esc(g.pending_handle, esc, sizeof(esc));
    snprintf(req, sizeof(req),
             "{\"player_id\":\"%s\",\"nonce\":\"%s\",\"proof\":\"%s\",\"handle\":\"%s\"}",
             id_esc, nonce, proof, esc);
    st = http_post("/auth/handle", req, body, sizeof(body));
    if (st == 200) {
        json_str(body, "handle", g.handle, sizeof(g.handle));
    } else {
        set_err("That name can't be used. Please pick another one.");
    }
}

static void job_sign_out(void) {
    char body[512], req[512], id_esc[128], nonce[128], verifier_hex[65], proof[65];
    unsigned char digest[32];

    /* Best effort: tell the server to retire this device's key, then forget it
     * locally whatever happened. Signing out must work offline. */
    if (g.secret[0] && g.player_id[0]) {
        json_esc(g.player_id, id_esc, sizeof(id_esc));
        snprintf(req, sizeof(req), "{\"player_id\":\"%s\"}", id_esc);
        if (http_post("/auth/challenge", req, body, sizeof(body)) == 200 &&
            json_str(body, "nonce", nonce, sizeof(nonce))) {
            psx_sha256_compute((const unsigned char *)g.secret, strlen(g.secret), digest);
            hex_encode(digest, 32, verifier_hex);
            hmac_sha256_hex(verifier_hex, nonce, proof);
            snprintf(req, sizeof(req),
                     "{\"player_id\":\"%s\",\"nonce\":\"%s\",\"proof\":\"%s\"}",
                     id_esc, nonce, proof);
            (void)http_post("/auth/secret/revoke", req, body, sizeof(body));
        }
    }
    secret_forget();
    g.session[0] = '\0';
    g.handle[0] = '\0';
    g.username[0] = '\0';
    g.state = PSX_ACCOUNT_GUEST;
}

#if defined(_WIN32)
static unsigned __stdcall auth_worker(void *arg) {
#else
static void *auth_worker(void *arg) {
#endif
    int job = (int)(intptr_t)arg;
    switch (job) {
    case JOB_LOGIN:      job_login();      break;
    case JOB_SESSION:    job_session();    break;
    case JOB_SET_HANDLE: job_set_handle(); break;
    case JOB_SIGN_OUT:   job_sign_out();   break;
    default: break;
    }
    g.busy = 0;
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int start_job(int job) {
    if (g.busy) return -1;
    g.busy = 1;
    g.job = job;
#if defined(_WIN32)
    {
        uintptr_t th = _beginthreadex(NULL, 0, auth_worker, (void *)(intptr_t)job, 0, NULL);
        if (!th) { g.busy = 0; return -1; }
        g.thread = (HANDLE)th;
    }
#else
    if (pthread_create(&g.thread, NULL, auth_worker, (void *)(intptr_t)job) != 0) {
        g.busy = 0;
        return -1;
    }
    pthread_detach(g.thread);
#endif
    g.thread_valid = 1;
    return 0;
}

/* ---- public surface ----------------------------------------------------- */

void psx_account_init(const char *ws_url) {
    const char *p = ws_url;
    const char *slash;
    const char *colon;
    char hostport[192];

    memset(g.host, 0, sizeof(g.host));
    g.port = 8765;
    if (p) {
        if (strncmp(p, "ws://", 5) == 0) p += 5;
        else if (strncmp(p, "wss://", 6) == 0) p += 6;
        slash = strchr(p, '/');
        snprintf(hostport, sizeof(hostport), "%.*s",
                 slash ? (int)(slash - p) : (int)strlen(p), p);
        colon = strrchr(hostport, ':');
        if (colon && strchr(colon, ']') == NULL) {
            g.port = atoi(colon + 1);
            *(char *)colon = '\0';
        }
        snprintf(g.host, sizeof(g.host), "%s", hostport);
    }
    if (!g.initialized) {
        g.available = -1;
        g.state = PSX_ACCOUNT_GUEST;
        secret_load();
        g.initialized = 1;
    }
}

void psx_account_pump(void) {
    if (!g.initialized || g.busy) return;
    /* A stored key is redeemed once, lazily, on the first pump: the launcher
     * should come up signed in without the player doing anything. */
    if (!g.tried_stored_key && g.secret[0] && g.player_id[0]) {
        g.tried_stored_key = 1;
        g.state = PSX_ACCOUNT_WAITING;
        (void)start_job(JOB_SESSION);
    }
}

int psx_account_available(void) {
    /* Unknown until something asks the server. Offering the button is right:
     * the first start() settles it, and a 503 turns it off for good. */
    return g.available != 0;
}

int psx_account_login_begin(void) {
    if (g.busy) return -1;
    g.error[0] = '\0';
    g.state = PSX_ACCOUNT_WAITING;
    return start_job(JOB_LOGIN);
}

int psx_account_state(void) { return g.state; }
const char *psx_account_handle(void) { return g.handle; }
const char *psx_account_username(void) { return g.username; }
const char *psx_account_error(void) { return g.error; }
const char *psx_account_session(void) { return g.session; }

int psx_account_sign_out(void) {
    if (g.busy) return -1;
    return start_job(JOB_SIGN_OUT);
}

int psx_account_set_handle(const char *handle) {
    if (g.busy || !handle || !handle[0]) return -1;
    snprintf(g.pending_handle, sizeof(g.pending_handle), "%s", handle);
    g.error[0] = '\0';
    if (start_job(JOB_SET_HANDLE) != 0) return -1;
    /* The launcher wants a synchronous verdict for its modal, and the request
     * is one round trip on a LAN-ish connection: wait briefly rather than
     * inventing an async path through the UI for a rename. */
    {
        int spins = 0;
        while (g.busy && spins++ < 400) {
#if defined(_WIN32)
            Sleep(10);
#else
            struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 10 * 1000 * 1000;
            nanosleep(&ts, NULL);
#endif
        }
    }
    return g.error[0] ? -1 : 0;
}
