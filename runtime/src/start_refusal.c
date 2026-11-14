/* start_refusal.c — see start_refusal.h. */
#include "start_refusal.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static char s_kind[PSX_START_REFUSAL_KIND_CAP];
static char s_title[PSX_START_REFUSAL_TITLE_CAP];
static char s_message[PSX_START_REFUSAL_TEXT_CAP];
static char s_row_bios[PSX_START_LAUNCHER_ROW_CAP];
static char s_row_disc[PSX_START_LAUNCHER_ROW_CAP];
static char s_disc_warning[PSX_START_REFUSAL_TEXT_CAP];

static int is_separator(char c) { return c == '/' || c == '\\'; }

/* Where a full path begins at s[i], if one does. `line` is the start of the
 * line. Three shapes: a drive ("C:\" or "C:/"), a share ("\\server"), and a
 * rooted path of two parts or more ("/home/me/x"). A relative path, a URL and
 * a lone slash are not full paths and stay as they are. */
static int path_starts_at(const char *line, const char *p, const char *eol) {
    const int at_word = (p == line) || p[-1] == ' ' || p[-1] == '"' || p[-1] == '\'' ||
                        p[-1] == '(' || p[-1] == '\t' || p[-1] == '=';
    if (isalpha((unsigned char)p[0]) && p + 2 < eol && p[1] == ':' && is_separator(p[2]) &&
        (p == line || !isalnum((unsigned char)p[-1])))
        return 1;
    if (p[0] == '\\' && p + 1 < eol && p[1] == '\\' && (p == line || p[-1] != '\\'))
        return 1;
    if (p[0] == '/' && at_word && p + 1 < eol && p[1] != ' ' && p[1] != '/' &&
        memchr(p + 1, '/', (size_t)(eol - p - 1)) != NULL)
        return 1;
    return 0;
}

/* The report names a file by its base name, never by its folder: players
 * paste this file into a chat. Only the box, which the player alone sees,
 * shows a full path. A full path runs from where it begins to the end of its
 * line (paths hold spaces); what follows its last separator is kept. */
void psx_start_refusal_without_folders(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    if (!dst || cap == 0) return;
    if (!src) src = "";
    while (*src && o + 1 < cap) {
        const char *eol = strchr(src, '\n');
        const char *p;
        const char *copy_to;
        const char *base = NULL;
        if (!eol) eol = src + strlen(src);
        copy_to = eol;
        for (p = src; p < eol; p++) {
            if (path_starts_at(src, p, eol)) {
                const char *q;
                copy_to = p;
                base = p;
                for (q = p; q < eol; q++)
                    if (is_separator(*q)) base = q + 1;
                break;
            }
        }
        for (p = src; p < copy_to && o + 1 < cap; p++) dst[o++] = *p;
        if (base)
            for (p = base; p < eol && o + 1 < cap; p++) dst[o++] = *p;
        if (*eol == '\n' && o + 1 < cap) dst[o++] = '\n';
        src = (*eol == '\n') ? eol + 1 : eol;
    }
    dst[o] = '\0';
}

void psx_start_refusal_set(const char *kind, const char *title, const char *message) {
    if (!kind || !kind[0] || s_kind[0]) return;
    snprintf(s_kind, sizeof(s_kind), "%s", kind);
    psx_start_refusal_without_folders(s_title, sizeof(s_title), title);
    psx_start_refusal_without_folders(s_message, sizeof(s_message), message);
}

const char *psx_start_refusal_kind(void) {
    return s_kind[0] ? s_kind : NULL;
}

void psx_start_note_launcher(const char *row, const char *text) {
    if (!row) return;
    if (strcmp(row, "bios") == 0)
        psx_start_refusal_without_folders(s_row_bios, sizeof(s_row_bios), text);
    else if (strcmp(row, "disc") == 0)
        psx_start_refusal_without_folders(s_row_disc, sizeof(s_row_disc), text);
}

void psx_start_refusal_reset(void) {
    s_kind[0] = s_title[0] = s_message[0] = '\0';
    s_row_bios[0] = s_row_disc[0] = '\0';
}

void psx_disc_warning_set(const char *message) {
    psx_start_refusal_without_folders(s_disc_warning, sizeof(s_disc_warning), message);
}

