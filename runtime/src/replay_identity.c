/* Player replay identity. See replay_identity.h. */
#include "replay_identity.h"
#include "psx_sha256.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PSX_FRAMEWORK_COMMIT
#define PSX_FRAMEWORK_COMMIT ""
#endif
#define PATH_BYTES 4096u

static char s_serial[REPLAY_FILE_TEXT], s_disc[PATH_BYTES], s_bios_stem[REPLAY_FILE_TEXT];
static int s_digest_cached;
static uint32_t s_digest_kind;
static uint8_t s_digest[32];

static void hex(const uint8_t *bytes, unsigned n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * n] = 0;
}

static void copy_text(char *dst, size_t size, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= size) n = size - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

void replay_identity_set_product(const char *disc_serial, const char *disc_path, const char *bios_path)
{
    const char *base = bios_path ? bios_path : "", *dot;
    size_t n;
    copy_text(s_serial, sizeof s_serial, disc_serial);
    copy_text(s_disc, sizeof s_disc, disc_path);
    s_digest_cached = 0;
    for (const char *p = base; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    dot = strrchr(base, '.');
    n = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof s_bios_stem) n = sizeof s_bios_stem - 1;
    memcpy(s_bios_stem, base, n);
    s_bios_stem[n] = 0;
}

static int file_sha256(const char *path, uint8_t out[32])
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    size_t n;
    int failed;
    psx_sha256_ctx ctx;
    if (!f) return 0;
    if (!(buf = (unsigned char *)malloc(1u << 16))) { fclose(f); return 0; }
    psx_sha256_init(&ctx);
    while ((n = fread(buf, 1, 1u << 16, f)) > 0) psx_sha256_update(&ctx, buf, n);
    failed = ferror(f);
    free(buf);
    fclose(f);
    if (failed) return 0;
    psx_sha256_final(&ctx, out);
    return 1;
}

static int ends_with_ci(const char *text, const char *suffix)
{
    size_t a = strlen(text), b = strlen(suffix);
    if (a < b) return 0;
    for (size_t i = 0; i < b; ++i)
        if (tolower((unsigned char)text[a - b + i]) != tolower((unsigned char)suffix[i])) return 0;
    return 1;
}

static int word_ci(const char *p, const char *word)
{
    for (; *word; ++p, ++word)
        if (toupper((unsigned char)*p) != *word) return 0;
    return 1;
}

/* One cue line: 0 when not a FILE line, 1 with `name` for a quoted BINARY
 * FILE line, -1 for any other FILE line. */
static int cue_file_line(const char *line, char *name, size_t size)
{
    const char *p = line, *start;
    while (isspace((unsigned char)*p)) ++p;
    if (!word_ci(p, "FILE") || isalnum((unsigned char)p[4]) || p[4] == '_') return 0;
    p += 4;
    if (!isspace((unsigned char)*p)) return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '"') return -1;
    start = p;
    while (*p && *p != '"' && *p != '\r' && *p != '\n') ++p;
    if (*p != '"' || p == start || (size_t)(p - start) >= size) return -1;
    memcpy(name, start, (size_t)(p - start));
    name[p - start] = 0;
    ++p;
    if (!isspace((unsigned char)*p)) return -1;
    while (isspace((unsigned char)*p)) ++p;
    if (!word_ci(p, "BINARY")) return -1;
    p += 6;
    while (isspace((unsigned char)*p)) ++p;
    return *p ? -1 : 1;
}

const char *replay_identity_disc_digest(const char *path, uint32_t *kind, uint8_t out[32])
{
    static char dir[PATH_BYTES], name[PATH_BYTES], track[PATH_BYTES];
    uint8_t digest[32];
    char text[65];
    const char *error = NULL;
    size_t dir_len = 0;
    int tracks = 0;
    psx_sha256_ctx ctx;
    FILE *f;
    char *cue, *line;
    long size;
    if (!path || !path[0]) return "no disc is mounted";
    if (!file_sha256(path, digest)) return "cannot read the disc image";
    if (!ends_with_ci(path, ".cue")) {
        *kind = REPLAY_DISC_DIGEST_FILE;
        memcpy(out, digest, 32);
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) < 0 || size > (1L << 20) || fseek(f, 0, SEEK_SET)) {
        if (f) fclose(f);
        return "cannot read the cue sheet";
    }
    cue = (char *)malloc((size_t)size + 1);
    if (!cue || fread(cue, 1, (size_t)size, f) != (size_t)size) {
        free(cue);
        fclose(f);
        return "cannot read the cue sheet";
    }
    fclose(f);
    cue[size] = 0;
    for (size_t i = 0; path[i]; ++i)
        if (path[i] == '/' || path[i] == '\\') dir_len = i + 1;
    if (dir_len >= sizeof dir) { free(cue); return "cue track path too long"; }
    memcpy(dir, path, dir_len);
    psx_sha256_init(&ctx);
    hex(digest, 32, text);
    psx_sha256_update(&ctx, (const uint8_t *)text, 64);
    line = cue;
    if (size >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
        (unsigned char)line[2] == 0xBF) line += 3;
    while (!error && line && *line) {
        char *next = strpbrk(line, "\r\n");
        int kind_line;
        if (next) *next++ = 0;
        kind_line = cue_file_line(line, name, sizeof name);
        if (kind_line < 0) error = "cue FILE line is not a quoted BINARY track";
        else if (kind_line > 0) {
            if (dir_len + strlen(name) >= sizeof track) error = "cue track path too long";
            else {
                memcpy(track, dir, dir_len);
                strcpy(track + dir_len, name);
                if (!file_sha256(track, digest)) error = "cannot read a cue track";
                else {
                    hex(digest, 32, text);
                    psx_sha256_update(&ctx, (const uint8_t *)"\n", 1);
                    psx_sha256_update(&ctx, (const uint8_t *)text, 64);
                    ++tracks;
                }
            }
        }
        line = next;
    }
    free(cue);
    if (error) return error;
    if (!tracks) return "cue sheet has no tracks";
    psx_sha256_final(&ctx, out);
    *kind = REPLAY_DISC_DIGEST_CUE;
    return NULL;
}

