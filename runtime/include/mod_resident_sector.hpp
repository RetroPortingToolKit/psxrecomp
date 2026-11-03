#pragma once
#include <array>
#include <cstdint>
#include <cstring>

// A scoped PsyQ Mode 2/Form 1 data view, starting after the 12-byte sync.
// Only MSF, mode and data submode are synthesized; EDC/ECC are not reproduced.
// Consumers must guard that their guest callback ignores those fields. The
// title still owns request selection, guest DMA, callback and queue scheduling.
class PSXResidentMode2Sector {
    std::array<uint8_t, 2340> bytes_{};
    uint32_t cursor_ = 0;
    bool valid_ = false;
    static uint8_t bcd(uint32_t n) { return uint8_t((n / 10 << 4) | n % 10); }
public:
    bool assign(uint32_t lba, const uint8_t* data, bool last = false) {
        cursor_ = 0; valid_ = false;
        // MSF has two decimal minute digits. Avoid wrapping on invalid LBAs.
        if (!data || lba >= 450000u - 150u) return false;
        bytes_.fill(0);
        const uint32_t msf = lba + 150;
        bytes_[0] = bcd(msf / 4500);
        bytes_[1] = bcd(msf / 75 % 60);
        bytes_[2] = bcd(msf % 75); bytes_[3] = 2;
        bytes_[6] = bytes_[10] = last ? 0x89 : 0x08;
        std::memcpy(bytes_.data() + 12, data, 2048);
        valid_ = true; return true;
    }
    const uint8_t* take_words(uint32_t words) {
        if (!valid_ || !words || words > (bytes_.size() - cursor_) / 4)
            return nullptr;
        const auto* p = bytes_.data() + cursor_;
        cursor_ += words * 4;
        return p;
    }
    uint32_t consumed() const { return cursor_; }
};
