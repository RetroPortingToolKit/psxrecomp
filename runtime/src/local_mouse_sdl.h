#pragma once
#include "psx_sdl.h"

/* All calls, including reset/release, belong to the SDL owner thread. */
void psx_local_mouse_clear();
bool psx_local_mouse_installed();
void psx_local_mouse_reset();
void psx_local_mouse_begin(SDL_Window* window, bool live);
bool psx_local_mouse_event(const SDL_Event& event);
void psx_local_mouse_pad(bool connected, bool analog, uint16_t buttons,
                         uint8_t& rx, uint8_t& ry);
