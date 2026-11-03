#include "render_pass_ot_relocation.hpp"
#include <cstdio>

static int failures=0;
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
static void put(std::vector<uint8_t>& ram,uint32_t address,uint32_t value) {
    std::memcpy(ram.data()+address,&value,4);
}
int main() {
    std::vector<uint8_t> ram(0x200000);
    constexpr uint32_t OT=0x1000,Prefix=0x1100,Core=0x1200,Tail=0x1400;
    put(ram,OT,Prefix);put(ram,Prefix,0x02ffffffu);
    put(ram,Prefix+4,0x64000000);put(ram,Prefix+8,0x000a000b);
    PSXOTRelocation journal;
    CHECK(journal.begin(ram.data(),ram.size(),OT));
    put(ram,OT,Core);put(ram,Core,0x02000000u|Prefix);
    put(ram,Core+4,0x20000000);put(ram,Core+8,0x000c000d);
    CHECK(journal.end(ram.data(),ram.size(),0x1300));
    const auto core=ram;
    put(ram,OT,Tail);put(ram,Tail,0x02000000u|Core);
    put(ram,Tail+4,0x64000000);put(ram,Tail+8,0x00100020);
    CHECK(journal.prepare(ram.data(),ram.size()));
    ram=core;put(ram,Core+8,0x000d000e); // interpolated core coordinates
    CHECK(journal.matches(ram.data(),ram.size(),OT,0x1300));
    CHECK(!journal.matches(ram.data(),ram.size(),OT,0x1304));
    CHECK(!journal.matches(ram.data(),ram.size(),OT+4,0x1300));
    put(ram,Core,0x02000000u|Tail);
    CHECK(!journal.matches(ram.data(),ram.size(),OT,0x1300));
    ram=core;put(ram,Core+8,0x000d000e);journal.apply(ram.data());
    CHECK(PSXOTRelocation::word(ram.data(),OT)==Tail);
    CHECK(PSXOTRelocation::word(ram.data(),Core+8)==0x000d000e);
    CHECK(PSXOTRelocation::word(ram.data(),Tail+8)==0x00100020);

    // Tail must never overwrite interpolated output, or grow a core packet.
    ram=core;put(ram,Core+8,0x10000000);
    CHECK(!journal.prepare(ram.data(),ram.size()));
    ram=core;put(ram,Core,0x03000000u|Prefix);
    CHECK(!journal.prepare(ram.data(),ram.size()));
    ram=core;put(ram,OT,0x300000); // out-of-RAM link
    CHECK(!journal.prepare(ram.data(),ram.size()));
    ram=core;put(ram,OT,OT); // cycle, bounded traversal
    CHECK(!journal.prepare(ram.data(),ram.size()));
    ram=core;
    CHECK(journal.begin(ram.data(),ram.size(),OT));
    put(ram,Prefix+8,0x11112222); // in-place core reuse is unsupported
    CHECK(!journal.end(ram.data(),ram.size(),0x1300));
    ram=core;put(ram,Core,0x02000000u|(Core+4));
    put(ram,Core+4,0x00ffffffu); // linked header aliases a packet payload
    std::vector<PSXOTRelocation::Node> aliased;
    CHECK(!PSXOTRelocation::walk(ram.data(),ram.size(),OT,aliased));
    // Variable core packet layout: relocate a contiguous two-packet HUD tail
    // and splice its terminal onto the current OT head, preserving live core.
    ram.assign(0x200000,0);
    put(ram,OT,Prefix);put(ram,Prefix,0x02ffffffu);
    put(ram,Prefix+4,0x64000000);put(ram,Prefix+8,0x000a000b);
    PSXOTRelocation relocated;
    CHECK(relocated.begin(ram.data(),ram.size(),OT,0x80001200));
    put(ram,OT,Core);put(ram,Core,0x02000000u|Prefix);
    put(ram,Core+4,0x20000000);put(ram,Core+8,0x000c000d);
    put(ram,0x1800,0x80001300);
    CHECK(relocated.end(ram.data(),ram.size(),0x80001300));
    const auto recorded_core=ram;
    put(ram,0x1300,0x02000000u|Core);
    put(ram,0x1304,0x64000000);put(ram,0x1308,0x00100020);
    put(ram,0x130c,0x02001300u);
    put(ram,0x1310,0x60000000);put(ram,0x1314,0x00300040);
    put(ram,OT,0x130c);put(ram,0x1800,0x80001318);
    CHECK(relocated.prepare(ram.data(),ram.size()));
    ram=recorded_core;
    put(ram,OT,0x1220);put(ram,0x1220,0x02000000u|Prefix);
    put(ram,0x1224,0x20000000);put(ram,0x1228,0x000e000f);
    put(ram,0x1800,0x80001320);
    CHECK(relocated.prepare_splice(ram.data(),ram.size(),OT,0x80001320,0x1800));
    CHECK(relocated.shadow_destination(0x1308)==0x1328);
    relocated.apply_splice(ram.data());
    CHECK(PSXOTRelocation::word(ram.data(),OT)==0x132c);
    CHECK(PSXOTRelocation::word(ram.data(),0x1320)==(0x02000000u|0x1220));
    CHECK(PSXOTRelocation::word(ram.data(),0x132c)==0x02001320u);
    CHECK(PSXOTRelocation::word(ram.data(),0x1228)==0x000e000f);
    CHECK(PSXOTRelocation::word(ram.data(),0x1800)==0x80001338);
    CHECK(!relocated.prepare_splice(ram.data(),ram.size(),OT+4,0x80001320,0x1800));
    ram=recorded_core;put(ram,Prefix+8,0x00010002);
    CHECK(!relocated.prepare_splice(ram.data(),ram.size(),OT,0x80001320,0x1800));
    ram=recorded_core;
    CHECK(!relocated.prepare_splice(ram.data(),ram.size(),OT,0x80001200,0x1800));
    CHECK(!relocated.prepare_splice(ram.data(),ram.size(),OT,0x801ffffc,0x1800));
    ram=recorded_core;put(ram,0x1800,0x800012e0);
    CHECK(relocated.prepare_splice(ram.data(),ram.size(),OT,0x800012e0,0x1800));
    relocated.apply_splice(ram.data());
    CHECK(PSXOTRelocation::word(ram.data(),OT)==0x12ec);
    CHECK(PSXOTRelocation::word(ram.data(),0x12e0)==(0x02000000u|Core));
    // DrawMode's allocator reserves 16 bytes but DMA reads 8. Stale padding
    // must neither be interpreted as a tag nor copied onto live core output.
    ram=recorded_core;
    put(ram,0x1300,0x01000000u|Core);
    put(ram,0x1304,0xe1000220);
    put(ram,0x1308,0xffffffffu);put(ram,0x130c,0xaaaaaaaa);
    put(ram,0x1310,0x02001300u);
    put(ram,0x1314,0x64000000);put(ram,0x1318,0x00100020);
    put(ram,OT,0x1310);put(ram,0x1800,0x80001320);
    put(ram,0x1900,0x1234); // real tail advances an ordinary scratch counter
    CHECK(relocated.prepare(ram.data(),ram.size(),0x80001320));
    ram=recorded_core;put(ram,0x1800,0x80001340);
    put(ram,0x1900,0xabcd); // interpolation changed the temporary core value
    put(ram,0x1348,0x12345678);put(ram,0x134c,0x87654321);
    CHECK(relocated.prepare_splice(ram.data(),ram.size(),OT,0x80001340,0x1800));
    relocated.apply_splice(ram.data());
    CHECK(PSXOTRelocation::word(ram.data(),OT)==0x1350);
    CHECK(PSXOTRelocation::word(ram.data(),0x1340)==(0x01000000u|Core));
    CHECK(PSXOTRelocation::word(ram.data(),0x1348)==0x12345678);
    CHECK(PSXOTRelocation::word(ram.data(),0x134c)==0x87654321);
    CHECK(PSXOTRelocation::word(ram.data(),0x1800)==0x80001360);
    CHECK(PSXOTRelocation::word(ram.data(),0x1900)==0x1234);
    ram=recorded_core;put(ram,0x1800,0x80001340);
    put(ram,OT,0x18fc);put(ram,0x18fc,0x02000000u|Core);
    put(ram,0x1900,0x64000000);put(ram,0x1904,0x10002000);
    CHECK(!relocated.prepare_splice(ram.data(),ram.size(),OT,0x80001340,0x1800));
    ram=recorded_core;put(ram,0x1800,0x80001340);
    // A packet outside the actual producer interval is still refused.
    CHECK(relocated.prepare_splice(ram.data(),ram.size(),OT,0x801ffff0,0x1800)==false);
    // A middle-list insertion into a primitive header is unsupported even if
    // the complete DMA graph remains structurally valid.
    ram=recorded_core;
    put(ram,0x1300,0x02000000u|Prefix);
    put(ram,0x1304,0x64000000);put(ram,0x1308,0x00100020);
    put(ram,Core,0x02001300u);put(ram,0x1800,0x8000130c);
    CHECK(relocated.prepare(ram.data(),ram.size()));
    ram=recorded_core;put(ram,0x1800,0x80001320);
    CHECK(!relocated.prepare_splice(ram.data(),ram.size(),OT,0x80001320,0x1800));
    std::printf("OT relocation guards: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
