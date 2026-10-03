#pragma once
#include <cmath>
#include <cstdlib>
#include <cerrno>
#include <cctype>
inline bool psx_projection_scale_valid(double v) {
    return std::isfinite(v) && v > 0.0 && v <= 8.0;
}
inline bool psx_projection_scale_parse(const char* text, double* out) {
    if (!text || !out) return false;
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (end == text || errno == ERANGE) return false;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end || !psx_projection_scale_valid(value)) return false;
    *out = value;
    return true;
}
/* Preserve the existing milliscale precision without rounding tiny positive
 * values to a zero denominator (which would silently restore identity). */
inline int psx_projection_scale_denominator(double v) {
    const int quantized = static_cast<int>(v * 1000.0 + 0.5);
    return quantized < 1 ? 1 : quantized;
}
