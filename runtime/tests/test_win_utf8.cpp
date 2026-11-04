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

static BOOL CALLBACK count_manifest(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR data) {
    ++*reinterpret_cast<unsigned*>(data);
    CHECK(IS_INTRESOURCE(name) && reinterpret_cast<ULONG_PTR>(name) == 1);
    return TRUE;
}

static void test_manifest_resource() {
    HMODULE module = GetModuleHandleW(nullptr);
    unsigned count = 0;
    CHECK(EnumResourceNamesW(module, MAKEINTRESOURCEW(24), count_manifest,
                             reinterpret_cast<LONG_PTR>(&count)) != 0);
    CHECK(count == 1);  // ld can exit 0 while retaining duplicate ID-1 entries.
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24));
    CHECK(resource != nullptr);
    if (!resource) return;
    HGLOBAL loaded = LoadResource(module, resource);
    const char* bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    CHECK(bytes != nullptr);
    if (!bytes) return;
    const std::string xml(bytes, SizeofResource(module, resource));
    CHECK(xml.find(">UTF-8</activeCodePage>") != std::string::npos);
    CHECK(xml.find(">true</longPathAware>") != std::string::npos);
    CHECK(xml.find("level=\"asInvoker\"") != std::string::npos);
    for (const char* id : {"e2011457-1546-43c5-a5fe-008deee3d3f0",
                           "35138b9a-5d96-4fbd-8e2d-a2440225f93a",
                           "4a2f28e3-53b9-4441-ba9c-d69d4a4a6e38",
                           "1f676c76-80e1-4239-95bb-83d0f6d0da78",
                           "8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a"}) {
        CHECK(xml.find(id) != std::string::npos);
    }
}

int main(int argc, char** argv) {
    test_from_utf8();
    test_multi_sz();
    test_filesystem_narrow_is_utf8();
    if (argc > 1 && std::strcmp(argv[1], "--expect-utf8-acp") == 0) {
        test_manifest_resource();
        test_manifest_code_page();
    }
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("win_utf8 tests passed (ACP=%u)\n", (unsigned)GetACP());
    return 0;
}
