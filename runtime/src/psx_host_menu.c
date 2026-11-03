#include "psx_host_menu.h"
#include "psx_runtime_menu.h"
#include "psx_savestate_menu.h"
int psx_host_menu_overlay_image(const uint32_t **pixels, int *w, int *h) {
    if (psx_runtime_menu_overlay_image(pixels, w, h)) return 1;
    return psx_savestate_menu_overlay_image(pixels, w, h);
}
