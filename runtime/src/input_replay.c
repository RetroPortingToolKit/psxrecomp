#include "input_replay.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

typedef struct ReplayStep { uint64_t end; unsigned buttons; } ReplayStep;
static ReplayStep *s_steps;
static size_t s_count, s_index;
static uint64_t s_base;
static int s_armed;

int input_replay_load(const char *path) {
    FILE *f = fopen(path, "r");
    ReplayStep *steps = NULL;
    size_t count = 0;
    uint64_t end = 0;
    char line[256];
    int ok = f != NULL;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        unsigned long long frames, buttons;
        char *endp;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        if (*p == '-' || *p == '+') { ok = 0; break; }
        errno = 0; frames = strtoull(p, &endp, 10);
        if (endp == p || errno || !isspace((unsigned char)*endp) ||
            !frames || frames > UINT32_MAX) { ok = 0; break; }
        p = endp;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '-' || *p == '+') { ok = 0; break; }
        errno = 0; buttons = strtoull(p, &endp, 16);
        if (endp == p || errno || buttons > 65535) { ok = 0; break; }
        while (isspace((unsigned char)*endp)) endp++;
        if (*endp || count >= 65536 || end + frames > UINT32_MAX) { ok = 0; break; }
        ReplayStep *next = realloc(steps, (count+1)*sizeof *steps);
        if (!next) { ok = 0; break; }
        steps = next;
        end += frames;
        steps[count++] = (ReplayStep){end, buttons};
    }
    if (ferror(f)) ok = 0;
    fclose(f);
    if (!ok || !count) { free(steps); return 0; }
    free(s_steps);
    s_steps = steps; s_count = count; s_index = 0; s_armed = 0;
    return 1;
}

void input_replay_on_load(uint64_t next_frame) {
    s_base = next_frame; s_index = 0; s_armed = s_count != 0;
}

uint64_t input_replay_frame(uint64_t frame) {
    return s_armed && frame >= s_base ? frame-s_base : UINT64_MAX;
}

int input_replay_buttons(uint64_t frame) {
    uint64_t at = input_replay_frame(frame);
    if (at == UINT64_MAX) return -1;
    if (s_index && at < s_steps[s_index-1].end) s_index = 0;
    while (s_index < s_count && at >= s_steps[s_index].end) s_index++;
    return s_index < s_count ? (int)s_steps[s_index].buttons : 65535;
}
