#ifndef PSX_CONTROLLER_TYPE_H
#define PSX_CONTROLLER_TYPE_H

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <string>

/* SDL2/3 does not expose a consistently useful wheel class for every mapping.
 * This is a conservative name hint; only mapped controllers with known wheel
 * family names are promoted to the JogCon device protocol. */
inline bool psx_controller_name_is_wheel(const char* name) {
    if (!name || !*name) return false;
    std::string normalized(name);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    static const char* tokens[] = {
        "wheel", "g29", "g920", "g923", "g27", "g25", "driving force",
        "t150", "t300", "t500", "t248", "tx racing", "thrustmaster",
        "fanatec", "moza"
    };
    for (const char* token : tokens)
        if (normalized.find(token) != std::string::npos) return true;
    return false;
}

/* Pure reopen policy so unplug and launcher-selected device changes can be
 * regression-tested without a physical SDL controller. */
inline bool psx_controller_handle_needs_close(bool controller_selected,
                                               bool handle_open,
                                               bool handle_attached,
                                               const char* selected_guid,
                                               const char* active_guid) {
    if (!controller_selected) return true;
    if (!handle_open) return false;
    if (!handle_attached) return true;
    return selected_guid && selected_guid[0] && active_guid && active_guid[0] &&
           std::strcmp(selected_guid, active_guid) != 0;
}

/* Recover even if a removal/add event was lost. Healthy devices must not be
 * reopened (that would reset the guest pad protocol). Retry absent devices
 * at a bounded rate instead of enumerating joysticks every video frame. */
inline bool psx_controller_recovery_needed(bool selected, bool open,
                                         bool attached, uint64_t now_ms,
                                         uint64_t last_retry_ms) {
    return selected && ((open && !attached) ||
                        (!open && now_ms - last_retry_ms >= 500));
}

#endif
