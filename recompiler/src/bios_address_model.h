// bios_address_model.h — the BIOS profile's address model, as one value type.
// ----------------------------------------------------------------------------
// A PS1 BIOS relocates code out of ROM into RAM at boot (SCPH1001: the Kernel
// Part 2 copy to RAM 0x500 and the shell copy to RAM 0x80030000). Before this
// existed, those window constants were copy-pasted across ~9 sites in
// full_function_emitter.cpp / function_discovery.cpp — including as literal C
// text emitted into the generated dispatch — and the two files had drifted on
// the shell window's upper bound. This class is the single source of truth,
// built from the profile's [recompiler.address_model] tables; both the C++
// helpers and the emitted-C text generators derive from the same table, so
// they cannot disagree.
//
// A profile with NO copy entries (a BIOS that runs entirely from ROM) is a
// valid, first-class model: normalize() degenerates to the KSEG mask, there is
// no kernel-bless window, and every generator emits nothing.
//
// Every [[recompiler.address_model.copy]] entry is a CLAIM that the BIOS's
// boot-time copy is byte-verbatim (lw/sw loop or memcpy, no patching or
// decompression). The runtime's kernel-bless memcmp is the enforcement of
// that claim for the kernel_bless window.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace PSXRecompV4 {

struct BiosConfig;

// One [[recompiler.address_model.copy]]: a boot-time bulk copy of code from
// ROM [rom_lo, rom_hi) to RAM ram_lo+, executed by the CPU at runtime_base+.
struct BiosAddrCopy {
    std::string name;             // comment label in emitted C, e.g. "Kernel Part 2"
    uint32_t    rom_lo = 0;       // physical ROM window [lo, hi) — hi EXCLUSIVE
    uint32_t    rom_hi = 0;
    uint32_t    ram_lo = 0;       // physical RAM destination of rom_lo
    uint32_t    runtime_base = 0; // vaddr the CPU executes the copy at (KUSEG/KSEG0)
    bool        key_is_ram = false;   // dispatch_key = "ram": functions in this
                                      // window are keyed by RAM address (normalize
                                      // folds ROM->RAM). "rom": keyed by ROM
                                      // (normalize folds RAM->ROM).
    bool        kernel_bless = false; // runtime may byte-verify live RAM vs ROM
                                      // and run native (memory.c kbless)

    uint32_t len() const { return rom_hi - rom_lo; }
    uint32_t ram_hi() const { return ram_lo + len(); }   // exclusive
};

// One [[recompiler.install_slots]]: a RANGE of kernel-RAM words the guest is
// expected to overwrite at runtime (Psy-Q's _patch_gte / _patch_card /
// _patch_card2 / _patch_pad, and the BIOS's own install stubs).
//
// The original model was a single 4-word `lui/addiu/jalr/nop` stub written
// over ROM zeros, detected by testing the first word against 0. Only one of
// the five shapes observed in the wild fits that (docs/dynamic_handler_install.md):
// patchers also overwrite REAL ROM instructions with a `jr`, rewrite an
// 11-word prologue and insert an `mfc0 Cause`, and NOP out a driver routine.
// So a slot carries a length and is detected against the ROM-BAKED words,
// not against zero.
struct BiosInstallSlot {
    // How the compiled body resumes native execution after the interpreter
    // has run the patched words.
    enum class Resume {
        Jalr,         // legacy 4-word stub: the jalr's ra = ram_addr + 0x10
        Fallthrough,  // the patched words ARE the function: resume at ram_addr + len
        None,         // the patch never returns here (a `jr` out of the kernel)
    };

    uint32_t ram_addr = 0;                  // start of the patched range
    uint32_t len      = 0x10;               // bytes; legacy default = 4 words
    Resume   resume   = Resume::Jalr;       // legacy default

    uint32_t hi() const { return ram_addr + len; }          // exclusive
    // RAM PC the compiled body resumes at, or 0 for Resume::None.
    uint32_t resume_addr() const {
        switch (resume) {
            case Resume::Jalr:        return ram_addr + 0x10u;
            case Resume::Fallthrough: return hi();
            default:                  return 0u;
        }
    }
};

class BiosAddressModel {
public:
    // Empty model: pure KSEG-mask normalization, no copies, no install slots.
    BiosAddressModel() = default;

