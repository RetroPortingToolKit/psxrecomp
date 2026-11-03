#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

// Version/count followed by {stored size, aligned size}; no title addresses.
// Header and every member must exactly cover the supplied archive. The header
// is itself a legal request; following requests must span whole members.
class PSXAlignedArchiveIndex {
    struct Span { uint32_t begin, end; };
    std::vector<Span> spans;
    static uint32_t word(const uint8_t* p) {
        return uint32_t(p[0]) | uint32_t(p[1]) << 8 |
               uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    }
public:
    bool assign(const uint8_t* data, uint32_t size,
                uint32_t alignment = 2048, uint32_t version = 1) {
        spans.clear();
        if (!data || alignment < 16 || size < alignment ||
            word(data) != version) return false;
        const uint32_t count = word(data + 4);
        if (!count || count > (alignment - 8) / 8) return false;
        for (uint32_t i = 8 + count * 8; i < alignment; ++i)
            if (data[i]) return false;
        std::vector<Span> parsed{{0, alignment}};
        uint64_t cursor = alignment;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t stored = word(data + 8 + i * 8);
            const uint32_t padded = word(data + 12 + i * 8);
            const uint64_t expected = (uint64_t(stored) + alignment - 1) /
                                       alignment * alignment;
            if (!stored || padded != expected || cursor + padded > size)
                return false;
            parsed.push_back({uint32_t(cursor), uint32_t(cursor + padded)});
            cursor += padded;
        }
        if (cursor != size) return false;
        spans.swap(parsed);
        return true;
    }
    bool matches(uint32_t offset, uint32_t bytes) const {
        const uint64_t end = uint64_t(offset) + bytes;
        if (!bytes || end > UINT32_MAX) return false;
        for (size_t i = 0; i < spans.size(); ++i) {
            if (spans[i].begin != offset) continue;
            for (; i < spans.size(); ++i) {
                if (spans[i].end == end) return true;
                if (spans[i].end > end) return false;
            }
            break;
        }
        return false;
    }
};
