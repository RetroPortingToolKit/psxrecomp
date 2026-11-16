#include "pgxp.h"
#include "pgxp_hooks.h"
#include "mod_memory.h"
#include "cpu_state.h"
#include "../../recompiler/src/pgxp_demand_regions.h"
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
    CHECK(!(a.flags&PGXP_F_PROJECTION)||!std::memcmp(&a.projection,&b.projection,sizeof a.projection));
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
    pgxp_set_enabled(1);pgxp_set_cpu_mode(1);
    seed(Address,Packed,(8<<16)+24576);CHECK(pgxp_tag_word_source(Address,Packed,819));
    PGXPNativeWord control_word{};
    CHECK(pgxp_native_capture_words(inputs,&Packed,&control_word,1));
    const auto before_exact_query=capture(Address,Packed);
    PGXPNativeWord half_receipt{};
    CHECK(pgxp_native_capture_half(Address,uint16_t(Packed),0,&half_receipt));
    CHECK(half_receipt.word.value==uint16_t(Packed));
    CHECK(half_receipt.word.flags==PGXP_F_VXY&&half_receipt.word.x16==before_exact_query.x16&&half_receipt.word.y16==0);
    CHECK(half_receipt.source==0);
    // LH ends the GPR full scalar. MTC2 keeps its packed half receipt, not
    // a newly fabricated full scalar, including a same-integer later MFC2.
    CHECK(pgxp_native_scalar_data(9,8,99.75,1));
    CHECK(pgxp_native_import_data(9,&half_receipt));
    CHECK(pgxp_debug_shadow(2,9,&live,&value,&flags,&sx,&sy,&sz));
    CHECK(live&&value==8&&flags==PGXP_F_VXY&&sx==before_exact_query.x16&&sy==0&&sz==0);
    double typed_scalar=0;
    CHECK(!pgxp_native_gte_scalar(9,8,&typed_scalar));
    machine.gpr[5]=8;psx_pgxp_cop2(&machine,0x48054800u,8,0); // MFC2 a1,IR1
    psx_pgxp_store(&machine,0xAC250000u,Other,8);
    CHECK(capture(Other,8).x16==before_exact_query.x16);
    CHECK(pgxp_native_scalar_data(9,8,99.75,1));
    pgxp_suppress_begin();CHECK(pgxp_native_import_data(9,&half_receipt));pgxp_suppress_end();
    CHECK(pgxp_native_gte_scalar(9,8,&typed_scalar)&&typed_scalar==99.75);
    CHECK(pgxp_native_import_data(9,&half_receipt));
    CHECK(!pgxp_native_import_data(12,&half_receipt));
    CHECK(!pgxp_native_import_data(32,&half_receipt));
    CHECK(pgxp_native_import_data(6,&control_word));machine.gte_data[6]=Packed;
    psx_pgxp_cop2(&machine,0xE8060000u,Packed,Other);
    CHECK(pgxp_load_source(Other,Packed)==819);
    const uint32_t half_destination=Other;
    CHECK(pgxp_native_publish_words(&half_destination,&half_receipt,1));
    const auto native_half=capture(Other,uint16_t(Packed));
    psx_pgxp_load(&machine,0x94250000u,Address,uint16_t(Packed));
    psx_pgxp_store(&machine,0xAC250000u,Other,uint16_t(Packed));
    check_attributes(native_half,capture(Other,uint16_t(Packed)));
    CHECK(pgxp_native_capture_half(Address+2,uint16_t(Packed>>16),1,&half_receipt));
    CHECK(half_receipt.word.value==(uint32_t)(int32_t)(int16_t)(Packed>>16));
    CHECK(!pgxp_native_capture_half(Address+1,0,0,&half_receipt));
    // A half input leaves even stale metadata in the other half untouched.
    CHECK(pgxp_native_capture_half(Address+2,uint16_t(Packed>>16)^1u,0,&half_receipt));
    CHECK(!(half_receipt.word.flags&PGXP_F_VX));
    check_attributes(before_exact_query,capture(Address,Packed));
    CHECK(!pgxp_native_word_exact(Address,Packed));
    CHECK(pgxp_native_word_exact(Address,Packed^0x00010001u)); // both carried halves mismatch
    check_attributes(before_exact_query,capture(Address,Packed));
    CHECK(pgxp_load_source(Address,Packed)==819);
    CHECK(!pgxp_native_import_control(32,&control_word));
    CHECK(pgxp_native_import_control(8,&control_word));
    machine.gpr[5]=Packed;
    psx_pgxp_cop2(&machine,0x48454000u,Packed,0); // CFC2 a1,control8
    psx_pgxp_store(&machine,0xAC250000u,Other,Packed);
    auto expected_control=control_word.word;expected_control.address=Other&0x1FFFFFFFu;
    check_attributes(expected_control,capture(Other,Packed));
    CHECK(pgxp_load_source(Other,Packed)==819);
    machine.gpr[5]=8;
    CHECK(pgxp_native_scalar_gpr(&machine,5,8.375,1));
    psx_pgxp_store(&machine,0xAC250000u,Other,8);
    const auto scalar_output=capture(Other,8);
    CHECK(scalar_output.valid&&scalar_output.flags==PGXP_F_VXY&&scalar_output.x16==(8<<16)+24576);
    CHECK(pgxp_load_source(Other,8)==0);
    psx_pgxp_cop2(&machine,0x48854800u,8,0); // MTC2 a1,IR1: maintained scalar input
    double native_scalar=-1;
    CHECK(pgxp_native_gte_scalar(9,8,&native_scalar)&&native_scalar==8.375);
    CHECK(!pgxp_native_gte_scalar(9,9,&native_scalar)&&native_scalar==8.375);
    CHECK(pgxp_native_gpr_scalar(5,8,&native_scalar)&&native_scalar==8.375);
    CHECK(!pgxp_native_gpr_scalar(5,9,&native_scalar)&&native_scalar==8.375);
    machine.gpr[5]=100000000u;CHECK(pgxp_native_scalar_gpr(&machine,5,100000000.5,1));
    CHECK(pgxp_native_gpr_scalar(5,machine.gpr[5],&native_scalar)&&native_scalar==100000000.5);
    CHECK(pgxp_native_scalar_data(9,301,301.5,1));
    CHECK(pgxp_native_gte_scalar(9,301,&native_scalar)&&native_scalar==301.5);
    CHECK(!pgxp_native_gte_scalar(9,302,&native_scalar));
    CHECK(pgxp_native_scalar_data(9,301,301.0,0));
    CHECK(!pgxp_native_gte_scalar(9,301,&native_scalar));
    CHECK(!pgxp_native_scalar_data(14,Packed,0,0)); // FIFO belongs to the vertex boundary
    pgxp_suppress_begin();CHECK(pgxp_native_scalar_data(9,302,302.5,1));pgxp_suppress_end();
    CHECK(!pgxp_native_gte_scalar(9,302,&native_scalar));
    machine.lo=7;CHECK(pgxp_native_scalar_gpr(&machine,33,7.75,1));
    machine.gpr[5]=7;psx_pgxp_alu(&machine,0x00002812u,7,7,0); // MFLO a1
    psx_pgxp_store(&machine,0xAC250000u,Other,7);
    CHECK(capture(Other,7).x16==(7<<16)+49152);
    CHECK(!pgxp_native_word_exact(Other,7));
    machine.gpr[5]=7;CHECK(pgxp_native_scalar_gpr(&machine,5,7.0,1));
    psx_pgxp_store(&machine,0xAC250000u,Other,7);CHECK(pgxp_native_word_exact(Other,7));
    CHECK(!pgxp_native_scalar_gpr(&machine,0,0,0));
    CHECK(!pgxp_native_scalar_gpr(&machine,34,0,0));
    // Actual native packet regression: a high-half store cannot turn unknown
    // low RAM bits into a validated whole word. Retain the same stale-half
    // state as the maintained SH, then compare both cold and tracked outputs.
    pgxp_invalidate_all();
    constexpr uint32_t HalfReference=Other+64,HalfNative=Other+68;
    PGXPNativeWord half_source{};
    CHECK(pgxp_native_capture_half(Address,0x4567,0,&half_source));
    machine.gpr[5]=0x4567;psx_pgxp_load(&machine,0x94250000u,Address,0x4567);
    psx_pgxp_store(&machine,0xA4250000u,HalfReference+2,0x4567);
    CHECK(pgxp_native_publish_half(HalfNative+2,&half_source,0,0));
    CHECK(!capture(HalfReference,0x45670007u).valid);
    CHECK(!capture(HalfNative,0x45670007u).valid);
    auto expected_half=capture(HalfReference,0x45670000u);
    auto actual_half=capture(HalfNative,0x45670000u);actual_half.address=expected_half.address;
    check_attributes(expected_half,actual_half);
    seed(Address,Packed,(8<<16)+24576);
    seed(HalfReference,Packed,(8<<16)+16384);seed(HalfNative,Packed,(8<<16)+16384);
    CHECK(pgxp_tag_word_source(HalfReference,Packed,917));CHECK(pgxp_tag_word_source(HalfNative,Packed,918));
    CHECK(pgxp_native_capture_half(Address,8,0,&half_source));
    machine.gpr[5]=8;psx_pgxp_load(&machine,0x94250000u,Address,8);
    psx_pgxp_store(&machine,0xA4250000u,HalfReference,8);
    CHECK(pgxp_native_publish_half(HalfNative,&half_source,0,0));
    expected_half=capture(HalfReference,Packed);actual_half=capture(HalfNative,Packed);actual_half.address=expected_half.address;
    check_attributes(expected_half,actual_half);
    CHECK(actual_half.flags==PGXP_F_VXY&&actual_half.x16==(8<<16)+24576&&actual_half.y16==(12<<16)+32768);
    CHECK(pgxp_load_source(HalfNative,Packed)==0);
    half_source.word.value=7;half_source.word.flags=0;
    machine.gpr[5]=7;CHECK(pgxp_native_scalar_gpr(&machine,5,7.75,1));
    psx_pgxp_store(&machine,0xA4250000u,HalfReference,7);
    CHECK(pgxp_native_publish_half(HalfNative,&half_source,7.75,1));
    expected_half=capture(HalfReference,0x000C0007u);actual_half=capture(HalfNative,0x000C0007u);actual_half.address=expected_half.address;
    check_attributes(expected_half,actual_half);CHECK(actual_half.x16==(7<<16)+49152);
    const auto before_partial=capture(HalfNative,0x000C0007u);
    pgxp_checkpoint_begin();CHECK(pgxp_native_publish_half(HalfNative+2,&half_source,7.75,1));pgxp_checkpoint_rollback();
    check_attributes(before_partial,capture(HalfNative,0x000C0007u));
    pgxp_suppress_begin();CHECK(pgxp_native_publish_half(HalfNative,&half_source,0,0));pgxp_suppress_end();
    check_attributes(before_partial,capture(HalfNative,0x000C0007u));
    CHECK(!pgxp_native_publish_half(HalfNative+1,&half_source,0,0));
    CHECK(!pgxp_native_publish_half(0x1F801814u,&half_source,0,0));
    pgxp_set_enabled(0);CHECK(pgxp_set_source_tracking(1));
    CHECK(pgxp_native_capture_half(Address,7,0,&half_source));
    CHECK(pgxp_tag_word_source(HalfNative,Packed,919));
    CHECK(pgxp_native_publish_half(HalfNative,&half_source,0,0));
    CHECK(pgxp_load_source(HalfNative,0x000C0007u)==0);
