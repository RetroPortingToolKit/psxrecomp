// Windows UTF-8 text boundary (issue #371).
//
// Built twice by runtime/CMakeLists.txt:
//   win_utf8_helpers_test   — no manifest: the psx_win_utf8.h conversions that
//                             keep dialog text right on Windows < 10 1903.
//   win_utf8_manifest_test  — with the shipped manifest, run with
//                             --expect-utf8-acp: the process code page must
//                             be UTF-8, so a UTF-8 path given to fopen or an
//                             A call lands on its real Unicode name.
#include "psx_win_utf8.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

// "Step 2 of 2 — game disc image" as UTF-8 bytes (em dash = E2 80 94).
static const char kTitleUtf8[] = "Step 2 of 2 \xE2\x80\x94 game disc image";

static void test_from_utf8() {
    const std::wstring w = psx_win_utf8::from_utf8(kTitleUtf8);
    CHECK(w == L"Step 2 of 2 \x2014 game disc image");
    CHECK(psx_win_utf8::from_utf8("").empty());
    CHECK(psx_win_utf8::from_utf8(static_cast<const char*>(nullptr)).empty());
    // Japanese: 日本 = E6 97 A5 E6 9C AC.
    CHECK(psx_win_utf8::from_utf8(std::string("\xE6\x97\xA5\xE6\x9C\xAC")) ==
          L"\x65E5\x672C");
    // Malformed input degrades to U+FFFD instead of dropping the string.
    const std::wstring bad = psx_win_utf8::from_utf8("a\xFF" "b");
    CHECK(bad.size() == 3 && bad[0] == L'a' && bad[1] == 0xFFFD && bad[2] == L'b');
}

static void test_multi_sz() {
    static const char filter[] =
        "PS1 Disc Images (*.cue;*.bin)\0*.cue;*.bin\0All Files (*.*)\0*.*\0";
    const std::wstring w = psx_win_utf8::multi_sz_from_utf8(filter);
    static const wchar_t expect[] =
        L"PS1 Disc Images (*.cue;*.bin)\0*.cue;*.bin\0All Files (*.*)\0*.*\0";
    // sizeof(expect) counts the literal's own terminator: double-NUL end.
    CHECK(w.size() == sizeof(expect) / sizeof(expect[0]));
    CHECK(std::memcmp(w.data(), expect, sizeof(expect)) == 0);

    // Non-ASCII label survives; shape stays double-NUL terminated.
    static const char u8filter[] = "Disc \xE2\x80\x94 images\0*.cue\0";
    const std::wstring u = psx_win_utf8::multi_sz_from_utf8(u8filter);
    static const wchar_t uexpect[] = L"Disc \x2014 images\0*.cue\0";
    CHECK(u.size() == sizeof(uexpect) / sizeof(uexpect[0]));
    CHECK(std::memcmp(u.data(), uexpect, sizeof(uexpect)) == 0);

    const std::wstring empty = psx_win_utf8::multi_sz_from_utf8(nullptr);
    CHECK(empty.size() == 2 && empty[0] == 0 && empty[1] == 0);
}

// The picker returns std::filesystem::path(wchar_t*), and the runtime moves
// paths around as path.string(). That is only lossless because libstdc++
// treats narrow paths as UTF-8 regardless of the code page — pin it, since
// the manifest exists to make Win32 agree with exactly this.
static void test_filesystem_narrow_is_utf8() {
#if defined(__GLIBCXX__)
    const std::filesystem::path wide(L"C:\\\x65E5\x672C\\caf\x00E9 \x2014.cue");
    CHECK(wide.string() ==
          "C:\\\xE6\x97\xA5\xE6\x9C\xAC\\caf\xC3\xA9 \xE2\x80\x94.cue");
    const std::filesystem::path narrow(wide.string());
    CHECK(narrow.wstring() == wide.wstring());
#endif
}

typedef LONG(WINAPI* RtlGetVersionFn)(OSVERSIONINFOW*);

static DWORD windows_build() {
    OSVERSIONINFOW vi;
    std::memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn fn = ntdll ? reinterpret_cast<RtlGetVersionFn>(
                                     reinterpret_cast<void*>(
                                         GetProcAddress(ntdll, "RtlGetVersion")))
                               : nullptr;
    if (!fn || fn(&vi) != 0) return 0;
    return vi.dwMajorVersion > 10 ? 0xFFFFFFFFu
         : vi.dwMajorVersion == 10 ? vi.dwBuildNumber : 0;
}

static void test_manifest_code_page() {
    const DWORD build = windows_build();
    if (build < 18362) {
        std::printf("SKIP manifest checks: Windows build %lu predates 1903\n",
                    (unsigned long)build);
        return;
    }
    CHECK(GetACP() == CP_UTF8);
    CHECK(psx_win_utf8::utf8_code_page_active());

    // A UTF-8 path through the CRT's narrow fopen and a narrow A call must
    // resolve to the file's true Unicode name — the property every narrow
    // call site in the runtime depends on.
    wchar_t tmp_w[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, tmp_w);
    CHECK(n > 0 && n < MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    const std::wstring name_w =
        L"psx_win_utf8_\x65E5\x672C_\x2014_\x00E9_" +
        std::to_wstring(GetCurrentProcessId()) + L".tmp";
    const std::filesystem::path tmp_path(tmp_w);
    const std::wstring wide = (tmp_path / name_w).wstring();
    const std::string narrow = (tmp_path / name_w).string();  // UTF-8

    std::FILE* f = std::fopen(narrow.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f) return;
    std::fputs("x", f);
    std::fclose(f);
    CHECK(GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesA(narrow.c_str()) != INVALID_FILE_ATTRIBUTES);
    DeleteFileW(wide.c_str());
}

int main(int argc, char** argv) {
    test_from_utf8();
    test_multi_sz();
    test_filesystem_narrow_is_utf8();
    if (argc > 1 && std::strcmp(argv[1], "--expect-utf8-acp") == 0) {
        test_manifest_code_page();
    }
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("win_utf8 tests passed (ACP=%u)\n", (unsigned)GetACP());
    return 0;
}
