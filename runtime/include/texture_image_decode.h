#ifndef PSX_TEXTURE_IMAGE_DECODE_H
#define PSX_TEXTURE_IMAGE_DECODE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

enum class TextureImageFormat { Png, Jpeg, WebP };

struct TextureImageProbe {
    uint32_t width = 0;
    uint32_t height = 0;
    TextureImageFormat format = TextureImageFormat::Png;
};

struct DecodedTextureImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
};

constexpr size_t TEXTURE_IMAGE_MAX_ENCODED_BYTES = 64u * 1024u * 1024u;
constexpr size_t TEXTURE_IMAGE_DEFAULT_DECODED_BUDGET = 64u * 1024u * 1024u;
constexpr uint32_t TEXTURE_IMAGE_MAX_DIMENSION = 8192;

// Encoded bytes are supplied by the caller: these helpers perform no I/O.
// Limits are checked before allocating decoded pixels. RGBA alpha is retained
// verbatim; the texture renderer separately classifies PSX transparency.
bool texture_image_probe(const uint8_t* encoded, size_t encoded_size,
                         size_t decoded_budget, TextureImageProbe& probe,
                         std::string& error);
bool texture_image_decode(const uint8_t* encoded, size_t encoded_size,
                          size_t decoded_budget, DecodedTextureImage& image,
                          std::string& error);

#endif
