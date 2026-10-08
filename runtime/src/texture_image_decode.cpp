#include "texture_image_decode.h"

#include "../third_party/stb_image.h"
#include <webp/decode.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace {

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool probe_impl(const uint8_t* encoded, size_t encoded_size,
                size_t decoded_budget, TextureImageProbe& probe,
                std::string& error) {
    if (!encoded || !encoded_size) return fail(error, "texture image is empty");
    if (encoded_size > TEXTURE_IMAGE_MAX_ENCODED_BYTES)
        return fail(error, "texture image exceeds 64 MiB encoded limit");
    int width = 0, height = 0, channels = 0;
    static const uint8_t png_magic[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (encoded_size >= sizeof(png_magic) &&
        std::memcmp(encoded, png_magic, sizeof(png_magic)) == 0) {
        probe.format = TextureImageFormat::Png;
        if (!stbi_info_from_memory(encoded, static_cast<int>(encoded_size),
                                   &width, &height, &channels))
            return fail(error, "malformed PNG texture image header");
    } else if (encoded_size >= 2 && encoded[0] == 0xff && encoded[1] == 0xd8) {
        probe.format = TextureImageFormat::Jpeg;
        if (!stbi_info_from_memory(encoded, static_cast<int>(encoded_size),
                                   &width, &height, &channels))
            return fail(error, "malformed JPEG texture image header");
    } else if (encoded_size >= 12 && std::memcmp(encoded, "RIFF", 4) == 0 &&
               std::memcmp(encoded + 8, "WEBP", 4) == 0) {
        probe.format = TextureImageFormat::WebP;
        WebPBitstreamFeatures features{};
        if (WebPGetFeatures(encoded, encoded_size, &features) != VP8_STATUS_OK)
            return fail(error, "malformed WebP texture image header");
        if (features.has_animation)
            return fail(error, "animated WebP texture images are unsupported");
        width = features.width;
        height = features.height;
    } else {
        return fail(error, "texture image must be PNG, JPEG, or WebP");
    }
    if (width <= 0 || height <= 0 || width > static_cast<int>(TEXTURE_IMAGE_MAX_DIMENSION) ||
        height > static_cast<int>(TEXTURE_IMAGE_MAX_DIMENSION))
        return fail(error, "texture image dimensions exceed 8192 pixels per side");
    const uint64_t rgba_bytes = static_cast<uint64_t>(width) * height * 4u;
    if (rgba_bytes > decoded_budget || rgba_bytes > TEXTURE_IMAGE_DEFAULT_DECODED_BUDGET)
        return fail(error, "texture image exceeds decoded RGBA budget (maximum 64 MiB)");
    probe.width = static_cast<uint32_t>(width);
    probe.height = static_cast<uint32_t>(height);
    return true;
}

}  // namespace

bool texture_image_probe(const uint8_t* encoded, size_t encoded_size,
                         size_t decoded_budget, TextureImageProbe& probe,
                         std::string& error) {
    probe = {};
    error.clear();
    TextureImageProbe checked;
    if (!probe_impl(encoded, encoded_size, decoded_budget, checked, error)) return false;
    probe = checked;
    return true;
}

bool texture_image_decode(const uint8_t* encoded, size_t encoded_size,
                          size_t decoded_budget, DecodedTextureImage& image,
                          std::string& error) {
    image = {};
    error.clear();
    TextureImageProbe probe;
    if (!probe_impl(encoded, encoded_size, decoded_budget, probe, error)) return false;
    const size_t rgba_bytes = static_cast<size_t>(probe.width) * probe.height * 4u;
    try {
        DecodedTextureImage decoded;
        decoded.width = probe.width;
        decoded.height = probe.height;
        if (probe.format == TextureImageFormat::WebP) {
            decoded.rgba.resize(rgba_bytes);
            if (!WebPDecodeRGBAInto(encoded, encoded_size, decoded.rgba.data(),
                                     decoded.rgba.size(), static_cast<int>(probe.width * 4u)))
                return fail(error, "malformed WebP texture image payload");
        } else {
            int width = 0, height = 0, channels = 0;
            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
                stbi_load_from_memory(encoded, static_cast<int>(encoded_size),
                                      &width, &height, &channels, 4), &stbi_image_free);
            if (!pixels) return fail(error, "malformed PNG/JPEG texture image payload");
            if (width != static_cast<int>(probe.width) || height != static_cast<int>(probe.height))
                return fail(error, "texture dimensions changed during decode");
            decoded.rgba.assign(pixels.get(), pixels.get() + rgba_bytes);
        }
        image = std::move(decoded);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "texture image allocation failed");
    }
}
