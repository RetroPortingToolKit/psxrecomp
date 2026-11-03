#ifndef PSX_RUNTIME_MENU_H
#define PSX_RUNTIME_MENU_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Shared recomp-ui model. These operations run on the emulation thread. */
enum PsxRuntimeMenuInput { PSX_MENU_TOGGLE, PSX_MENU_BACK, PSX_MENU_UP,
    PSX_MENU_DOWN, PSX_MENU_LEFT, PSX_MENU_RIGHT, PSX_MENU_ACCEPT };
void psx_runtime_menu_init(void);
int psx_runtime_menu_open(void);
void psx_runtime_menu_close(void);
int psx_runtime_menu_is_open(void);
void psx_runtime_menu_input(int input, int repeat);
int psx_runtime_menu_selected_disc(void);
int psx_runtime_menu_select_disc(int disc);
int psx_runtime_menu_insert(void);
int psx_runtime_menu_overlay_image(const uint32_t **pixels, int *w, int *h);
struct RecompRuntimeUi;
struct RecompRuntimeUi *psx_runtime_menu_model(void);
/* The framebuffer menu is available only while no provider is attached. */
void psx_runtime_menu_set_fallback_enabled(int enabled);
#ifdef __cplusplus
}
#endif
#endif