#if defined(PSX_PGXP_OPERATIONS) && PSX_PGXP_OPERATIONS
    pgxp_set_enabled(1);pgxp_set_cpu_mode(1);pgxp_set_source_tracking(1);
    // Whole integer receipts must reproduce canonical scalar/half behavior.
    struct IntegerCase {uint32_t instruction,input,result,kind;};
    const IntegerCase cases[]={
        {0x3c1a1234u,0,0x12340000u,1}, // LUI: both exact
        {0x001bd400u,0x76543210u,0x32100000u,3}, // SLL16: zero low half
        {0x001bd402u,0x76543210u,0x00007654u,2}, // SRL16: exact high half
        {0x337a0000u,0x76543210u,0,4}, // ANDI0: two exact zero halves
        {0x277a0007u,17,24,0}, // untracked ADDIU
        {0x2b7a0020u,17,1,0}, // SLTI kills coordinate lifetime
    };
    for(const auto& c:cases) {
        pgxp_invalidate_all();CPUState cpu{};cpu.gpr[27]=c.input;
        pgxp_native_clobber(&cpu,1u<<27,0);
        cpu.gpr[26]=c.result;
        psx_pgxp_alu(&cpu,c.instruction,c.result,c.input,c.instruction&0xffffu);
        psx_pgxp_store(&cpu,0xac1a0000u,Other,c.result);
        const auto expected=capture(Other,c.result);
        pgxp_native_clobber(&cpu,(1u<<27)|(1u<<26),0);
        CHECK(psx_pgxp_operation_begin(&cpu,1u<<27,1u<<26,nullptr,0,1,0,0));
        psx_pgxp_operation_end(&cpu,c.kind==0?1u<<26:0,c.kind==1?1u<<26:0,
            c.kind==2?1u<<26:0,c.kind==3?1u<<26:0,c.kind==4?1u<<26:0);
        psx_pgxp_store(&cpu,0xac1a0000u,Other,c.result);
        check_attributes(expected,capture(Other,c.result));
    }
    // Actual write ownership is the sole store metadata producer inside a
    // mixed operation. Keep all typed records, including preexisting precision
    // in an equal-bit destination and the raw virtual alias supplied by code.
    constexpr uint32_t StoreReference=0x80061000u,StoreNative=0x80061004u;
    CPUState owner{};owner.gpr[27]=Packed;pgxp_native_clobber(&owner,1u<<27,0);
    PGXPProjection old_projection{8.25f,12.5f,2.f,.5f};
    auto seed_store=[&](uint32_t a){seed(a,Packed,(8<<16)+16384);
        pgxp_gte_set_projection(&old_projection);pgxp_store_gte_reg(a,15);};
    seed_store(StoreReference);seed_store(StoreNative);
    const PGXPStoreRecord *records=nullptr;uint32_t capacity=0;
    const uint64_t canonical_begin=pgxp_store_ring(&records,&capacity);
    pgxp_write_owned(StoreReference,Packed,4);psx_pgxp_store(&owner,0xac1b0000u,StoreReference,Packed);
    pgxp_write_owned(StoreReference+2,8,2);psx_pgxp_store(&owner,0xa41b0000u,StoreReference+2,8);
    pgxp_write_owned(StoreReference+3,8,1);psx_pgxp_store(&owner,0xa01b0000u,StoreReference+3,8);
    const auto store_expected=capture(StoreReference,0x08080008u);
    const uint64_t canonical_end=pgxp_store_ring(&records,&capacity);
    CHECK(canonical_end-canonical_begin==2);
    PGXPStoreRecord expected_records[2]={records[canonical_begin%capacity],records[(canonical_begin+1)%capacity]};
    uint8_t proof_bytes[4]={};
    const uint32_t native_alias=StoreNative^0x20200000u; // KSEG1 + retail mirror
    PSXPGXPOperationAccess owned_writes[]={
        {native_alias,0x104u,proof_bytes,0xac1b0000u},
        {native_alias+2,0x102u,proof_bytes,0xa41b0000u},
        {native_alias+3,0x101u,proof_bytes,0xa01b0000u}};
    CHECK(psx_pgxp_operation_begin(&owner,1u<<27,0,owned_writes,3,0,0,3));
    pgxp_write_owned(StoreNative,Packed,4);pgxp_write_owned(StoreNative+2,8,2);pgxp_write_owned(StoreNative+3,8,1);
    psx_pgxp_operation_end(&owner,0,0,0,0,0);
    auto actual_store=capture(StoreNative,0x08080008u);actual_store.address=store_expected.address;
    check_attributes(store_expected,actual_store);
    const uint64_t native_end=pgxp_store_ring(&records,&capacity);CHECK(native_end-canonical_end==2);
    for(unsigned j=0;j<2;++j){auto expected=expected_records[j],actual=records[(canonical_end+j)%capacity];
        expected.seq=actual.seq;expected.addr=native_alias+j*2u;
        CHECK(!std::memcmp(&expected,&actual,sizeof expected));}
    // A partial write retains the untouched low coordinate/depth semantics,
    // and its checkpoint rolls back exactly like the canonical typed SH.
    seed_store(StoreReference);seed_store(StoreNative);
    pgxp_write_owned(StoreReference+2,8,2);psx_pgxp_store(&owner,0xa41b0000u,StoreReference+2,8);
    const auto half_expected=capture(StoreReference,0x00080008u),half_before=capture(StoreNative,Packed);
    PSXPGXPOperationAccess owned_partial{native_alias+2,0x102u,proof_bytes,0xa41b0000u};
    pgxp_checkpoint_begin();CHECK(psx_pgxp_operation_begin(&owner,1u<<27,0,&owned_partial,1,0,0,1));
    pgxp_write_owned(StoreNative+2,8,2);psx_pgxp_operation_end(&owner,0,0,0,0,0);
    auto half_actual=capture(StoreNative,0x00080008u);half_actual.address=half_expected.address;
    check_attributes(half_expected,half_actual);pgxp_checkpoint_rollback();
    check_attributes(half_before,capture(StoreNative,Packed));
    // Do not bypass cold partial-word validation merely because both stores
    // and reads are integer. Full-word write then read is an owned safe case.
    pgxp_invalidate_word(StoreNative);
    PSXPGXPOperationAccess partial_read[]={owned_partial,{native_alias,4,proof_bytes,0}};
    CHECK(!psx_pgxp_operation_begin(&owner,1u<<27,0,partial_read,2,0,1,1));
    CPUState proof{};proof.gpr[5]=7;pgxp_native_clobber(&proof,1u<<5,0);
    CHECK(pgxp_native_scalar_gpr(&proof,5,7.25,1));
    CHECK(!psx_pgxp_operation_begin(&proof,1u<<5,0,nullptr,0,1,0,0));
    uint8_t actual[]={8,0,12,0};PSXPGXPOperationAccess access{StoreNative+64,4,actual,0};
    pgxp_write_owned(StoreNative+64,Packed,4);
    CHECK(psx_pgxp_operation_begin(&proof,0,0,&access,1,0,1,0));
    CHECK(pgxp_tag_word_source(StoreNative+64,Packed,811));
    CHECK(!psx_pgxp_operation_begin(&proof,0,0,&access,1,0,1,0));
    pgxp_write_owned(StoreNative+64,Packed,4);actual[0]^=1;
    CHECK(!psx_pgxp_operation_begin(&proof,0,0,&access,1,0,1,0));
    actual[0]=8;seed(StoreNative+64,Packed,(8<<16)+16384);
    CHECK(!psx_pgxp_operation_begin(&proof,0,0,&access,1,0,1,0));
