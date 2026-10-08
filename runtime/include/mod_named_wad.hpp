#pragma once
// Neversoft's HED format: NUL-terminated name, word padding, offset/size,
// terminated by 0xff. Only the archive format is shared; title lookup and
// request/completion contracts remain with the game.
#include <cstdint>
#include <unordered_map>

class PSXNamedWadIndex {
    std::unordered_map<uint32_t, uint32_t> extents;
    static uint32_t word(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
               uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
public:
    bool assign(const uint8_t* hed, uint32_t size, uint32_t wad_size) {
        extents.clear();
        if (!hed) return false;
        uint32_t cursor = 0;
        while (cursor < size && hed[cursor] != 0xff) {
            uint32_t end = cursor;
            while (end < size && hed[end]) ++end;
            if (end == size || end == cursor) { extents.clear(); return false; }
            const uint64_t fields = (uint64_t(end) + 4) & ~uint64_t(3);
            if (fields > size || size - fields < 8) { extents.clear(); return false; }
            const uint32_t offset = word(hed + fields), bytes = word(hed + fields + 4);
            const uint64_t padded = (uint64_t(bytes) + 2047) & ~uint64_t(2047);
            if ((offset & 2047) || offset > wad_size || padded > wad_size - offset) {
                extents.clear(); return false;
            }
            const auto found = extents.find(offset);
            if (found != extents.end() && found->second != bytes) {
                extents.clear(); return false;
            }
            extents[offset] = bytes;
            cursor = uint32_t(fields + 8);
        }
        if (cursor >= size || hed[cursor] != 0xff || extents.empty()) {
            extents.clear(); return false;
        }
        return true;
    }
    bool matches(uint32_t offset, uint32_t padded_bytes, uint32_t sectors) const {
        const auto found = extents.find(offset);
        return found != extents.end() && padded_bytes != 0 &&
               uint64_t(padded_bytes) == ((uint64_t(found->second) + 2047) & ~uint64_t(2047)) &&
               sectors == padded_bytes / 2048;
    }
};
