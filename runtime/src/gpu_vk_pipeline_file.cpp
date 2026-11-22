#include "atomic_file_write.h"

namespace {
std::filesystem::path cache_path(const char* directory, const char* filename) {
    return std::filesystem::u8path(directory) / "vulkan-pipelines" / filename;
}
}

extern "C" size_t vk_pipeline_file_read(const char* directory, const char* filename,
                                         void* data, size_t capacity) noexcept {
    try {
        std::ifstream file(cache_path(directory, filename), std::ios::binary | std::ios::ate);
        const auto length = file.tellg();
        if (length <= 0 || length > static_cast<std::streamoff>(capacity)) return 0;
        file.seekg(0);
        return file.read(static_cast<char*>(data), length) ? static_cast<size_t>(length) : 0;
    } catch (...) {
        return 0;
    }
}

extern "C" void vk_pipeline_file_write(const char* directory, const char* filename,
                                       const void* data, size_t size) noexcept {
    try {
        const auto path = cache_path(directory, filename);
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (!error) psx::atomic_write_file(path, {static_cast<const char*>(data), size});
    } catch (...) {
        // Pipeline data is disposable. The next launch can start with an empty cache.
    }
}