#endif
    // Compare actual canonical instruction results with compiler receipts in
    // both CPU modes, including equal-word overwrite of existing source/scalar.
    for(int mode=0;mode<2;++mode) {
        pgxp_set_enabled(1);pgxp_set_cpu_mode(mode);pgxp_set_source_tracking(1);
        PSXRecomp::PgxpRegionState abstract{};
        struct Step {uint32_t word,left,right,value;};
        const Step steps[]={
            {0x3c038000u,0,0x8000u,0x80000000u},
            {0x24631234u,0x80000000u,0x1234u,0x80001234u},
            {0x34630080u,0x80001234u,0x80u,0x800012b4u},
            {0x28630010u,0x800012b4u,0x10u,1u},
            {0x24630002u,1u,2u,3u},
            {0x3c038000u,0,0x8000u,0x80000000u},
            {0x34637000u,0x80000000u,0x7000u,0x80007000u},
            {0x24632000u,0x80007000u,0x2000u,0x80009000u}, // unsigned no-carry, signed low crosses
            {0x3c038000u,0,0x8000u,0x80000000u},
            {0x34638000u,0x80000000u,0x8000u,0x80008000u},
            {0x2463ffffu,0x80008000u,0xffffffffu,0x80007fffu}, // unsigned no-borrow
        };
        for(const auto& step:steps) {
            unsigned dst=0;PSXRecomp::PgxpRegionValue recipe;
            CHECK(PSXRecomp::pgxp_region_definition(step.word,abstract,dst,recipe));
            const uint32_t bit=1u<<dst;CPUState state{};state.gpr[dst]=step.value;
            if((step.word>>26)==15) {
                seed(Other,step.value,(8<<16)+32768);
                CHECK(pgxp_tag_word_source(Other,step.value,991));
                psx_pgxp_load(&state,0x8c030000u,Other,step.value);
            } else if((step.word>>26)==10)
                CHECK(pgxp_native_scalar_gpr(&state,dst,double(step.value)+.25,1));
            psx_pgxp_alu(&state,step.word,step.value,step.left,step.right);
            psx_pgxp_store(&state,0xac030000u,Other,step.value);
            const auto expected=capture(Other,step.value);
            double canonical_scalar=0,actual_scalar=0;
            const int had_scalar=pgxp_native_gpr_scalar(dst,step.value,&canonical_scalar);
            CHECK(pgxp_native_scalar_gpr(&state,dst,double(step.value)+.25,1));
            psx_pgxp_region_materialize(&state,bit,
                (recipe.flags[0]&1)?bit:0,(recipe.flags[0]&2)?bit:0,
                (recipe.flags[1]&1)?bit:0,(recipe.flags[1]&2)?bit:0);
            psx_pgxp_store(&state,0xac030000u,Other,step.value);
            check_attributes(expected,capture(Other,step.value));
            CHECK(pgxp_load_source(Other,step.value)==0);
            CHECK(pgxp_native_gpr_scalar(dst,step.value,&actual_scalar)==had_scalar);
            if(had_scalar)CHECK(canonical_scalar==actual_scalar);
            abstract[dst]=recipe;
        }
        // Unknown/source/scalar-bearing input cannot be declared plain merely
        // because integer address/branch consumers follow it.
        unsigned dst;PSXRecomp::PgxpRegionValue recipe;PSXRecomp::PgxpRegionState unknown{};
        CHECK(!PSXRecomp::pgxp_region_definition(0x24630004u,unknown,dst,recipe));
        CHECK(!PSXRecomp::pgxp_region_definition(0x00641821u,unknown,dst,recipe));
        // An exact low scalar can become a fractional DIV/MULT input later.
        unknown[3].known=unknown[3].constant=true;unknown[3].value=7;
        unknown[3].flags[0]=unknown[3].flags[1]=3;
        CHECK(!PSXRecomp::pgxp_region_definition(0x24630004u,unknown,dst,recipe));
    }
    gpu_bytes = 0; // pages retained on the host are not guest allocations
    CHECK(!pgxp_capture_word_shadow(Arena, Packed, &refused));
    std::printf("PGXP packet shadow checks: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
