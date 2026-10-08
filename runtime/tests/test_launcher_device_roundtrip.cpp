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
    return 0;
}
