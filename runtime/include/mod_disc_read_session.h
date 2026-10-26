#pragma once
#include "iso_reader.h"
#include <memory>
#include <string>

namespace PSXRecompV4 {
/* Preparation-local host IO only. No file extents or patched sectors are cached.
 * A fresh scope opens the effective disc again; nested scopes share its handle.
 * Directory lookups still read the current effective sector overlay. */
struct ModDiscReadSessionState {
    unsigned depth = 0;
    std::string mount;
    std::unique_ptr<PS1::ISOReader> reader;
};
inline ModDiscReadSessionState& mod_disc_read_session_state() {
    static thread_local ModDiscReadSessionState state;
    return state;
}
class ModDiscReadSession {
public:
    ModDiscReadSession() { ++mod_disc_read_session_state().depth; }
    ~ModDiscReadSession() {
        auto& s = mod_disc_read_session_state();
        if (!--s.depth) { s.reader.reset(); s.mount.clear(); }
    }
    ModDiscReadSession(const ModDiscReadSession&) = delete;
    ModDiscReadSession& operator=(const ModDiscReadSession&) = delete;
};
/* Outside the bounded prepare scope, retain the caller's original fresh reader.
 * A changed mount invalidates the shared reader. Failed opens are never cached. */
inline PS1::ISOReader* mod_disc_read_reader(const std::string& mount,
                                           PS1::ISOReader& fresh) {
    auto& s = mod_disc_read_session_state();
    if (!s.depth) return fresh.Open(mount) ? &fresh : nullptr;
    if (!s.reader || s.mount != mount) {
        s.reader.reset(); s.mount.clear();
        auto next = std::make_unique<PS1::ISOReader>();
        if (!next->Open(mount)) return nullptr;
        s.mount = mount; s.reader = std::move(next);
    }
    return s.reader.get();
}
} // namespace PSXRecompV4
