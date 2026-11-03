#undef NDEBUG
#include "mod_resident_sector.hpp"
#include <cassert>
#include <cstring>
int main() {
    uint8_t data[2048];
    for (unsigned i=0; i<2048; ++i) data[i] = uint8_t(i * 17);
    PSXResidentMode2Sector sector;
    assert(!sector.take_words(3));
    assert(sector.assign(4500 + 75 * 23 + 14 - 150,data,true));
    auto* h = sector.take_words(3);
    assert(h && h[0] == 1 && h[1] == 0x23 && h[2] == 0x14 && h[3] == 2);
    assert(h[6] == 0x89 && h[10] == 0x89);
    assert(!sector.take_words(UINT32_MAX) && sector.consumed() == 12);
    assert(!sector.take_words(0) && sector.consumed() == 12);
    assert(std::memcmp(sector.take_words(17),data,68) == 0);
    assert(std::memcmp(sector.take_words(495),data+68,1980) == 0);
    assert(sector.consumed() == 2060);
    assert(sector.take_words(70)); // optional uninterpreted EDC/ECC tail
    assert(!sector.take_words(1));
    assert(!sector.assign(UINT32_MAX,data) && !sector.take_words(1));
    assert(!sector.assign(449850,data));
    assert(!sector.assign(0,nullptr));
    assert(sector.assign(0,data)); h = sector.take_words(3);
    assert(h[0] == 0 && h[1] == 2 && h[2] == 0 && h[6] == 8 && h[10] == 8);
}
