/* Production debug access is an explicit persisted launcher mod choice. */
#include "mod_plugins.h"
#include "debug_server.h"

static void builtin_debug_server_activate(void)
{
    debug_server_request(1);
}

PSX_MOD_CONSTRUCTOR(psx_register_builtin_debug_plugin) {
    (void)psx_mod_register_activation_plugin(
        "psx.debug-server", builtin_debug_server_activate);
}
