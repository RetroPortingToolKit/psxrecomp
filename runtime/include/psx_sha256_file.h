#pragma once

#include <cstdint>
#include <filesystem>

enum class PsxFileHashResult { Ok, OpenError, ReadError };

// Hash the file's exact bytes, including an empty file. Publish out only on
// success. Host crypto accelerates the calculation; unavailable/failed native
// hashing restarts with the maintained portable implementation.
PsxFileHashResult psx_sha256_file(const std::filesystem::path& path,
                                uint8_t out[32]);