    // Build from a loaded profile, enforcing the semantic invariants
    // (disjoint input windows, no output/input intersection, single bless
    // window, alignment, in-image bounds). Throws std::runtime_error with the
    // profile path in the message on violation.
    static BiosAddressModel from_config(const BiosConfig& cfg);

    // --- C++-side address mapping (used while analyzing/emitting) ---------

    // Dispatch-key normalization: KSEG strip, then per-copy key fold.
    uint32_t normalize(uint32_t addr) const;

    // RAM alias -> KSEG1 ROM virtual address (for reading jump tables etc.
    // out of the ROM image when a pointer references the relocated RAM copy).
    // Returns addr unchanged when it is not inside any copy's RAM window.
    uint32_t ram_alias_to_rom(uint32_t addr) const;

    // ROM PC -> the vaddr the CPU actually executes it from (runtime_base+).
    // Identity for ROM outside every copy window.
    uint32_t runtime_pc(uint32_t rom_pc) const;

    // Fix a J/JAL target encoded relative to the RUNTIME region of relocated
    // code (the 26-bit target inherits (runtime PC & 0xF0000000), not the ROM
    // region).
    uint32_t relocate_j_target(uint32_t rom_addr, uint32_t target) const;

    // ROM PC -> physical RAM address, for RAM-keyed copies only (install-slot
    // detection in kernel RAM). Non-RAM-keyed windows and plain ROM return
    // the physical address unchanged.
    uint32_t rom_to_ram_phys(uint32_t rom_addr) const;

    // Discovery-side J/JAL remap: compute the runtime-region target, then fold
    // it back to a followable KSEG1 ROM address through any copy's RAM window.
    // Returns the original target when the source is not in a copy window or
    // the computed target is not resolvable to ROM.
    uint32_t remap_relocated_j_target(uint32_t target, uint32_t source_rom_addr) const;

    // --- kernel-bless window ---------------------------------------------

    bool     has_kbless() const { return kbless_idx_ >= 0; }
    uint32_t kbless_ram_lo() const;    // inclusive
    uint32_t kbless_ram_hi() const;    // exclusive
    uint32_t kbless_rom_off() const;   // ROM file offset of kbless_ram_lo
    bool     in_kbless(uint32_t norm) const;

    // --- install slots ----------------------------------------------------

    // True when ram_pc is the START of a declared slot (where the emitter
    // plants the hook). Interior words of a range are NOT slot starts.
    bool is_install_slot(uint32_t ram_pc) const;
    // The slot starting at ram_pc, or null.
    const BiosInstallSlot* install_slot_at(uint32_t ram_pc) const;
    const std::vector<BiosInstallSlot>& install_slots() const { return install_slots_; }
    // Is this RAM PC inside ANY declared range? Such a PC must never become a
    // native dispatch key: the compiled bytes there are not what executes —
    // the guest's patch is. A key inside a range re-enters the compiled body,
    // whose patch-range hook immediately sets pc back to the range start and
    // returns, so the dispatch loop spins forever (observed 2026-09-11: a
    // pre-existing jal-return continuation at OpenBIOS RAM 0x357C wedged the
    // boot at frame 0).
    bool in_install_slot_range(uint32_t ram_pc) const;

    // --- ROM-keyed RAM window (shell-style), for the game-overlap text ----

    bool     has_rom_keyed_ram_window() const { return rom_keyed_idx_ >= 0; }
    uint32_t rom_keyed_ram_lo() const;         // inclusive
    uint32_t rom_keyed_ram_hi_incl() const;    // INCLUSIVE (matches emitted text)

    // --- emitted-C text generators (same table as the C++ methods) --------

    // The generated dispatch's `static uint32_t normalize(uint32_t)`.
    std::string emit_normalize_c() const;

    const std::vector<BiosAddrCopy>& copies() const { return copies_; }

private:
    std::vector<BiosAddrCopy> copies_;
    std::vector<BiosInstallSlot> install_slots_;
    uint32_t                  rom_base_phys_ = 0x1FC00000u;
    uint32_t                  rom_size_ = 0x80000u;
    int                       kbless_idx_ = -1;     // index into copies_
    int                       rom_keyed_idx_ = -1;  // first dispatch_key="rom" copy
};

} // namespace PSXRecompV4
