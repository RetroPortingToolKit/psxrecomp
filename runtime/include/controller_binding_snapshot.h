#pragma once
#include "psx_sdl.h"
#include <recomp_input.h>

/* SDL handles and source selection belong to the frontend. The provider only
 * evaluates the selected button/axis snapshot under its raw-axis threshold. */
static inline bool psx_controller_binding_pressed(SDL_GameController* controller,
                                                  uint32_t binding, int deadzone) {
    if (!controller) return false;
    ri_device snapshot{};
    if (binding >= RI_BUTTON(0) && binding <= RI_BUTTON(31)) {
        const unsigned button = binding - RI_BUTTON(0);
        if (SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)button))
            snapshot.buttons = 1u << button;
    } else if (binding >= RI_AXIS(0, 0) && binding <= RI_AXIS(RI_MAX_AXES - 1, 1)) {
        const unsigned axis = (binding - RI_AXIS(0, 0)) / 2;
        snapshot.axes[axis] = SDL_GameControllerGetAxis(controller, (SDL_GameControllerAxis)axis);
    }
    return ri_binding_held(&snapshot, binding, deadzone) != 0;
}
