/* psx_menu_host_test: provider and disc-op registration (psx_menu_host.h). */
#include "psx_menu_host.h"

#include <stdio.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static int attach(void* c, void* w, void* g, const PsxMenuHost* h) { (void)c; (void)w; (void)g; return h != NULL; }
static int count3(void) { return 3; }
static int insert_ok(int d) { return d >= 1 && d <= 3 ? 0 : 2; }

int main(void) {
    /* Default: no provider, so a title's own UI is unchanged. */
    CHECK(psx_menu_provider() == NULL);
    PsxMenuProvider p = {PSX_MENU_ABI_VERSION, "test", NULL, attach, NULL, NULL, NULL, NULL, NULL};
    CHECK(psx_menu_set_provider(&p) == 1 && psx_menu_provider() == &p);
    /* Another ABI version is refused and leaves the registration alone. */
    PsxMenuProvider old = p;
    old.abi_version = PSX_MENU_ABI_VERSION + 1;
    CHECK(psx_menu_set_provider(&old) == 0 && psx_menu_provider() == &p);
    CHECK(psx_menu_set_provider(NULL) == 1 && psx_menu_provider() == NULL);
    /* Disc ops: none until the tray policy publishes them. */
    CHECK(psx_menu_disc_ops() == NULL);
    PsxMenuDiscOps d = {count3, NULL, NULL, insert_ok};
    psx_menu_set_disc_ops(&d);
    CHECK(psx_menu_disc_ops() == &d && psx_menu_disc_ops()->count() == 3 &&
          psx_menu_disc_ops()->insert(2) == 0 && psx_menu_disc_ops()->insert(9) != 0);
    psx_menu_set_disc_ops(NULL);
    CHECK(psx_menu_disc_ops() == NULL);
    if (!fails) printf("psx_menu_host_test: ok\n");
    return fails ? 1 : 0;
}
