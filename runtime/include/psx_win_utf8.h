/* psx_win_utf8.h — UTF-8 -> Win32 wide text for dialog APIs (issue #371).
 *
 * The runtime's narrow strings are UTF-8: source literals ("Step 2 of 2 —"),
 * SDL, tinyfiledialogs, TOML, and libstdc++'s std::filesystem, which reads
 * and writes narrow paths as UTF-8 whatever the code page. Narrow ("A") Win32
 * calls and the CRT decode bytes in the process ANSI code page instead. Every
 * shipped exe embeds a manifest that makes that code page UTF-8
 * (assets/windows/psxrecomp.manifest), which settles it on Windows 10 1903
 * and later.
 *
 * Older Windows ignores the manifest and keeps the legacy code page (CP932
 * on Japanese installs). UI text we hand to Win32 therefore goes through the
 * "W" API via these helpers, which is right on every Windows version, and a
 * path a W API returns is kept wide (std::filesystem::path(wchar_t*)) rather
 * than squeezed through the legacy code page.
 *
 * Header-only C++; empty on non-Windows builds. */
#pragma once

#if defined(_WIN32) && defined(__cplusplus)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstring>
#include <string>

namespace psx_win_utf8 {

/* True when the manifest's UTF-8 code page is in effect (Windows 10 1903+). */
inline bool utf8_code_page_active() { return GetACP() == CP_UTF8; }

/* UTF-8 bytes (explicit length, may contain NULs) -> UTF-16. Invalid
 * sequences become U+FFFD rather than failing: this feeds display text. */
inline std::wstring from_utf8(const char* s, std::size_t n) {
    if (!s || n == 0) return std::wstring();
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, nullptr, 0);
    if (wlen <= 0) return std::wstring();
    std::wstring w((std::size_t)wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, &w[0], wlen);
    return w;
}

inline std::wstring from_utf8(const char* s) {
    return s ? from_utf8(s, std::strlen(s)) : std::wstring();
}

inline std::wstring from_utf8(const std::string& s) {
    return from_utf8(s.data(), s.size());
}

/* A double-NUL-terminated UTF-8 multi-string (OPENFILENAME lpstrFilter) ->
 * the same shape in UTF-16, terminators included. */
inline std::wstring multi_sz_from_utf8(const char* s) {
    if (!s) return std::wstring(2, L'\0');
    const char* p = s;
    while (*p) p += std::strlen(p) + 1;
    /* p is at the final NUL; convert through it so both terminators survive. */
    std::wstring w = from_utf8(s, (std::size_t)(p - s) + 1);
    if (w.empty() || w.back() != L'\0') w.push_back(L'\0');
    if (w.size() < 2 || w[w.size() - 2] != L'\0') w.push_back(L'\0');
    return w;
}

}  // namespace psx_win_utf8

#endif  /* _WIN32 && __cplusplus */
