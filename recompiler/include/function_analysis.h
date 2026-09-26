#pragma once

#include <cstdint>
#include <set>
#include <utility>
#include <vector>
#include <string>
#include "ps1_exe_parser.h"

namespace PSXRecomp {

struct Function {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t size;
    bool has_prologue;
    bool has_epilogue;
    int32_t stack_frame_size; // Size of stack frame (if has prologue)
    std::string name; // Optional name (for now, just "func_<addr>")
    bool is_data_section = false; // True if this "function" is actually a data section
    // Overlapping-alias entry: nonzero means start_addr is an INTERIOR address of
    // the host function beginning at alias_walk_lo. The CFG covers the host's
    // full range [alias_walk_lo, end_addr) and the emitted C function enters via
    // a goto to start_addr's block. The host function is emitted unchanged —
    // aliases never cap or truncate it (the mid-function-seed hazard class).
    uint32_t alias_walk_lo = 0;
    // All alias entries sharing this host (including start_addr). Injected as
    // block leaders so every sibling alias gets an identical CFG, letting the
    // emitter generate ONE shared body per host with an entry switch instead
    // of duplicating the host's blocks per alias.
    std::vector<uint32_t> alias_group_entries;
    // Exact-entry composites retain the source producer that owns this code.
    // Zero means an ordinary single-image function.
    uint32_t producer_lo = 0;
    uint32_t producer_hi = 0;
};

struct AbsorbedEntry {
    uint32_t addr;
    uint32_t host_start;
    uint32_t host_end;
    uint32_t source_addr;
    bool resolved_indirect;
};

struct FunctionAnalysisResult {
    std::vector<Function> functions;
    int total_instructions;
    int jr_ra_count;
    int prologue_count;
    int call_discovered_count = 0; // Functions found via JAL call-target following
    int strong_prologue_count = 0; // Functions found from prologues with saved $ra
    int bios_thunk_count = 0; // Packed A0/B0/C0 BIOS dispatch thunks
    int state_continuation_count = 0; // Split entries after calls to SaveState-style helpers
    int pointer_table_entry_count = 0; // Function entries found from executable pointer tables
    // Statically proven direct/constant-register transfer targets that the
    // FINAL exact-entry partition reaches inside another function. These are
    // safe overlapping aliases: the source edge and host reachability were
    // both observed by the analyzer (not inferred from a raw byte envelope).
    std::vector<AbsorbedEntry> absorbed_entries;
    // Instruction PCs reached by the FINAL exact-entry partition. Consumers
    // use this to reject hostless `interior` seeds that merely fall inside a
    // function's coarse [start,end) envelope or an unreachable data hole.
    std::set<uint32_t> exact_reachable_pcs;
};

// A canonical, bounds-checked MIPS jump table recovered from a jr $rN site.
// `table_base` and each pair's first value are the runtime addresses stored by
// the guest; the pair's second value is the corresponding address in the
// active executable image (normally identical, except for BIOS shell remaps).
struct ExactJumpTable {
    uint32_t table_base = 0;
    uint32_t table_count = 0;
    std::vector<std::pair<uint32_t, uint32_t>> targets;
    // Non-zero when the targets came from resolve_computed_stride_jump
    // rather than a table in memory: `table_base` is then the first word of
    // the unrolled run and the targets are table_base + k * stride.
    uint32_t stride = 0;
    // True when the targets came from resolve_self_limited_jump_table: the
    // guest checks no bound, so the extent was taken from the table's own
    // layout (it ends where its lowest target begins).
    bool self_limited = false;
};

using ExactAddressMapper = uint32_t (*)(uint32_t, const PS1Executable&);

// Recognize only the canonical bounded-switch dependency chain:
// sltiu/beq guard -> sll index,2 -> addu table address -> lw target -> jr.
// The table constant may use either same-register or cross-register
// `lui source; addiu base,source,lo`, either before the guard or scheduled
// exactly as `sltiu; beq; lui (delay slot); addiu; sll` without clobbering
// the checked index or allowing direct edges to bypass the guard.
// `producer_lo/producer_hi` bound both
// table storage and case code for composite images; zero/zero means the full
// executable. Every dependency, table word, and target must pass the hard
// safety checks; otherwise the whole table is rejected.
bool resolve_exact_bounded_jump_table(
    const PS1Executable& exe,
    uint32_t entry,
    uint32_t hard_cap,
    uint32_t jr_pc,
    uint32_t jr_rs,
    ExactJumpTable& table,
    ExactAddressMapper runtime_to_image = nullptr,
    uint32_t producer_lo = 0,
    uint32_t producer_hi = 0);

// Recognize a computed-stride entry into an unrolled run (Duff's device):
//
//     sll   S, I, k              (stride = 1<<k)
//   or
//     sll   P, I, a ; sll Q, I, b ; addu S, P, Q     (stride = (1<<a)+(1<<b))
//     addu  R, B, S   (either operand order)
//     jr    R
//     <delay slot>
//     run_start: N >= 2 shape-identical groups of `stride` bytes — the same
//                opcodes and register fields in every group, immediates free,
//                no control flow inside a group.
//
// The dependency chain is walked backwards from the jr through nearest
// definitions with no control flow in between. The base register's value is
// deliberately NOT assumed: the caller emits a switch on the actual runtime
// target with the CPS tail-transfer as the default, so the recovered targets
// only ever add native cases and can never redirect a jump that lands
// elsewhere. Targets are run_start + k * stride for k = 0..N (k = N is the
// word after the run, the loop tail); all lie inside [entry, hard_cap).
// `table.stride` is set to the stride; `table.table_base` to run_start.
bool resolve_computed_stride_jump(
    const PS1Executable& exe,
    uint32_t entry,
    uint32_t hard_cap,
    uint32_t jr_pc,
    uint32_t jr_rs,
    ExactJumpTable& table,
    ExactAddressMapper runtime_to_image = nullptr);

// Recognize an in-function pointer table indexed by a value the guest never
// bounds-checks (a stored, pre-scaled state offset rather than a checked
// case number):
//
//     lui   T, hi ; ori|addiu T, T, lo     (table base, an in-function constant)
//     addu  T, T, I   (either operand order; I is the byte offset)
//     lw    R, off(T)
//     [nop]
//     jr    R
//
// With no guard there is no count to read, so the extent comes from the
// table itself: words are taken from the base while each is a 4-aligned
// in-function code address outside every delay slot, and the table ends
// where its lowest target begins (a pointer table cannot overlap the code
// it points at). As with resolve_computed_stride_jump the caller switches
// on the actual runtime target with the CPS tail-transfer as default, so the
// recovered entries only add native cases; a value past the recovered
// extent still takes the fallback. Requires at least two entries.
bool resolve_self_limited_jump_table(
    const PS1Executable& exe,
    uint32_t entry,
    uint32_t hard_cap,
    uint32_t jr_pc,
    uint32_t jr_rs,
    ExactJumpTable& table,
    ExactAddressMapper runtime_to_image = nullptr);

class FunctionAnalyzer {
public:
    explicit FunctionAnalyzer(const PS1Executable& exe);

