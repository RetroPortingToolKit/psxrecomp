#ifndef PSX_PGXP_DEPRECATION_HPP
#define PSX_PGXP_DEPRECATION_HPP

#include "pgxp_deprecation.h"
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace PSXRecompV4 { namespace precision {

inline bool legacy_option_enabled(const char* value) {
    if (!value || !*value) return false;
    const std::size_t length = std::strlen(value);
    if (length > 5) return true;
    char normalized[6] = {};
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        normalized[i] = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    }
    return std::strcmp(normalized, "0") != 0 &&
           std::strcmp(normalized, "off") != 0 &&
           std::strcmp(normalized, "false") != 0 &&
           std::strcmp(normalized, "no") != 0;
}

[[noreturn]] inline void reject_legacy_operations() {
    throw std::logic_error(PSX_PGXP_OPERATIONS_DEPRECATION_MESSAGE);
}

inline void validate_legacy_environment() {
    if (legacy_option_enabled(std::getenv("PSX_NATIVE_PRECISION_SLICES")))
        throw std::logic_error(PSX_NATIVE_PRECISION_SLICES_DEPRECATION_MESSAGE);
    if (legacy_option_enabled(std::getenv("PSX_PGXP_DATAFLOW")))
        throw std::logic_error(PSX_PGXP_DATAFLOW_DEPRECATION_MESSAGE);
    if (legacy_option_enabled(std::getenv("PSX_PGXP_OPERATIONS")))
        reject_legacy_operations();
}

} } // namespace PSXRecompV4::precision

#endif
