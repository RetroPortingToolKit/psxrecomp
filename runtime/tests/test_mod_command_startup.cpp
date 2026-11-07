#include "mod_command_registry.h"
#include <cstdio>
#include <cstring>

static int registered_before_main;
static int echo(const char* args, char* reply, int capacity) {
    std::snprintf(reply, capacity, "%s", args);
    return 1;
}

PSX_MOD_CONSTRUCTOR(register_early_command) {
    registered_before_main = psx::mod_command_registry().add("startup.echo", echo);
}

int main() {
    char reply[16];
    auto& registry = psx::mod_command_registry();
    if (!registered_before_main ||
        registry.run("startup.echo", "ready", reply, sizeof(reply)) != 1 ||
        std::strcmp(reply, "ready") ||
        registry.add("startup.echo", echo) != 1 ||
        registry.run("missing", "", reply, sizeof(reply)) != -1 || reply[0] ||
        registry.add("", echo) != 0) {
        std::fputs("mod command startup: FAIL\n", stderr);
        return 1;
    }
    std::puts("mod command startup: PASS");
    return 0;
}
