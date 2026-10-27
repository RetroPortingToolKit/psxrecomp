#include "launcher_device.h"

#include <cassert>

int main() {
    assert(PSXRecompV4::launcher_controller_can_share(true, true));
    assert(!PSXRecompV4::launcher_controller_can_share(true, false));
    assert(!PSXRecompV4::launcher_controller_can_share(false, true));
    using PSXRecompV4::launcher_device_from_source;
    using PSXRecompV4::launcher_source_from_device;

    assert(launcher_source_from_device("") == 0);
    assert(launcher_source_from_device(" none ") == 0);
    assert(launcher_source_from_device("Keyboard") == 1);
    assert(launcher_source_from_device("gamepad") == 2);
    assert(launcher_source_from_device("030000005e0400008e02000000000000") == 2);

    assert(launcher_device_from_source(0, "keyboard") == "none");
    assert(launcher_device_from_source(1, "none") == "keyboard");
    assert(launcher_device_from_source(2, "keyboard") == "gamepad");
    assert(launcher_device_from_source(2, "none") == "gamepad");
    assert(launcher_device_from_source(2, " auto ") == "auto");
    assert(launcher_device_from_source(
        2, " 030000005e0400008e02000000000000 ") ==
        "030000005e0400008e02000000000000");

    // A PlayStation Mouse seat shows as Keyboard; keeping Keyboard keeps the
    // mouse, None or a gamepad replace it.
    using PSXRecompV4::launcher_device_is_ps_mouse;
    assert(launcher_device_is_ps_mouse(" Mouse "));
    assert(launcher_device_is_ps_mouse("ps_mouse"));
    assert(launcher_device_is_ps_mouse("PlayStation_Mouse"));
    assert(!launcher_device_is_ps_mouse("keyboard"));
    assert(!launcher_device_is_ps_mouse("mousepad"));
    assert(launcher_source_from_device("mouse") == 1);
    assert(launcher_device_from_source(1, " mouse ") == "mouse");
    assert(launcher_device_from_source(1, "keyboard") == "keyboard");
    assert(launcher_device_from_source(0, "mouse") == "none");
    assert(launcher_device_from_source(2, "mouse") == "gamepad");

    // Player 1's "PlayStation Mouse" entry (supports_ps_mouse + a recomp-ui
    // with RECOMP_LAUNCHER_HAS_MOUSE_SOURCE): seeded from the device, and the
    // keyboard-family source written back from the player's choice.
    using PSXRecompV4::launcher_keyboard_device;
    using PSXRecompV4::launcher_mouse_enabled_from_device;
    assert(launcher_mouse_enabled_from_device("mouse") == 1);
    assert(launcher_mouse_enabled_from_device(" PlayStation_Mouse ") == 1);
    assert(launcher_mouse_enabled_from_device("keyboard") == 0);
    assert(launcher_mouse_enabled_from_device("gamepad") == 0);
    assert(launcher_mouse_enabled_from_device("none") == 0);
    assert(launcher_keyboard_device(true, 1, "keyboard") == "mouse");
    assert(launcher_keyboard_device(true, 1, "none") == "mouse");
    assert(launcher_keyboard_device(true, 0, "mouse") == "keyboard");
    assert(launcher_keyboard_device(true, 0, "keyboard") == "keyboard");
    // Without the entry (title without supports_ps_mouse, or an older
    // recomp-ui) a settings.toml mouse seat survives the launcher.
    assert(launcher_keyboard_device(false, 0, "mouse") == "mouse");
    assert(launcher_keyboard_device(false, 1, "keyboard") == "keyboard");
    assert(launcher_keyboard_device(false, 0, "none") == "keyboard");
    return 0;
}
