/* The build and game checks for LAN / Direct IP rooms. See
 * netplay_lan_version.h. */
#include "netplay_lan_version.h"

#include <string.h>

/* Copy line `index` of a JOIN tail. The line comes from the network and is
 * logged, so control characters become '?'. keep_high lets bytes above 0x7f
 * through, for a UTF-8 game name; a version never holds one. */
static void join_line(const char *tail, int index, int keep_high, char *out, size_t out_cap)
{
    const char *p = tail;
    size_t n = 0;
    int i;

    if (!out || !out_cap) return;
    out[0] = '\0';
    if (!p) return;
    for (i = 0; i < index; ++i) {
        const char *nl = strchr(p, '\n');
        if (!nl) return;
        p = nl + 1;
    }
    while (p[n] && p[n] != '\n' && p[n] != '\r' && n + 1 < out_cap) {
        const unsigned char c = (unsigned char)p[n];
        out[n] = ((c >= 0x20 && c <= 0x7e) || (keep_high && c > 0x7f)) ? (char)c : '?';
        ++n;
    }
    out[n] = '\0';
}

void netplay_lan_join_version(const char *tail, char *out, size_t out_cap)
{
    /* Printable ASCII only, so '?' cannot match a version. */
    join_line(tail, NETPLAY_LAN_VERSION_LINE, 0, out, out_cap);
}

void netplay_lan_join_game(const char *tail, char *out, size_t out_cap)
{
    join_line(tail, NETPLAY_LAN_GAME_LINE, 1, out, out_cap);
}

/* The text without its surrounding blanks; *len is 0 when nothing is left. */
static const char *trimmed(const char *text, size_t *len)
{
    const char *end;

    if (!text) text = "";
    while (*text == ' ' || *text == '\t') ++text;
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n'))
        --end;
    *len = (size_t)(end - text);
    return text;
}

/* Blank or missing means "dev", as recomp-net-server normalises it. */
static const char *normalised(const char *version, size_t *len)
{
    const char *text = trimmed(version, len);

    if (*len == 0) {
        *len = 3;
        return "dev";
    }
    return text;
}

int netplay_lan_version_ok(const char *host_version, const char *guest_version)
{
    size_t host_len, guest_len;
    const char *host = normalised(host_version, &host_len);
    const char *guest = normalised(guest_version, &guest_len);

    return host_len == guest_len && memcmp(host, guest, host_len) == 0;
}

int netplay_lan_game_ok(const char *host_game, const char *guest_game)
{
    size_t host_len, guest_len;
    const char *host = trimmed(host_game, &host_len);
    const char *guest = trimmed(guest_game, &guest_len);

    return host_len != 0 && host_len == guest_len && memcmp(host, guest, host_len) == 0;
}
