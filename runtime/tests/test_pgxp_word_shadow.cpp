#include "pgxp.h"
#include "pgxp_hooks.h"
#include "mod_memory.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

extern "C" int gte_geometry_correction_lookup(uint32_t,int32_t*,int32_t*) {return 0;}
extern "C" int gte_geometry_correction_lookup_probe(uint32_t,int32_t*,int32_t*) {return 0;}
static uint32_t gpu_bytes = 0;
extern "C" int psx_mod_gpu_dma_memory_contains(uint32_t address, uint32_t bytes) {
    return psx_mod_gpu_dma_aperture_offset_for(address, bytes, gpu_bytes, nullptr);
}
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
    CHECK(!pgxp_restore_relocated_word_shadow(Other,Packed,&original));
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

    // Relocating a packet changes only the explicitly owned destination.
    // Preserve projection and the previous destination across sandbox rollback.
    seed(Other,Packed,(9<<16)+32768);
    const auto old_destination=capture(Other,Packed);
    pgxp_checkpoint_begin();pgxp_invalidate_all();
    CHECK(!pgxp_restore_word_shadow(Other,Packed,&original));
    CHECK(pgxp_restore_relocated_word_shadow(0xA0202000u,Packed,&original));
    auto relocated=original;relocated.address=old_destination.address;
    check_attributes(relocated,capture(Other,Packed));
    CHECK(capture(Other,Packed).source_generation!=original.source_generation);
    CHECK(!capture(Address,Packed).valid); // no source writes/rebinding
    const auto before_rejection=capture(Other,Packed);
    CHECK(!pgxp_restore_relocated_word_shadow(Other,Packed+1,&original));
    CHECK(!pgxp_restore_relocated_word_shadow(Other+1,Packed,&original));
    CHECK(!pgxp_restore_relocated_word_shadow(0x1F801814u,Packed,&original));
    CHECK(!pgxp_restore_relocated_word_shadow(0xC0002000u,Packed,&original));
    CHECK(!pgxp_restore_relocated_word_shadow(Other,Packed,nullptr));
    for (const uint32_t invalid_source : {0x1F801814u, 0x80001000u, 0x1001u}) {
        auto invalid=original;invalid.address=invalid_source;
        CHECK(!pgxp_restore_relocated_word_shadow(Other,Packed,&invalid));
    }
    auto invalid=original;invalid.valid=2;
    CHECK(!pgxp_restore_relocated_word_shadow(Other,Packed,&invalid));
    check_attributes(before_rejection,capture(Other,Packed));
    pgxp_checkpoint_rollback();
    check_attributes(original,capture(Address,Packed));
    check_attributes(old_destination,capture(Other,Packed));
    pgxp_invalidate_all();

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
    CHECK(pgxp_restore_relocated_word_shadow(Other,DerivedPacked,&derived));
    auto relocated_derived=derived;relocated_derived.address=Other&0x1FFFFFFFu;
    check_attributes(relocated_derived,capture(Other,DerivedPacked));
    pgxp_checkpoint_rollback();

    const auto absent=capture(Other,Packed);
    CHECK(!absent.valid);
    pgxp_checkpoint_begin();seed(Other,Packed,8<<16);
    CHECK(capture(Other,Packed).valid);
    CHECK(pgxp_restore_word_shadow(Other,Packed,&absent));
    CHECK(!capture(Other,Packed).valid);
    pgxp_checkpoint_rollback();CHECK(!capture(Other,Packed).valid);

    // Absence is transferable too: erase a destination shadow inside replay,
    // then recover the original destination on rollback.
    pgxp_checkpoint_begin();
    CHECK(pgxp_restore_relocated_word_shadow(Address,Packed,&absent));
    CHECK(!capture(Address,DerivedPacked).valid);
    CHECK(!capture(Address,Packed).valid);
    pgxp_checkpoint_rollback();check_attributes(derived,capture(Address,DerivedPacked));

    // Both live and absent receipts belong to one generation/timeline.
    pgxp_invalidate_all();seed(Address,DerivedPacked,12<<16);
    const auto latest=capture(Address,DerivedPacked);
    pgxp_checkpoint_begin();
    CHECK(!pgxp_restore_word_shadow(Address,DerivedPacked,&derived));
    CHECK(!pgxp_restore_word_shadow(Other,Packed,&absent));
    CHECK(!pgxp_restore_relocated_word_shadow(Other,DerivedPacked,&derived));
    CHECK(!pgxp_restore_relocated_word_shadow(Address,Packed,&absent));
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
    CHECK(pgxp_restore_relocated_word_shadow(Address,Packed,&scratch));
    auto relocated_scratch=scratch;relocated_scratch.address=Address&0x1FFFFFFFu;
    check_attributes(relocated_scratch,capture(Address,Packed));
    pgxp_checkpoint_rollback();
    // Expanded primitive storage must retain the exact same XYZ/depth and
    // projection as RAM, without aliasing the retail RAM word under its tag.
    const uint32_t Arena = PSX_MOD_GPU_DMA_GUEST_BASE + 0x1000u;
    const uint32_t ArenaOther = Arena + 0x1000u;
    gpu_bytes = 0x4000u;
    pgxp_invalidate_all();
    seed(Address, Packed, (8<<16)+16384);
    seed(Arena, Packed, (8<<16)+32768);
    const auto ram = capture(Address, Packed);
    const auto arena = capture(Arena, Packed);
    CHECK(arena.valid && arena.address == 0x00801000u);
    CHECK(arena.x16 != ram.x16);
    check_attributes(arena, capture(Arena & 0x1FFFFFFFu, Packed));
    check_attributes(arena, capture(Arena | 0x20000000u, Packed));
    uint16_t depth = 0;
    CHECK(pgxp_load_precise_word(Arena, Packed, nullptr, nullptr, &depth));
    CHECK(depth == 300);
    CHECK(!pgxp_load_precise_word(Arena, Packed + 1, nullptr, nullptr, &depth));
    PGXPWordShadow refused{};
    CHECK(!pgxp_capture_word_shadow(PSX_MOD_GPU_DMA_GUEST_BASE + gpu_bytes, Packed, &refused));
    CHECK(!pgxp_capture_word_shadow(0xBF801000u, Packed, &refused));
    CHECK(!pgxp_capture_word_shadow(0xC0801000u, Packed, &refused));
    const auto empty_arena = capture(ArenaOther, Packed);
    CHECK(!empty_arena.valid);
    pgxp_checkpoint_begin();
    seed(Arena, Packed, (8<<16)+49152);
    // This page is allocated for the first time inside the checkpoint.
    CHECK(pgxp_restore_relocated_word_shadow(ArenaOther, Packed, &arena));
    auto relocated_arena = arena; relocated_arena.address = 0x00802000u;
    check_attributes(relocated_arena, capture(ArenaOther, Packed));
    check_attributes(ram, capture(Address, Packed));
    pgxp_checkpoint_rollback();
    check_attributes(arena, capture(Arena, Packed));
    check_attributes(empty_arena, capture(ArenaOther, Packed));
    check_attributes(ram, capture(Address, Packed));
    pgxp_checkpoint_begin(); pgxp_invalidate_all();
    CHECK(pgxp_restore_word_shadow(Arena, Packed, &arena));
    check_attributes(arena, capture(Arena, Packed));
    pgxp_checkpoint_rollback();
    check_attributes(arena, capture(Arena, Packed));
    pgxp_invalidate_all();
    CHECK(!capture(Arena, Packed).valid);
    gpu_bytes = 0; // pages retained on the host are not guest allocations
    CHECK(!pgxp_capture_word_shadow(Arena, Packed, &refused));
    std::printf("PGXP packet shadow checks: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