const char *replay_identity_boot_mode(int call_hle, int boot_skip)
{
    if (call_hle && boot_skip) return "hle";
    if (call_hle) return "hle-calls";
    if (boot_skip) return "hle-boot";
    return "lle";
}

static int valid_commit(const char *c)
{
    if (strlen(c) != 40) return 0;
    for (unsigned i = 0; i < 40; ++i)
        if (!((c[i] >= '0' && c[i] <= '9') || (c[i] >= 'a' && c[i] <= 'f'))) return 0;
    return 1;
}

int replay_identity_current(ReplayIdentity *id, int call_hle, int boot_skip, char *why, size_t why_size)
{
    const char *commit = PSX_FRAMEWORK_COMMIT;
    const char *error = NULL;
    if (!s_serial[0] || !s_bios_stem[0]) error = "a disc with a boot serial and a BIOS file are required";
    else if (!s_digest_cached) {
        error = replay_identity_disc_digest(s_disc, &s_digest_kind, s_digest);
        s_digest_cached = !error;
    }
    if (error) {
        snprintf(why, why_size, "%s", error);
        return 0;
    }
    memset(id, 0, sizeof *id);
    /* A build outside a git checkout has no commit: a placeholder that never
     * matches, so playback warns. */
    copy_text(id->build, sizeof id->build,
              valid_commit(commit) ? commit : "0000000000000000000000000000000000000000");
    copy_text(id->disc_serial, sizeof id->disc_serial, s_serial);
    copy_text(id->bios_stem, sizeof id->bios_stem, s_bios_stem);
    copy_text(id->boot_mode, sizeof id->boot_mode, replay_identity_boot_mode(call_hle, boot_skip));
    id->disc_digest_kind = s_digest_kind;
    memcpy(id->disc_digest, s_digest, 32);
    return 1;
}

void replay_identity_line_value(const char *lines, const char *key, char *out, size_t cap)
{
    const size_t k = strlen(key);
    out[0] = 0;
    for (const char *p = lines; p && *p;) {
        const char *end = strchr(p, '\n');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n > k && !strncmp(p, key, k) && p[k] == '=') {
            snprintf(out, cap, "%.*s", (int)(n - k - 1), p + k + 1);
            return;
        }
        p = end ? end + 1 : p + n;
    }
}

static int same_text_ci(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

ReplayIdentityMatch replay_identity_check(const ReplayIdentity *rec, const char *rec_product,
                                          const ReplayIdentity *player, const char *player_product,
                                          char *why, size_t why_size)
{
    char rec_crc[64], player_crc[64], rec_exe[80], player_exe[80];
    const char *error = NULL;
    replay_identity_line_value(rec_product, "bios_crc32", rec_crc, sizeof rec_crc);
    replay_identity_line_value(player_product, "bios_crc32", player_crc, sizeof player_crc);
    replay_identity_line_value(rec_product, "exe_sha256", rec_exe, sizeof rec_exe);
    replay_identity_line_value(player_product, "exe_sha256", player_exe, sizeof player_exe);
    if (strcmp(rec->disc_serial, player->disc_serial) || rec->disc_digest_kind != player->disc_digest_kind ||
        memcmp(rec->disc_digest, player->disc_digest, 32))
        error = "it was recorded on a different game or disc image";
    else if (rec_crc[0] && player_crc[0] ? !same_text_ci(rec_crc, player_crc)
                                         : !same_text_ci(rec->bios_stem, player->bios_stem))
        error = "it was recorded with a different BIOS";
    else if (strcmp(rec->boot_mode, player->boot_mode))
        error = "it was recorded with a different boot mode";
    if (error) {
        if (why && why_size) snprintf(why, why_size, "%s", error);
        return REPLAY_IDENTITY_REFUSE;
    }
    if (why && why_size) why[0] = 0;
    if (strcmp(rec->build, player->build) || (rec_exe[0] && player_exe[0] && strcmp(rec_exe, player_exe)))
        return REPLAY_IDENTITY_OTHER_BUILD;
    return REPLAY_IDENTITY_SAME_BUILD;
}
