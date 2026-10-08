#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "../third_party/stb_image.h"
#include "texture_image_decode.h"

#include <cstdio>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    int failures = 0;
    const auto check = [&](bool okay, const char* message) {
        if (!okay) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
    };
    const auto read = [&](const char* name) {
        std::ifstream input(std::string(argv[1]) + "/" + name, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
    };
    const uint8_t expected[] = {255, 0, 17, 0, 3, 127, 241, 1,
                               63, 19, 211, 128, 23, 47, 71, 254};
    std::string error;
    TextureImageProbe probe;
    DecodedTextureImage image;
    for (const char* name : {"rgba.png", "rgba.webp"}) {
        const auto bytes = read(name);
        check(texture_image_probe(bytes.data(), bytes.size(), 16, probe, error), "bounded probe succeeds");
        check(probe.width == 4 && probe.height == 1, "PNG/WebP dimensions");
        check(texture_image_decode(bytes.data(), bytes.size(), 16, image, error), "PNG/WebP decode succeeds");
        check(image.rgba == std::vector<uint8_t>(expected, expected + sizeof(expected)), "raw RGBA including alpha retained");
        check(!texture_image_decode(bytes.data(), bytes.size(), 15, image, error) && image.rgba.empty() && !error.empty(), "budget failure clears output");
        check(!texture_image_decode(bytes.data(), bytes.size() / 2, 16, image, error), "truncated payload rejected");
    }
    for (const char* name : {"opaque.jpg", "progressive.jpg", "opaque-lossy.webp"}) {
        const auto bytes = read(name);
        check(texture_image_decode(bytes.data(), bytes.size(), 1024, image, error), "lossy image decode succeeds");
        check(image.width == 16 && image.height == 16 && image.rgba.size() == 1024, "lossy image dimensions");
        for (size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
            check(image.rgba[i] >= 26 && image.rgba[i] <= 34 && image.rgba[i+1] >= 76 &&
                  image.rgba[i+1] <= 84 && image.rgba[i+2] >= 116 && image.rgba[i+2] <= 124 &&
                  image.rgba[i+3] == 255, "lossy RGB tolerance and opaque alpha");
        }
    }
    auto png = read("rgba.png");
    png[16] = 0; png[17] = 0; png[18] = 0x20; png[19] = 1;
    check(!texture_image_probe(png.data(), png.size(), TEXTURE_IMAGE_DEFAULT_DECODED_BUDGET, probe, error), "8193-pixel dimension rejected before decode");
    png[18] = 0x10; png[19] = 0; png[22] = 0x10; png[23] = 1;
    check(!texture_image_probe(png.data(), png.size(), TEXTURE_IMAGE_DEFAULT_DECODED_BUDGET, probe, error), "oversized RGBA rejected before decode");
    const uint8_t junk[] = {0, 1, 2, 3};
    check(!texture_image_probe(junk, sizeof(junk), 64, probe, error), "unsupported bytes rejected");
    check(!texture_image_probe(junk, TEXTURE_IMAGE_MAX_ENCODED_BYTES + 1, 64, probe, error), "encoded bound checked before input read");
    const auto animation = read("animated.webp");
    check(!texture_image_probe(animation.data(), animation.size(), 64, probe, error) && error.find("animated") != std::string::npos, "animated WebP explicitly rejected");
    check(!texture_image_probe(nullptr, 0, 64, probe, error), "empty input rejected");
    if (failures) return 1;
    std::puts("PASS: PNG/JPEG/WebP raw RGBA, dimensions, budgets, malformed and animated input");
    return 0;
}
