#include "pgxp.h"
#include "pgxp_hooks.h"
#include <cstdio>
#include <cstring>

extern "C" int gte_geometry_correction_lookup(uint32_t,int32_t*,int32_t*) {return 0;}
extern "C" int gte_geometry_correction_lookup_probe(uint32_t,int32_t*,int32_t*) {return 0;}
static int failures=0;
#define CHECK(c) do {if(!(c)){std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
constexpr uint32_t Address=0x80001000u,Other=0x80002000u,Packed=0x000C0008u;
static PGXPWordShadow capture(uint32_t address,uint32_t packed) {
    PGXPWordShadow result{};
    CHECK(pgxp_capture_word_shadow(address,packed,&result));
    return result;
}
static void seed(uint32_t address,uint32_t packed,int32_t x) {
    pgxp_gte_push_sxy(x,(12<<16)+32768,300,packed);
    pgxp_store_gte_reg(address,15);
}
static void check_attributes(const PGXPWordShadow& a,const PGXPWordShadow& b) {
    CHECK(a.valid==b.valid && a.value==b.value && a.address==b.address);
    CHECK(a.x16==b.x16 && a.y16==b.y16 && a.z==b.z && a.flags==b.flags);
    CHECK(!std::memcmp(&a.projection,&b.projection,sizeof a.projection));
}
int main() {
    pgxp_set_enabled(1);pgxp_set_cpu_mode(1);pgxp_set_projection_tracking(1);
    seed(Address,Packed,(8<<16)+16384);
    PGXPProjection projection{12.5f,18.25f,2.0f,0.125f};
    pgxp_gte_set_projection(&projection);pgxp_store_gte_reg(Address,15);
    const auto original=capture(Address,Packed);
    CHECK(original.valid && (original.flags&8u));
    CHECK(!pgxp_restore_word_shadow(Address,Packed,&original)); // sandbox only
    PGXPWordShadow ignored{};
    CHECK(!pgxp_capture_word_shadow(0x1F801814u,0,&ignored)); // no MMIO
    CHECK(!pgxp_capture_word_shadow(Address+1,Packed,&ignored));
    CHECK(!pgxp_capture_word_shadow(0xC0001000u,Packed,&ignored));

    pgxp_checkpoint_begin();
    pgxp_invalidate_all(); // raw entry RAM restoration invalidates live shadows
    seed(Address,Packed,(8<<16)+49152);
    const auto changed=capture(Address,Packed);
    CHECK(!pgxp_restore_word_shadow(Address,Packed+1,&original));
    check_attributes(changed,capture(Address,Packed)); // refused restore is atomic
    CHECK(!pgxp_restore_word_shadow(Other,Packed,&original)); // bound address
    CHECK(pgxp_restore_word_shadow(0xA0201000u,Packed,&original)); // retail mirror
    check_attributes(original,capture(Address,Packed));
    CHECK(capture(Address,Packed).source_generation!=original.source_generation);
    pgxp_checkpoint_rollback();
    check_attributes(original,capture(Address,Packed));
    CHECK(capture(Address,Packed).source_generation==original.source_generation);

    // CPU-derived coordinate flags must survive; XYZ reconstruction loses them.
    // 100.9 * 2 propagates as 201.8 while the native MULT returns 200.
    // Storing that scalar marks the coordinate as derived, rather than merely
    // offsetting a packed word (which intentionally retains its original flags).
    pgxp_gte_push_sxy((100<<16)+0xE666,(12<<16)+32768,300,0x000C0064u);
    pgxp_store_gte_reg(Address,15);
    psx_pgxp_load(nullptr,0x84280000u,Address,100); // lh t0,0(at)
    psx_pgxp_alu(nullptr,0x24190002u,2,0,2); // addiu t9,zero,2
    psx_pgxp_muldiv(nullptr,0x01190018u,0,200,100,2); // mult t0,t9
    psx_pgxp_alu(nullptr,0x00006012u,200,200,0); // mflo t4
    psx_pgxp_store(nullptr,0xAC2C0000u,Address,200);
    constexpr uint32_t DerivedPacked=200;
    const auto derived=capture(Address,DerivedPacked);
    CHECK(derived.valid && (derived.flags&0x30u));
    pgxp_checkpoint_begin();pgxp_invalidate_all();
    CHECK(pgxp_restore_word_shadow(Address,DerivedPacked,&derived));
    check_attributes(derived,capture(Address,DerivedPacked));
    pgxp_checkpoint_rollback();

    const auto absent=capture(Other,Packed);
    CHECK(!absent.valid);
    pgxp_checkpoint_begin();seed(Other,Packed,8<<16);
    CHECK(capture(Other,Packed).valid);
    CHECK(pgxp_restore_word_shadow(Other,Packed,&absent));
    CHECK(!capture(Other,Packed).valid);
    pgxp_checkpoint_rollback();CHECK(!capture(Other,Packed).valid);

    // Both live and absent receipts belong to one generation/timeline.
    pgxp_invalidate_all();seed(Address,DerivedPacked,12<<16);
    const auto latest=capture(Address,DerivedPacked);
    pgxp_checkpoint_begin();
    CHECK(!pgxp_restore_word_shadow(Address,DerivedPacked,&derived));
    CHECK(!pgxp_restore_word_shadow(Other,Packed,&absent));
    check_attributes(latest,capture(Address,DerivedPacked));
    pgxp_checkpoint_rollback();

    // A stale/value-mismatched slot is captured as absence, never promoted.
    CHECK(!capture(Address,Packed).valid);
    const uint32_t Scratch=0x1F800080u;
    seed(Scratch,Packed,8<<16);
    const auto scratch=capture(Scratch,Packed);
    pgxp_checkpoint_begin();pgxp_invalidate_all();
    CHECK(pgxp_restore_word_shadow(Scratch,Packed,&scratch));
    check_attributes(scratch,capture(Scratch,Packed));
    pgxp_checkpoint_rollback();
    std::printf("PGXP packet shadow checks: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
