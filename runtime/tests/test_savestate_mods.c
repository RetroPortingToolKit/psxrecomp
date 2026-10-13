/* savestate_mods_test: a slot's recorded mod set vs the current one. */
#include "savestate.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

int main(void) {
    /* No record (a state saved before records existed): never warns. */
    CHECK(!savestate_mods_record_differs(NULL, "abc"));
    CHECK(!savestate_mods_record_differs(NULL, ""));
    /* Same set, with the sidecar's trailing newline. */
    CHECK(!savestate_mods_record_differs("abc123\n", "abc123"));
    CHECK(!savestate_mods_record_differs("abc123\r\n", "abc123"));
    /* Unmodded save, unmodded now. */
    CHECK(!savestate_mods_record_differs("\n", ""));
    CHECK(!savestate_mods_record_differs("", NULL));
    /* Differences: other plan, mods added, mods removed. */
    CHECK(savestate_mods_record_differs("abc123\n", "def456"));
    CHECK(savestate_mods_record_differs("\n", "def456"));
    CHECK(savestate_mods_record_differs("abc123\n", ""));
    if (!fails) printf("savestate_mods_test: ok\n");
    return fails ? 1 : 0;
}
