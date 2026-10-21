/* Windows compiler scratch is private to the child, never the game process. */
#ifndef PSX_AUTOCOMPILE_CHILD_ENV_H
#define PSX_AUTOCOMPILE_CHILD_ENV_H
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int ac_env_compare(const void *a, const void *b) {
    return _stricmp(*(const char *const *)a, *(const char *const *)b);
}
/* cache is the canonical existing cache directory. Caller frees the returned
 * sorted ANSI environment block after CreateProcessA. NULL is a hard failure;
 * do not silently inherit a system-drive TEMP after a scratch setup failure. */
static char *ac_child_environment(const char *cache) {
    char temp_dir[sizeof(s_cache_dir) + 5];
    char temp_var[sizeof(temp_dir) + 6], tmp_var[sizeof(temp_dir) + 5];
    int n = snprintf(temp_dir, sizeof(temp_dir), "%s\\tmp", cache);
    if (n < 0 || (size_t)n >= sizeof(temp_dir)) return NULL;
    if (!CreateDirectoryA(temp_dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return NULL;
    DWORD attr = GetFileAttributesA(temp_dir);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return NULL;
    snprintf(temp_var, sizeof(temp_var), "TEMP=%s", temp_dir);
    snprintf(tmp_var, sizeof(tmp_var), "TMP=%s", temp_dir);
    char *inherited = GetEnvironmentStringsA();
    if (!inherited) return NULL;
    size_t count = 0;
    for (char *p = inherited; *p; p += strlen(p) + 1) ++count;
    const char **entries = (const char **)malloc((count + 2) * sizeof(*entries));
    if (!entries) { FreeEnvironmentStringsA(inherited); return NULL; }
    size_t used = 0, bytes = 1;
    for (char *p = inherited; *p; p += strlen(p) + 1) {
        if (!_strnicmp(p, "TEMP=", 5) || !_strnicmp(p, "TMP=", 4)) continue;
        entries[used++] = p;
    }
    entries[used++] = temp_var;
    entries[used++] = tmp_var;
    qsort(entries, used, sizeof(*entries), ac_env_compare);
    for (size_t i = 0; i < used; ++i) bytes += strlen(entries[i]) + 1;
    char *block = (char *)malloc(bytes);
    if (block) {
        char *out = block;
        for (size_t i = 0; i < used; ++i) {
            size_t length = strlen(entries[i]) + 1;
            memcpy(out, entries[i], length); out += length;
        }
        *out = 0;
    }
    free(entries);
    FreeEnvironmentStringsA(inherited);
    return block;
}
#endif
