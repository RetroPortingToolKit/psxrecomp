// Include the maintained implementation to inject backend failures without a
// product hook or a second copy of its stream/fallback logic.
#include "../src/psx_sha256_file.cpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

static unsigned checks;
static void check(bool value, const char* message) {
    ++checks;
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

static std::string hex(const uint8_t* digest) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result;
    for (size_t i = 0; i < 32; ++i) {
        result += alphabet[digest[i] >> 4];
        result += alphabet[digest[i] & 15];
    }
    return result;
}

struct FailedBackend {
    int stage;
    unsigned calls = 0;
    bool failed = false;
    bool init() { return !(failed = stage == 0); }
    bool update(const uint8_t*, size_t) {
        ++calls;
        return !(failed = stage == 1 && calls == 2);
    }
    bool finish(uint8_t out[32]) {
        std::fill(out, out + 32, 0x19); // failure must never publish this
        failed = true;
        return false;
    }
};

int main(int argc, char** argv) {
    // Optional private file probe: one real host backend or portable build,
    // suitable for separate isolated throughput measurements.
    if (argc == 2) {
        uint8_t digest[32];
        const auto begin = std::chrono::steady_clock::now();
        const auto result = psx_sha256_file(std::filesystem::u8path(argv[1]), digest);
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - begin).count();
        if (result != PsxFileHashResult::Ok) return 2;
        std::cout << hex(digest) << " " << seconds << '\n';
        return 0;
    }

#if !defined(PSX_SHA256_FILE_PORTABLE) && (defined(_WIN32) || defined(PSX_SHA256_FILE_OPENSSL))
    NativeHash available;
    check(available.init(), "native backend is available on qualification host");
#endif

    const auto unique = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path() /
        ("psx-sha256-" + std::to_string(unique));
    check(std::filesystem::create_directory(root), "private fixture directory");
    const auto path = root / std::filesystem::u8path(u8"disc-\u00e9-\u65e5.bin");
    std::vector<uint8_t> bytes;
    constexpr size_t lengths[] = {
        0, 1, 3, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129,
        1048575, 1048576, 1048577, 2097169};
    uint32_t rng = 0x53484132;
    for (size_t size : lengths) {
        bytes.resize(size);
        for (auto& byte : bytes) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            byte = static_cast<uint8_t>(rng);
        }
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!bytes.empty()) file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            check(static_cast<bool>(file), "write fixture");
        }
        std::array<uint8_t, 32> expected{}, observed{};
        psx_sha256_compute(bytes.data(), bytes.size(), expected.data());
        check(psx_sha256_file(path, observed.data()) == PsxFileHashResult::Ok,
              "hash file");
        check(observed == expected, "native/portable exact digest");
        if (!size) check(hex(observed.data()) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            "standard empty vector");
    }
    // The largest vector consumes more than one native chunk before fallback.
    std::array<uint8_t, 32> expected{}, observed{};
    psx_sha256_compute(bytes.data(), bytes.size(), expected.data());
    for (int stage = 0; stage < 3; ++stage) {
        FailedBackend backend{stage};
        check(hash_file(path, observed.data(), backend) == PsxFileHashResult::Ok,
              "native failure falls back");
        check(backend.failed && observed == expected, "fallback restarts at byte zero");
        if (stage == 1) check(backend.calls == 2, "injected partial update failure");
    }
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "abc";
    }
    check(psx_sha256_file(path, observed.data()) == PsxFileHashResult::Ok &&
          hex(observed.data()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "standard abc vector");
    observed.fill(0xa5);
    expected = observed;
    check(psx_sha256_file(root / "absent", observed.data()) == PsxFileHashResult::OpenError,
          "missing file rejected");
    check(observed == expected, "missing file leaves output untouched");
    check(psx_sha256_file(root, observed.data()) != PsxFileHashResult::Ok,
          "directory rejected");
    check(observed == expected, "unreadable file leaves output untouched");
    // Delete only our two exact fixture paths; no recursive cleanup.
    check(std::filesystem::remove(path), "remove fixture file");
    check(std::filesystem::remove(root), "remove empty fixture directory");
    std::cout << checks << " SHA-256 file checks passed\n";
}