    // Scan entire executable for function boundaries
    FunctionAnalysisResult analyze();

    // Analyze only explicit entry points and callable direct-JAL targets
    // reachable from them. Used by runtime-loaded overlays and opt-in main-EXE
    // reachable discovery. Unresolved jalr/indirect targets do not mint
    // functions; evidence-backed entries must be supplied explicitly.
    //
    // no_split_guard (overlay mode): an EXPLICIT entry that the entry below
    // it reaches by fallthrough or branch -- walked with the entry's own cap
    // removed -- is inside that function. It is absorbed like a derived
    // call target and exported in absorbed_entries (an alias, never a cap).
    // `trusted_entries` (classifier-promoted dispatch roots) are exempt.
    // compile_overlays.py applies the same rule before writing seeds; this is
    // the enforcement point for walk proofs only this analyzer can make
    // (computed-stride and self-limited jump tables). Off by default so the
    // main-EXE reachable-discovery partition is unchanged.
    FunctionAnalysisResult analyze_exact_entries(
        const std::vector<uint32_t>& entries,
        const std::vector<std::pair<uint32_t, uint32_t>>& producer_ranges = {},
        const std::set<uint32_t>& cross_call_allow = {},
        bool no_split_guard = false,
        const std::set<uint32_t>& trusted_entries = {});

    // Add a forced entry point address that is treated as a function start
    // even if it has no standard ADDIU $sp prologue. The function will be
    // included in the analysis result with has_prologue = false.
    void add_forced_entry(uint32_t addr);

    // Check if instruction is jr $ra (return)
    static bool is_jr_ra(uint32_t instr);

    // Check if instruction is function prologue (addiu $sp, $sp, -N)
    static bool is_prologue(uint32_t instr, int32_t& stack_size);

    // Check if instruction is epilogue (addiu $sp, $sp, +N)
    static bool is_epilogue(uint32_t instr, int32_t& stack_size);

    // Check if instruction is a branch or jump (has a delay slot)
    static bool is_branch_or_jump(uint32_t instr);

    // Check if a raw word decodes as a plausible R3000 instruction. Used to
    // validate scanned data pointers before promoting/aliasing them.
    static bool is_valid_mips_word(uint32_t instr);

private:
    const PS1Executable& exe_;

    // Forced entry points (from add_forced_entry())
    std::vector<uint32_t> forced_entry_points_;

    // Find function start by scanning backward from jr $ra
    uint32_t find_function_start(uint32_t return_addr);

    // Detect PSY-Q style BIOS dispatch thunks:
    //   addiu/ori rN, $zero, {0xA0,0xB0,0xC0}
    //   jr        rN
    //   addiu/ori $t1, $zero, function_index
    bool is_bios_dispatch_thunk(uint32_t addr, uint32_t& jr_addr_out) const;

    // Detect if a region is likely a data section masquerading as code
    bool is_likely_data_section(uint32_t start_addr, uint32_t end_addr) const;
};

} // namespace PSXRecomp
