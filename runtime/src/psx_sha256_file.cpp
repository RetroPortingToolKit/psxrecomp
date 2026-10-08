#include "psx_sha256_file.h"
#include "psx_sha256.h"

#include <algorithm>
#include <fstream>
#include <vector>

#if defined(_WIN32) && !defined(PSX_SHA256_FILE_PORTABLE)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(PSX_SHA256_FILE_OPENSSL) && !defined(PSX_SHA256_FILE_PORTABLE)
#include <openssl/evp.h>
#endif

namespace {

class NativeHash {
public:
    NativeHash() = default;
    NativeHash(const NativeHash&) = delete;
    NativeHash& operator=(const NativeHash&) = delete;

#if defined(_WIN32) && !defined(PSX_SHA256_FILE_PORTABLE)
    ~NativeHash() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    bool init() {
        return BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                           nullptr, 0) >= 0 &&
               BCryptCreateHash(algorithm_, &hash_, nullptr, 0,
                                nullptr, 0, 0) >= 0;
    }
    bool update(const uint8_t* data, size_t size) {
        // The reader passes at most 1 MiB, within BCrypt's ULONG length.
        return BCryptHashData(hash_, const_cast<PUCHAR>(data),
                              static_cast<ULONG>(size), 0) >= 0;
    }
    bool finish(uint8_t out[32]) {
        return BCryptFinishHash(hash_, out, 32, 0) >= 0;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
#elif defined(PSX_SHA256_FILE_OPENSSL) && !defined(PSX_SHA256_FILE_PORTABLE)
    ~NativeHash() { EVP_MD_CTX_free(ctx_); }
    bool init() {
        ctx_ = EVP_MD_CTX_new();
        return ctx_ && EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) == 1;
    }
    bool update(const uint8_t* data, size_t size) {
        return EVP_DigestUpdate(ctx_, data, size) == 1;
    }
    bool finish(uint8_t out[32]) {
        unsigned int size = 0;
        return EVP_DigestFinal_ex(ctx_, out, &size) == 1 && size == 32;
    }
private:
    EVP_MD_CTX* ctx_ = nullptr;
#else
    bool init() { return false; }
    bool update(const uint8_t*, size_t) { return false; }
    bool finish(uint8_t[32]) { return false; }
#endif
};

// A backend error may occur after consuming input. Restart the whole stream,
// never mix states from different implementations or publish a partial hash.
template<class Hash>
bool try_native(std::ifstream& file, std::vector<uint8_t>& buffer,
                uint8_t digest[32], Hash& native) {
    if (!native.init()) return false;
    while (file) {
        file.read(reinterpret_cast<char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
        const auto got = file.gcount();
        if (got > 0 && !native.update(buffer.data(), static_cast<size_t>(got)))
            return false;
    }
    return !file.bad() && file.eof() && native.finish(digest);
}

template<class Hash>
PsxFileHashResult hash_file(const std::filesystem::path& path,
                           uint8_t out[32], Hash& native) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return PsxFileHashResult::OpenError;
    std::vector<uint8_t> buffer(1024 * 1024);
    uint8_t digest[32];
    if (!try_native(file, buffer, digest, native)) {
        if (file.bad()) return PsxFileHashResult::ReadError;
        file.clear();
        file.seekg(0, std::ios::beg);
        if (!file) return PsxFileHashResult::ReadError;
        psx_sha256_ctx portable;
        psx_sha256_init(&portable);
        while (file) {
            file.read(reinterpret_cast<char*>(buffer.data()),
                      static_cast<std::streamsize>(buffer.size()));
            const auto got = file.gcount();
            if (got > 0)
                psx_sha256_update(&portable, buffer.data(), static_cast<size_t>(got));
        }
        if (file.bad() || !file.eof()) return PsxFileHashResult::ReadError;
        psx_sha256_final(&portable, digest);
    }
    std::copy(digest, digest + 32, out);
    return PsxFileHashResult::Ok;
}

} // namespace

PsxFileHashResult psx_sha256_file(const std::filesystem::path& path,
                                uint8_t out[32]) {
    NativeHash native;
    return hash_file(path, out, native);
}
