#include "pgxp.h"
#include "pgxp_hooks.h"
#include "mod_memory.h"
#include "cpu_state.h"
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
    // Neversoft subdivision unpacks/re-packs SXY and adds screen outcodes.
    // Reattach validated position/depth; exact projection may cross the
    // original integer boundary while the guest packet remains unchanged.
    pgxp_set_preserve_projection(1);
    pgxp_set_tolerance(-1.0f);
    const uint32_t Flagged = Packed | 0x80004000u;
    pgxp_gte_push_sxy((8<<16)-16384, (12<<16)+73728, 300, Packed);
    CHECK(pgxp_store_flagged_gte_sxy(Arena, 14, Packed, Flagged));
    int32_t precise_x=0, precise_y=0;
    CHECK(pgxp_get_precise_vertex(Arena, Flagged, 8, 12, &precise_x, &precise_y, &depth) == PGXP_SRC_DATAFLOW);
    CHECK(precise_x == (8<<16)-16384 && precise_y == (12<<16)+73728 && depth == 300);
    CHECK(pgxp_load_precise_word(Arena, Flagged, nullptr, nullptr, &depth) && depth == 300);
    const auto flagged = capture(Arena, Flagged);
    CHECK(!pgxp_store_flagged_gte_sxy(Arena, 14, Packed + 1, Flagged));
    CHECK(!pgxp_store_flagged_gte_sxy(Arena, 14, Packed, Flagged ^ 1));
    CHECK(!pgxp_store_flagged_gte_sxy(Arena + 1, 14, Packed, Flagged));
    CHECK(!pgxp_store_flagged_gte_sxy(0x1F801814u, 14, Packed, Flagged));
    CHECK(!pgxp_store_flagged_gte_sxy(Arena, 11, Packed, Flagged));
    check_attributes(flagged, capture(Arena, Flagged));
    pgxp_checkpoint_begin();
    CHECK(pgxp_store_flagged_gte_sxy(Arena, 14, Packed, Packed));
    pgxp_checkpoint_rollback();
    check_attributes(flagged, capture(Arena, Flagged));
    pgxp_invalidate_all();
    CHECK(!pgxp_store_flagged_gte_sxy(Arena, 14, Packed, Flagged));
    // A native packet producer captures the old source words before any guest
    // writes, then republishes the same receipts after aliases are overwritten.
    pgxp_set_source_tracking(1);
    seed(Address, Packed, (8 << 16) + 16384);
    CHECK(pgxp_tag_word_source(Address, Packed, 101));
    pgxp_gte_set_projection(&projection); pgxp_store_gte_reg(Address, 15);
    CHECK(pgxp_tag_word_source(Address, Packed, 101));
    seed(Other, Packed, (8 << 16) + 49152);
    CHECK(pgxp_tag_word_source(Other, Packed, 202));
    const uint32_t inputs[2] = {Address, Other}, words[2] = {Packed, Packed};
    const uint32_t swapped[2] = {Other, Address};
    PGXPNativeWord receipts[2]{};
    CHECK(pgxp_native_capture_words(inputs, words, receipts, 2));
    CHECK(receipts[0].source == 101 && receipts[1].source == 202);
    const auto first_native = receipts[0].word, second_native = receipts[1].word;
    pgxp_invalidate_word(Address); pgxp_invalidate_word(Other); // guest overwrites
    uint32_t native_pc = 0x80012340u, store_cap = 0;
    uint64_t native_frame = 77;
    const PGXPStoreRecord *stores = nullptr;
    pgxp_set_trace_sources(&native_pc, &native_frame);
    const auto stores_before = pgxp_store_ring(&stores, &store_cap);
    CHECK(pgxp_native_publish_words(swapped, receipts, 2));
    CHECK(pgxp_store_ring(&stores, &store_cap) == stores_before + 2);
    for (unsigned i = 0; i < 2; ++i) {
        const auto &record = stores[(stores_before + i) % store_cap];
        CHECK(record.op == PGXP_STORE_NATIVE_WORD && record.reg == 0xFFu);
        CHECK(record.pc == native_pc && record.frame == native_frame);
        CHECK(record.addr == swapped[i] && record.value == Packed);
        CHECK(record.src_live && record.src_flags == receipts[i].word.flags);
        CHECK(record.dst_flags == receipts[i].word.flags);
        CHECK(record.dst_x16 == receipts[i].word.x16 && record.dst_y16 == receipts[i].word.y16);
    }
    pgxp_set_trace_sources(nullptr, nullptr);
    auto expect_first = first_native; expect_first.address = Other & 0x1FFFFFFFu;
    auto expect_second = second_native; expect_second.address = Address & 0x1FFFFFFFu;
    check_attributes(expect_first, capture(Other, Packed));
    check_attributes(expect_second, capture(Address, Packed));
    CHECK(pgxp_load_source(Other, Packed) == 101);
    CHECK(pgxp_load_source(Address, Packed) == 202);
    CHECK(!pgxp_native_import_sxy(15, &receipts[0])); // FIFO push is caller-owned
    CHECK(pgxp_native_import_sxy(14, &receipts[0]));
    psx_pgxp_cop2(nullptr, 0xE82F0000u, Packed, Address + 16); // swc2 SXYP
    CHECK(pgxp_load_source(Address + 16, Packed) == 101);
    auto imported = first_native; imported.address = (Address + 16) & 0x1FFFFFFFu;
    check_attributes(imported, capture(Address + 16, Packed));

    pgxp_checkpoint_begin();
    CHECK(pgxp_native_publish_words(inputs, receipts, 2));
    CHECK(pgxp_load_source(Address, Packed) == 101);
    pgxp_checkpoint_rollback();
    check_attributes(expect_second, capture(Address, Packed));
    CHECK(pgxp_load_source(Address, Packed) == 202);
    pgxp_suppress_begin();
    CHECK(pgxp_native_publish_words(inputs, receipts, 2));
    CHECK(pgxp_native_import_sxy(14, &receipts[1]));
    pgxp_suppress_end();
    check_attributes(expect_second, capture(Address, Packed));
    CHECK(pgxp_load_source(Address, Packed) == 202);

    const uint32_t bad_destinations[2] = {Address, 0x1F801814u};
    CHECK(!pgxp_native_publish_words(bad_destinations, receipts, 2));
    check_attributes(expect_second, capture(Address, Packed)); // atomic refusal
    auto bad_receipt = receipts[0]; bad_receipt.word.address |= 1;
    CHECK(!pgxp_native_publish_words(inputs, &bad_receipt, 1));
    CHECK(!pgxp_native_capture_words(inputs, words, receipts, 5));
    CHECK(!pgxp_native_capture_words(nullptr, words, receipts, 1));

    // Validation is part of consuming the input words. A changed X ends only
    // that half/depth/lineage; the still-matching precise Y survives exactly as
    // it does through the ordinary hooked load/store path.
    const uint32_t partial_value[1] = {Packed ^ 1u};
    PGXPNativeWord partial{};
    CHECK(pgxp_native_capture_words(inputs, partial_value, &partial, 1));
    CHECK(partial.word.valid && !(partial.word.flags & 1u) && (partial.word.flags & 2u));
    CHECK(partial.source == 0);

    // Restore the previously captured full vertex before the clobber case.
    CHECK(pgxp_native_publish_words(inputs, &receipts[1], 1));
    CPUState machine{};
    machine.gpr[7] = Packed;
    psx_pgxp_plain_values[7] = 99;
    psx_pgxp_plain_mask |= 1u << 7;
    CHECK(!pgxp_native_import_gpr(0, &receipts[0]));
    CHECK(!pgxp_native_import_gpr(32, &receipts[0]));
    CHECK(pgxp_native_import_gpr(7, &receipts[0]));
    CHECK(!(psx_pgxp_plain_mask & (1u << 7)));
    psx_pgxp_store(&machine, 0xAC270000u, Address + 32, Packed); // sw a3,0(at)
    auto imported_gpr = first_native; imported_gpr.address = (Address + 32) & 0x1FFFFFFFu;
    check_attributes(imported_gpr, capture(Address + 32, Packed));
    CHECK(pgxp_load_source(Address + 32, Packed) == 101);
    machine.gpr[5] = Packed;
    psx_pgxp_cop2(&machine, 0x48057000u, Packed, 0); // mfc2 a1,SXY2
    psx_pgxp_plain_values[5] = 99;
    psx_pgxp_plain_mask |= 1u << 5;
    machine.gte_data[16] = 123;
    pgxp_native_clobber(&machine, 1u << 5, (1u << 14) | (1u << 16));
    CHECK(!(psx_pgxp_plain_mask & (1u << 5)));
    int live; uint32_t value, flags; int32_t sx, sy; uint16_t sz;
    CHECK(pgxp_debug_shadow(1, 5, &live, &value, &flags, &sx, &sy, &sz));
    CHECK(live && value == Packed && flags == 0);
    CHECK(pgxp_debug_shadow(2, 16, &live, &value, &flags, &sx, &sy, &sz));
    CHECK(live && value == 123 && flags == 0);
    psx_pgxp_cop2(nullptr, 0xE82F0000u, Packed, Address + 16);
    check_attributes(imported, capture(Address + 16, Packed)); // clobber excludes FIFO

    pgxp_invalidate_all();
    seed(Address, Packed, (8 << 16) + 32768);
    const auto new_generation = capture(Address, Packed);
    CHECK(!pgxp_native_publish_words(inputs, receipts, 2));
    CHECK(!pgxp_native_import_sxy(14, &receipts[0]));
    check_attributes(new_generation, capture(Address, Packed));
    // Disabled native clobbers follow the same active gate as full hooks:
    // do not revive generation-invalid shadows merely by resetting a slot.
    pgxp_set_source_tracking(0); pgxp_set_enabled(0);
    machine.gpr[5] = Packed; machine.gte_data[16] = 321;
    pgxp_native_clobber(&machine, 1u << 5, 1u << 16);
    CHECK(pgxp_debug_shadow(1, 5, &live, &value, &flags, &sx, &sy, &sz) && !live);
    CHECK(pgxp_debug_shadow(2, 16, &live, &value, &flags, &sx, &sy, &sz) && !live);
    // Source-only recording is active even with visual precision disabled.
    CHECK(pgxp_set_source_tracking(1));
    CHECK(pgxp_tag_word_source(Address, Packed, 919));
    psx_pgxp_load(&machine, 0x8C250000u, Address, Packed); // lw a1,0(at)
    pgxp_suppress_begin(); machine.gpr[5] = Packed ^ 1u;
    pgxp_native_clobber(&machine, 1u << 5, 0);
    pgxp_suppress_end(); machine.gpr[5] = Packed;
    psx_pgxp_store(&machine, 0xAC250000u, Other, Packed);
    CHECK(pgxp_load_source(Other, Packed) == 919); // suppression preserves lineage
    pgxp_native_clobber(&machine, 1u << 5, 1u << 16);
    psx_pgxp_store(&machine, 0xAC250000u, Other, Packed);
    CHECK(pgxp_load_source(Other, Packed) == 0); // active native overwrite ends it
    CHECK(pgxp_debug_shadow(2, 16, &live, &value, &flags, &sx, &sy, &sz));
    CHECK(live && value == 321 && flags == 0);
    // A native constant output is a known-exact packed word, rather than a
    // shadowless value. Match the maintained LUI/ORI result at a real store.
    pgxp_set_enabled(1); pgxp_set_cpu_mode(1);
    for (uint32_t constant : {0x01000401u, 0x8000FFFFu}) {
        machine.gpr[3] = constant;
        psx_pgxp_alu(&machine, 0x3C030000u, constant & 0xFFFF0000u, 0, 0);
        psx_pgxp_alu(&machine, 0x34630000u | (constant & 65535u), constant,
                     constant & 0xFFFF0000u, constant & 65535u);
        psx_pgxp_store(&machine, 0xAC230000u, Other, constant);
        const auto expected_exact = capture(Other, constant);
        psx_pgxp_plain_values[3] = Packed;
        psx_pgxp_plain_mask |= 1u << 3;
        pgxp_native_exact_gpr(&machine, (1u << 3) | 1u);
        CHECK(!(psx_pgxp_plain_mask & (1u << 3)));
        psx_pgxp_store(&machine, 0xAC230000u, Other, constant);
        check_attributes(expected_exact, capture(Other, constant));
        CHECK(pgxp_load_source(Other, constant) == 0);
    }
    pgxp_set_source_tracking(0); pgxp_set_enabled(0);
    pgxp_native_exact_gpr(&machine, 1u << 3);
    CHECK(pgxp_debug_shadow(1, 3, &live, &value, &flags, &sx, &sy, &sz) && !live);
    CHECK(pgxp_set_source_tracking(1));
    CHECK(pgxp_tag_word_source(Address, Packed, 919));
    psx_pgxp_load(&machine, 0x8C230000u, Address, Packed);
    machine.gpr[3] = Packed;
    pgxp_suppress_begin(); pgxp_native_exact_gpr(&machine, 1u << 3); pgxp_suppress_end();
    psx_pgxp_store(&machine, 0xAC230000u, Other, Packed);
    CHECK(pgxp_load_source(Other, Packed) == 919);
    pgxp_native_exact_gpr(&machine, 1u << 3);
    psx_pgxp_store(&machine, 0xAC230000u, Other, Packed);
    CHECK(pgxp_load_source(Other, Packed) == 0);
    gpu_bytes = 0; // pages retained on the host are not guest allocations
    CHECK(!pgxp_capture_word_shadow(Arena, Packed, &refused));
    std::printf("PGXP packet shadow checks: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
