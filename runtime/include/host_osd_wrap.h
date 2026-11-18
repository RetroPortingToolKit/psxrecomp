/* host_osd_wrap.h - line breaking for the host toast.
 *
 * The toast is drawn in an 8x8 font at 2x, 16 pixels a character at the
 * smallest scale, 8 pixels in from the left edge. The narrowest window the
 * window-scale setting offers is 640 pixels wide, and at every 4:3 scale it
 * offers the toast scales with the window, so 39 characters is what fits.
 * A line is HOST_OSD_COLS characters, one less, and a message is at most
 * HOST_OSD_LINES lines. A longer message loses its end and shows "...".
 *
 * Breaking: a message that fits on one line stays on one line. Otherwise a
 * "Prefix: reason" message puts the prefix on its own line when the reason
 * then fits in the lines that are left; any other message breaks at spaces.
 * A word longer than a line is split.
 *
 * Kept free of SDL and of the OSD state so the breaking is unit-tested
 * directly (test_host_osd_wrap.c). */
#ifndef PSX_HOST_OSD_WRAP_H
#define PSX_HOST_OSD_WRAP_H

#include <string.h>

#define HOST_OSD_COLS   38
#define HOST_OSD_LINES  3
/* Every character a message can show, plus the terminator. */
#define HOST_OSD_MSG_BYTES (HOST_OSD_COLS * HOST_OSD_LINES + HOST_OSD_LINES + 1)

/* Greedy fill of lines[first..] from text. Returns the number of lines used;
 * *left is 1 when text remained after the last line. */
static inline int host_osd_wrap_fill(const char *text, char lines[][HOST_OSD_COLS + 1],
                                     int first, int *left)
{
    int n = first;
    const char *p = text;
    *left = 0;
    while (*p == ' ') ++p;
    while (*p) {
        if (n == HOST_OSD_LINES) { *left = 1; break; }
        int len = 0;
        while (*p) {
            const char *word = p;
            int w = 0;
            while (word[w] && word[w] != ' ') ++w;
            if (len && len + 1 + w > HOST_OSD_COLS) break;      /* next line */
            if (!len && w > HOST_OSD_COLS) {                     /* split a long word */
                memcpy(lines[n], word, HOST_OSD_COLS);
                len = HOST_OSD_COLS;
                p = word + HOST_OSD_COLS;
                break;
            }
            if (len) lines[n][len++] = ' ';
            memcpy(lines[n] + len, word, (size_t)w);
            len += w;
            p = word + w;
            while (*p == ' ') ++p;
        }
        lines[n][len] = 0;
        ++n;
    }
    return n;
}

/* Breaks msg into lines. Returns the line count (0 for an empty message);
 * *truncated is 1 when the message did not fit and its end was dropped. */
static inline int host_osd_wrap(const char *msg, char lines[HOST_OSD_LINES][HOST_OSD_COLS + 1],
                                int *truncated)
{
    int left = 0, n;
    const size_t total = msg ? strlen(msg) : 0;
    if (truncated) *truncated = 0;
    for (int i = 0; i < HOST_OSD_LINES; ++i) lines[i][0] = 0;
    if (!total) return 0;
    if (total <= HOST_OSD_COLS) { memcpy(lines[0], msg, total + 1); return 1; }
    /* "Prefix: reason": the prefix alone on the first line, when that keeps
     * the whole reason. */
    const char *colon = strstr(msg, ": ");
    if (colon && colon - msg < HOST_OSD_COLS && colon[2]) {
        const size_t head = (size_t)(colon - msg) + 1;
        memcpy(lines[0], msg, head);
        lines[0][head] = 0;
        n = host_osd_wrap_fill(colon + 2, lines, 1, &left);
        if (!left) return n;
    }
    n = host_osd_wrap_fill(msg, lines, 0, &left);
    if (left) {
        char *last = lines[HOST_OSD_LINES - 1];
        size_t len = strlen(last);
        if (len > HOST_OSD_COLS - 3) len = HOST_OSD_COLS - 3;
        memcpy(last + len, "...", 4);
        if (truncated) *truncated = 1;
    }
    return n;
}

#endif /* PSX_HOST_OSD_WRAP_H */
