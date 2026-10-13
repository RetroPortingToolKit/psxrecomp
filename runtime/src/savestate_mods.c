/* savestate_mods.c - compare a savestate's recorded mod set with the current
 * one (savestate.h). Kept apart from savestate.c so it tests standalone. */
#include "savestate.h"

#include <stdio.h>
#include <string.h>

int savestate_mods_record_differs(const char* record, const char* current) {
    char a[160];
    size_t n;
    if (!record) return 0;  /* no record: a state from before the record existed */
    snprintf(a, sizeof(a), "%s", record);
    n = strlen(a);
    while (n && (a[n - 1] == '\n' || a[n - 1] == '\r' || a[n - 1] == ' ')) a[--n] = '\0';
    return strcmp(a, current ? current : "") != 0;
}