/* Length of the well-formed UTF-8 sequence at s, or 0 when the bytes there
 * are not one (a lone continuation byte, a cut sequence, an overlong form, a
 * surrogate, a value past U+10FFFF). */
static int utf8_sequence(const unsigned char *s) {
    if (s[0] >= 0xC2 && s[0] <= 0xDF)
        return (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xE0 && s[1] < 0xA0) return 0;
        if (s[0] == 0xED && s[1] > 0x9F) return 0;
        return 3;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xF0 && s[1] < 0x90) return 0;
        if (s[0] == 0xF4 && s[1] > 0x8F) return 0;
        return 4;
    }
    return 0;
}

/* Append `s` as a JSON string. Returns 0 when it does not fit. A line break
 * becomes \n and any other control character a space. Text that is UTF-8
 * stays as it is. A byte that is not part of a UTF-8 sequence is written as
 * \u00XX: a Windows file name in the system code page would otherwise make
 * the whole report unreadable to a strict JSON reader. */
static int put_json_string(char *out, size_t cap, size_t *pos, const char *s) {
    const unsigned char *u = (const unsigned char *)s;
    size_t p = *pos;
    if (p + 1 >= cap) return 0;
    out[p++] = '"';
    while (*u) {
        unsigned char c = *u;
        if (c == '\r') { u++; continue; }
        if (c == '"' || c == '\\' || c == '\n') {
            if (p + 2 >= cap) return 0;
            out[p++] = '\\';
            out[p++] = (char)(c == '\n' ? 'n' : c);
            u++;
        } else if (c < 0x80) {
            if (p + 1 >= cap) return 0;
            out[p++] = (char)(c < 0x20 ? ' ' : c);
            u++;
        } else {
            int n = utf8_sequence(u);
            if (n) {
                if (p + (size_t)n >= cap) return 0;
                memcpy(out + p, u, (size_t)n);
                p += (size_t)n;
                u += n;
            } else {
                if (p + 6 >= cap) return 0;
                p += (size_t)snprintf(out + p, cap - p, "\\u%04x", (unsigned)c);
                u++;
            }
        }
    }
    if (p + 1 >= cap) return 0;
    out[p++] = '"';
    out[p] = '\0';
    *pos = p;
    return 1;
}

static int put_text(char *out, size_t cap, size_t *pos, const char *s) {
    size_t n = strlen(s);
    if (*pos + n >= cap) return 0;
    memcpy(out + *pos, s, n + 1);
    *pos += n;
    return 1;
}

static int put_null(char *out, size_t cap) {
    if (!out || cap == 0) return 0;
    return snprintf(out, cap, "%s", cap > 4 ? "null" : "");
}

int psx_start_refusal_json(char *out, size_t cap) {
    size_t pos = 0;
    if (!out || cap == 0) return 0;
    if (!s_kind[0]) return put_null(out, cap);
    if (put_text(out, cap, &pos, "{\"kind\": ") &&
        put_json_string(out, cap, &pos, s_kind) &&
        put_text(out, cap, &pos, ", \"title\": ") &&
        put_json_string(out, cap, &pos, s_title) &&
        put_text(out, cap, &pos, ", \"message\": ") &&
        put_json_string(out, cap, &pos, s_message) &&
        put_text(out, cap, &pos, "}"))
        return (int)pos;
    return put_null(out, cap);
}

int psx_start_launcher_status_json(char *out, size_t cap) {
    size_t pos = 0;
    if (!out || cap == 0) return 0;
    if (!s_row_bios[0] && !s_row_disc[0]) return put_null(out, cap);
    if (put_text(out, cap, &pos, "{\"bios\": ") &&
        put_json_string(out, cap, &pos, s_row_bios) &&
        put_text(out, cap, &pos, ", \"disc\": ") &&
        put_json_string(out, cap, &pos, s_row_disc) &&
        put_text(out, cap, &pos, "}"))
        return (int)pos;
    return put_null(out, cap);
}

int psx_disc_warning_json(char *out, size_t cap) {
    size_t pos = 0;
    if (!out || cap == 0) return 0;
    if (!s_disc_warning[0]) return put_null(out, cap);
    if (put_json_string(out, cap, &pos, s_disc_warning)) return (int)pos;
    return put_null(out, cap);
}
