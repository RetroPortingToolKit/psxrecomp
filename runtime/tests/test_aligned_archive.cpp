#undef NDEBUG
#include "mod_aligned_archive.hpp"
#include <cassert>
#include <vector>
static void put(std::vector<uint8_t>& b, unsigned at, uint32_t n) {
    for (int i = 0; i < 4; ++i) b[at+i] = uint8_t(n >> (8*i));
}
int main() {
    std::vector<uint8_t> bytes(8192);
    put(bytes,0,1); put(bytes,4,2);
    put(bytes,8,1500); put(bytes,12,2048);
    put(bytes,16,2049); put(bytes,20,4096);
    PSXAlignedArchiveIndex index;
    assert(index.assign(bytes.data(),unsigned(bytes.size())));
    assert(index.matches(0,2048) && index.matches(2048,6144));
    assert(index.matches(4096,4096) && index.matches(0,8192));
    assert(!index.matches(2048,2049) && !index.matches(4097,4095));
    assert(!index.matches(UINT32_MAX-5,16) && !index.matches(0,0));
    put(bytes,20,UINT32_MAX);
    assert(!index.assign(bytes.data(),unsigned(bytes.size())));
    assert(!index.matches(0,2048)); // stale catalogue cannot survive a failure.
    put(bytes,20,4096); bytes[100] = 1;
    assert(!index.assign(bytes.data(),unsigned(bytes.size())));
    bytes[100] = 0; put(bytes,16,UINT32_MAX);
    assert(!index.assign(bytes.data(),unsigned(bytes.size())));
    assert(!index.assign(nullptr,8192));
}
