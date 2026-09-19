#include "debug_trace_ranges.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>

static const char *skip_space(const char *p)
{
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static int parse_u32(const char **cursor, uint32_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    const char *start = skip_space(*cursor);

    if (!*start || *start == '-') {
        return 0;
    }

    errno = 0;
    parsed = strtoul(start, &end, 0);
    if (end == start || errno == ERANGE || parsed > UINT32_MAX) {
        return 0;
    }

    *cursor = end;
    *value = (uint32_t)parsed;
    return 1;
}

int psx_debug_parse_trace_ranges(const char *spec,
                                 PSXDebugTraceRange *ranges,
                                 int capacity)
{
    const char *cursor;
    int count = 0;

    if (!spec || !ranges || capacity < 0) {
        return -1;
    }

    cursor = skip_space(spec);
    if (!*cursor) {
        return 0;
    }

    for (;;) {
        uint32_t lo;
        uint32_t hi;

        if (!parse_u32(&cursor, &lo)) {
            return -1;
        }
        cursor = skip_space(cursor);
        if (*cursor++ != ',') {
            return -1;
        }
        if (!parse_u32(&cursor, &hi)) {
            return -1;
        }

        lo &= 0x1FFFFFFFu;
        hi &= 0x1FFFFFFFu;
        if (hi <= lo) {
            return -1;
        }
        if (count >= capacity) {
            return -2;
        }

        ranges[count].lo = lo;
        ranges[count].hi = hi;
        ++count;

        cursor = skip_space(cursor);
        if (!*cursor) {
            return count;
        }
        if (*cursor++ != ';') {
            return -1;
        }
        cursor = skip_space(cursor);
        if (!*cursor) {
            return -1;
        }
    }
}
