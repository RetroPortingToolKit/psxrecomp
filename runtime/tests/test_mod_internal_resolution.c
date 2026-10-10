#include "mod_plugins.h"
#include "mod_internal_resolution.h"
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Failed: %s\n", #x); return 1; } } while (0)

int main(void) {
    /* Disabled sessions retain the configured baseline; a later launch must
     * not inherit an earlier active graphics mod's requested height. */
    CHECK(psx_mod_internal_resolution_request() == 0u);
    CHECK(psx_mod_set_internal_resolution(1080u));
    CHECK(psx_mod_internal_resolution_request() == 1080u);
    CHECK(!psx_mod_set_internal_resolution(119u));
    CHECK(!psx_mod_set_internal_resolution(8193u));
    CHECK(!psx_mod_set_internal_resolution(UINT32_MAX));
    CHECK(psx_mod_internal_resolution_request() == 1080u);
    psx_mod_internal_resolution_reset();
    CHECK(psx_mod_internal_resolution_request() == 0u);
    CHECK(psx_mod_set_internal_resolution(120u));
    CHECK(psx_mod_set_internal_resolution(8192u));
    CHECK(psx_mod_set_internal_resolution(0u));
    CHECK(psx_mod_internal_resolution_request() == 0u);
    puts("session-scoped mod resolution validation passed");
    return 0;
}
