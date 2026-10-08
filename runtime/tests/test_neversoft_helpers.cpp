// Focused host-only checks: no game launch, disc scan or renderer required.
#include "mod_named_wad.hpp"
#include "mod_neversoft_renderer.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include <vector>

struct TestLayout {
    static constexpr const char* Plugin = "test.draw", *PrecisionPlugin = "test.pgxp", *Package = "test.package";
    static constexpr const char* PrecisionCounter = "test.precision", *FogCounter = "test.fog", *ArenaCounter = "test.arena";
    static constexpr const char* PacketCounter = "test.packet", *FullCounter = "test.full", *PeakCounter = "test.peak";
    static constexpr uint32_t BufferA = 0x800A707C, BufferB = BufferA + 0x78;
    static constexpr uint32_t Current = 0x800C247C, Cursor = 0x800C2484, Limit = 0x800C2034;
    static constexpr uint32_t AllocA = 0x8006BDD8, AllocB = 0x8006BDF0, AllocWord = 0x0C01C0CB;
    static constexpr uint32_t FogPc = 0x80081178, SwapPc = 0x8006BE18, LimitPc = 0x800314E4;
    static constexpr uint32_t SubmitPc = 0x8006BE68, PrecisionPc = 0x80089A98;
    static constexpr PSXNeversoftPacketLengthSite PacketLengthSites[] = {
        {0x800891C0, 0x000A5582}, {0x8008A128, 0x00129582},
    };
};
using R = PSXNeversoftRenderer<TestLayout>;
static bool loaded = false, reject_packet_code = false;
static uint32_t a = 0x800D0000, b = 0x800E7000, cursor = 0, limit = 0;
static unsigned registered = 0;
extern "C" {
int psx_mod_register_activation_plugin(const char*, PSXModActivationCallback) { return 1; }
int psx_mod_register_function_entry_plugin(const char*, uint32_t, PSXModFunctionEntryCallback) { return 1; }
int psx_mod_register_function_filter_plugin(const char*, uint32_t, PSXModFunctionFilterCallback) { return 1; }
int psx_mod_register_instruction_plugin(const char*, uint32_t, uint32_t, PSXModFunctionEntryCallback) { ++registered; return 1; }
int pgxp_store_flagged_gte_sxy(uint32_t, uint8_t, uint32_t, uint32_t) { return 0; }
int psx_mod_option_value(const char*, const char*, const char*, char*, uint32_t) { return 0; }
uint32_t psx_mod_alloc_gpu_dma_memory(uint32_t bytes, uint32_t alignment) {
    assert(bytes == 2 * R::ArenaSize && alignment == 8); return 0x80800000;
}
uint32_t psx_mod_read_word(uint32_t address) {
    if (address == TestLayout::BufferA + 0x74) return a;
    if (address == TestLayout::BufferB + 0x74) return b;
    if (address == TestLayout::Current) return TestLayout::BufferA;
    if (address == TestLayout::Cursor) return cursor;
    if (!loaded) return 0;
    if (address == TestLayout::AllocA || address == TestLayout::AllocB) return TestLayout::AllocWord;
    if (address == TestLayout::LimitPc + 4) return 0x34846F00;
    if (address == TestLayout::FogPc) return 0x00803821;
    for (const auto& site : TestLayout::PacketLengthSites)
        if (address == site.address) return reject_packet_code ? 0 : site.word;
    return 0;
}
void psx_mod_write_word(uint32_t address, uint32_t value) {
    if (address == TestLayout::BufferA + 0x74) a = value;
    else if (address == TestLayout::BufferB + 0x74) b = value;
    else if (address == TestLayout::Limit) limit = value;
    else assert(false);
}
void psx_mod_counter_add(const char*, uint32_t) {}
}
int main() {
    PSXNamedWadIndex index;
    std::vector<uint8_t> hed = {'a',0,0,0, 0,0,0,0, 1,0,0,0, 0xff};
    assert(index.assign(hed.data(), uint32_t(hed.size()), 2048));
    assert(index.matches(0,2048,1));
    assert(!index.matches(0,1,1) && !index.matches(0,2048,2) && !index.matches(2048,2048,1));
    assert(!index.assign(hed.data(), uint32_t(hed.size()), 1));
    assert(!index.matches(0,2048,1)); // failed assignment clears prior catalog
    assert(!index.assign(hed.data(),12,2048)); // absent sentinel
    hed[4] = 1;
    assert(!index.assign(hed.data(),13,4096)); // unaligned offset
    hed[4] = 0; hed[8] = hed[9] = hed[10] = hed[11] = 0xff;
    assert(!index.assign(hed.data(),13,UINT32_MAX)); // padded-size overflow
    hed[8] = 1; hed[9] = hed[10] = hed[11] = 0;
    hed.pop_back();
    hed.insert(hed.end(), {'b',0,0,0, 0,0,0,0, 2,0,0,0, 0xff});
    assert(!index.assign(hed.data(),25,2048)); // conflicting duplicate
    hed[20] = 1;
    assert(index.assign(hed.data(),25,2048)); // aliases with identical extent
    R::register_plugins(); assert(registered == 3);
    R::activate(); R::swap(nullptr,0);
    assert(a == 0x800D0000 && b == 0x800E7000);
    loaded = true; reject_packet_code = true; R::swap(nullptr,0);
    assert(a == 0x800D0000 && b == 0x800E7000);
    reject_packet_code = false; R::swap(nullptr,0);
    assert(a == R::arena && b == R::arena + R::ArenaSize);
    for (unsigned reg : {10u,18u}) for (unsigned length = 0; length < 256; ++length)
        for (uint32_t address : {0x00800000u,0x00860000u,0x008BFFFCu}) {
            CPUState cpu{};
            for (unsigned i=0; i<32; ++i) cpu.gpr[i] = 0x13570000 + i;
            cpu.pc = 0x800891C0;
            cpu.gpr[reg] = length << 24 | address;
            R::packet_length(&cpu,reg);
            assert((cpu.gpr[reg] >> 22) + 4 == 4 * (length + 1));
            assert(cpu.pc == 0x800891C0);
            for (unsigned i=0; i<32; ++i) if (i != reg) assert(cpu.gpr[i] == 0x13570000 + i);
        }
    CPUState cpu{}; cpu.gpr[10] = 0x090D0000;
    R::packet_length_t2(&cpu,0); assert(cpu.gpr[10] == 0x090D0000);
    assert(R::limit(&cpu,0) == 1 && limit == 0x0085FF00 && cpu.gpr[2] == limit);
    cursor = R::arena + 1232; R::submit(nullptr,0); assert(R::peak == 1232);
    for (int32_t range : {1,128,2048,4096,8192,8193,16379}) {
        cpu.gpr[5] = 0; cpu.gpr[6] = range; R::fog(&cpu,0);
        assert(cpu.gpr[5] + cpu.gpr[6] <= R::MaxFar);
        assert(cpu.gpr[5] + cpu.gpr[6] >= uint32_t(range));
    }
    cpu.gpr[5] = 6000; cpu.gpr[6] = 2048; R::fog(&cpu,0);
    assert(cpu.gpr[5] == 12000 && cpu.gpr[6] == 4096);
    R::arena = 0; cpu.gpr[10] = 0x09805978;
    R::packet_length_t2(&cpu,0); assert(cpu.gpr[10] == 0x09805978);
    std::puts("PASS: shared named archive bounds, packet ABI, code guards, primitive capacity and fog ceiling");
}
