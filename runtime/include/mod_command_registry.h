#pragma once
#include "mod_plugins.h"
#include <cstring>
#include <mutex>

namespace psx {
class ModCommandRegistry {
    struct Command { char name[64]; PSXModCommandFn fn; };
    std::mutex mutex_;
    Command commands_[16]{};
    int count_ = 0;
public:
    int add(const char* name, PSXModCommandFn fn) {
        if (!name || !name[0] || std::strlen(name) >= sizeof(commands_[0].name) || !fn) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (int i = 0; i < count_; ++i)
            if (!std::strcmp(commands_[i].name, name)) { commands_[i].fn = fn; return 1; }
        if (count_ >= 16) return 0;
        auto& entry = commands_[count_++];
        std::memcpy(entry.name, name, std::strlen(name) + 1);
        entry.fn = fn;
        return 1;
    }
    int run(const char* name, const char* args, char* reply, int capacity) {
        if (!reply || capacity <= 0) return -1;
        PSXModCommandFn fn = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (int i = 0; i < count_; ++i)
                if (name && !std::strcmp(commands_[i].name, name)) fn = commands_[i].fn;
        }
        reply[0] = '\0';
        if (!fn) return -1;
        const int result = fn(args ? args : "", reply, capacity);
        reply[capacity - 1] = '\0';
        return result;
    }
};

inline ModCommandRegistry& mod_command_registry() {
    // C plugin constructors can run before C++ translation-unit globals.
    // Initialize the mutex and storage together on the first registration.
    static ModCommandRegistry registry;
    return registry;
}
} // namespace psx
