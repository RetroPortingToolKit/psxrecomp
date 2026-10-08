#include "mod_local_input_policy.h"

#include <stdio.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", #cond); failures++; } } while (0)

int main(void) {
    /* Digital guest pad mode does not gate local presentation input. */
    CHECK(psx_mod_local_input_available(1, 1, 0, 0));
    CHECK(!psx_mod_local_input_available(0, 1, 0, 0));
    CHECK(!psx_mod_local_input_available(1, 0, 0, 0));
    CHECK(!psx_mod_local_input_available(1, 1, 1, 0));
    CHECK(!psx_mod_local_input_available(1, 1, 0, 1));
    return failures ? 1 : 0;
}
