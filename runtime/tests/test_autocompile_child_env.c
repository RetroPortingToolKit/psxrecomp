/* Real child launch: private TEMP/TMP, inherited variables, parent isolation. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char s_cache_dir[512]; /* same input capacity as autocompile.c */
#include "autocompile_child_env.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc, char **argv) {
    char temp[1024], tmp[1024];
    if (argc == 3 && !strcmp(argv[1], "--child")) {
        CHECK(GetEnvironmentVariableA("TEMP", temp, sizeof(temp)) > 0);
        CHECK(GetEnvironmentVariableA("TMP", tmp, sizeof(tmp)) > 0);
        CHECK(!strcmp(temp, argv[2]) && !strcmp(tmp, argv[2]));
        char marker[32];
        CHECK(GetEnvironmentVariableA("PSX_AC_TEMP_TEST", marker, sizeof(marker)) > 0);
        CHECK(!strcmp(marker, "preserved"));
        return 0;
    }
    char cwd[512], cache[600], expected[610], exe[1024], cmd[2300];
    CHECK(GetCurrentDirectoryA(sizeof(cwd), cwd) > 0);
    CHECK(GetEnvironmentVariableA("TEMP", temp, sizeof(temp)) > 0);
    CHECK(GetEnvironmentVariableA("TMP", tmp, sizeof(tmp)) > 0);
    CHECK(SetEnvironmentVariableA("PSX_AC_TEMP_TEST", "preserved"));
    snprintf(cache, sizeof(cache), "%s\\autocompile-child-cache", cwd);
    CHECK(CreateDirectoryA(cache, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    char *env = ac_child_environment(cache);
    CHECK(env != NULL);
    int temps = 0, tmps = 0;
    const char *previous = NULL;
    for (char *p = env; *p; p += strlen(p) + 1) {
        CHECK(!previous || _stricmp(previous, p) <= 0);
        if (!_strnicmp(p, "TEMP=", 5)) ++temps;
        if (!_strnicmp(p, "TMP=", 4)) ++tmps;
        previous = p;
    }
    CHECK(temps == 1 && tmps == 1);
    snprintf(expected, sizeof(expected), "%s\\tmp", cache);
    CHECK(GetModuleFileNameA(NULL, exe, sizeof(exe)) > 0);
    snprintf(cmd, sizeof(cmd), "\"%s\" --child \"%s\"", exe, expected);
    STARTUPINFOA si = {0}; PROCESS_INFORMATION pi = {0}; si.cb = sizeof(si);
    CHECK(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                         env, cwd, &si, &pi));
    free(env);
    CHECK(WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0);
    DWORD code = 1; CHECK(GetExitCodeProcess(pi.hProcess, &code));
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CHECK(code == 0);
    char after[1024];
    CHECK(GetEnvironmentVariableA("TEMP", after, sizeof(after)) > 0 && !strcmp(after,temp));
    CHECK(GetEnvironmentVariableA("TMP", after, sizeof(after)) > 0 && !strcmp(after,tmp));
    /* A file occupying <cache>/tmp must refuse, never inherit another TEMP. */
    snprintf(cache, sizeof(cache), "%s\\autocompile-blocked-cache", cwd);
    CHECK(CreateDirectoryA(cache, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    snprintf(expected, sizeof(expected), "%s\\tmp", cache);
    FILE *file = fopen(expected, "wb"); CHECK(file != NULL); fclose(file);
    CHECK(ac_child_environment(cache) == NULL);
    puts("PASS: compiler child gets cache scratch; parent TEMP/TMP stay unchanged; invalid scratch refuses");
    return 0;
}
