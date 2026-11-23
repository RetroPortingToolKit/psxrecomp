#include "replay_input_scope.h"
#include "sio.h"
#include <stdio.h>

int replay_sio_input_contract(uint64_t *profile, char *why, size_t cap) {
    if (sio_get_multitap() || sio_get_pads_swapped()) {
        snprintf(why, cap, "this controller layout is not supported");
        return 0;
    }
    uint64_t config = 0;
    for (int slot = 0; slot < 2; ++slot) {
        const int connected = sio_get_pad_connected(slot);
        const int capable = sio_get_pad_config_capable(slot);
        if (sio_get_port_device(slot) != SIO_DEVICE_PAD || sio_get_pad_analog(slot) > SIO_PAD_DUALSHOCK) {
            snprintf(why, cap, "special peripherals are not supported");
            return 0;
        }
        config |= (uint64_t)(connected | (capable << 1)) << (slot * 2);
        if (slot == 1 && connected) {
            uint8_t axes[4]; sio_get_pad_sticks(slot, axes);
            const int axes_visible = sio_get_pad_analog(slot) != SIO_PAD_DIGITAL || sio_get_pad_config_mode(slot);
            if (sio_get_pad_buttons_slot(slot) != 0xFFFFu ||
                (axes_visible && (axes[0] != 0x80 || axes[1] != 0x80 || axes[2] != 0x80 || axes[3] != 0x80))) {
                snprintf(why, cap, "Player 2 input is not recorded");
                return 0;
            }
        }
    }
    *profile = (sio_pad_host_type_generation() << 4) | config;
    return 1;
}
