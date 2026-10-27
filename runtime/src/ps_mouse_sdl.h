#pragma once
#include "psx_sdl.h"

/* SDL adapter for the emulated PlayStation Mouse (ps_mouse_input.h).
 *
 * Inert unless a controller port is configured as the mouse
 * (psx_ps_mouse_set_enabled). Then a left click in the game window captures
 * the host mouse in relative mode; that click is not passed to the guest.
 * While captured, motion and the left/right buttons drive the PlayStation
 * Mouse and are not seen by other mouse consumers. Escape, focus loss,
 * hide/minimize, mouse removal and any frame that is not live (menus, rewind,
 * save-state guard, netplay, injected input, headless) release the capture
 * and drop owed motion and buttons.
 *
 * All calls belong to the SDL owner thread. */
void psx_ps_mouse_set_enabled(bool enabled);
bool psx_ps_mouse_enabled();
bool psx_ps_mouse_captured();
/* Per frame, before draining events: `live` is the same gate the local
 * mouse policy uses. Not live, unfocused or hidden releases the capture. */
void psx_ps_mouse_begin(SDL_Window* window, bool live);
/* Returns true when the event was consumed by the PlayStation Mouse. */
bool psx_ps_mouse_event(const SDL_Event& event);
/* Release the capture (if any) and drop owed motion and buttons. */
void psx_ps_mouse_release();
