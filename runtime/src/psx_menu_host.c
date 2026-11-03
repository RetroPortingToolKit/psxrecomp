/* psx_menu_host.c -- provider and disc-op registration (psx_menu_host.h). */
#include "psx_menu_host.h"

#include <stddef.h>

static const PsxMenuProvider *s_provider = NULL;
static const PsxMenuDiscOps *s_disc = NULL;

int psx_menu_set_provider(const PsxMenuProvider *provider) {
    if (provider && provider->abi_version != PSX_MENU_ABI_VERSION) return 0;
    s_provider = provider;
    return 1;
}

const PsxMenuProvider *psx_menu_provider(void) { return s_provider; }

void psx_menu_set_disc_ops(const PsxMenuDiscOps *ops) { s_disc = ops; }
const PsxMenuDiscOps *psx_menu_disc_ops(void) { return s_disc; }
