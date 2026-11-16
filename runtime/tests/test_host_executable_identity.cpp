#include "host_executable_identity.h"
#include "psx_sha256_file.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
int main(int argc,char** argv) {
    try {
        CHECK(argc==2); // Harness supplies the real binary independently.
        std::array<uint8_t,32> expected{},actual{},again{};
        CHECK(psx_sha256_file(std::filesystem::path(argv[1]),expected.data())==PsxFileHashResult::Ok);
        CHECK(!psx_running_executable_sha256(nullptr));
#if defined(_WIN32) || defined(__linux__)
        CHECK(psx_running_executable_sha256(actual.data()));
        CHECK(actual==expected);
        // Source of identity cannot silently turn into cwd/argv lookup.
        const auto cwd=std::filesystem::current_path();
        std::filesystem::current_path(cwd.root_path());
        CHECK(psx_running_executable_sha256(again.data()));
        std::filesystem::current_path(cwd);
        CHECK(again==expected);
#else
        actual.fill(0xa5);again=actual;
        CHECK(!psx_running_executable_sha256(actual.data()));
        CHECK(actual==again);
#endif
        for (auto b:expected) std::printf("%02x",b);
        std::puts(" running image identity PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"running image identity: %s\n",error.what());
        return 1;
    }
}
