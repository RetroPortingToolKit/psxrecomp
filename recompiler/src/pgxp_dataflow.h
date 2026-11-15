#pragma once

#include <cstdint>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

namespace PSXRecomp {

// Precision is separate from integer value liveness. Addresses and branch
// conditions consume guest values, while stores, GTE transfers and scalar
// arithmetic consume shadow values. Unknown code may consume every shadow.
struct PgxpFlowEffect {
    uint64_t uses = 0, defines = 0;
    uint32_t destination = 0;
    bool pure_alu = false, barrier = false;
};

inline PgxpFlowEffect pgxp_flow_effect(uint32_t word) {
    constexpr uint64_t Hi = UINT64_C(1) << 32, Lo = UINT64_C(1) << 33;
    const uint32_t op = word >> 26, rs = (word >> 21) & 31u;
    const uint32_t rt = (word >> 16) & 31u, rd = (word >> 11) & 31u;
    const auto reg = [](uint32_t r) { return r ? UINT64_C(1) << r : 0; };
    PgxpFlowEffect e;
    auto alu = [&](uint32_t dst, uint64_t uses) {
        e.defines = reg(dst); e.uses = uses; e.destination = dst;
        e.pure_alu = dst != 0;
    };
    if (op == 0) {
        switch (word & 63u) {
        case 0: case 2: case 3: alu(rd, reg(rt)); return e;
        case 4: case 6: case 7: alu(rd, reg(rs) | reg(rt)); return e;
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:
            alu(rd, reg(rs) | reg(rt)); return e;
        case 0x2A: case 0x2B: alu(rd, 0); return e; // resets precision
        // These hooks also validate their source shadows. Keep them even if
        // the result is dead; do not discard a stale source-validation effect.
        case 0x10: e.uses = Hi; e.defines = reg(rd); return e;
        case 0x12: e.uses = Lo; e.defines = reg(rd); return e;
        case 0x11: e.uses = reg(rs); e.defines = Hi; return e;
        case 0x13: e.uses = reg(rs); e.defines = Lo; return e;
        case 0x18: case 0x19: case 0x1A: case 0x1B:
            e.uses = reg(rs) | reg(rt); e.defines = Hi | Lo; return e;
        default: e.barrier = true; return e;
        }
    }
    if (op >= 8 && op <= 0x0F) {
        alu(rt, op == 0x0F || op == 0x0A || op == 0x0B ? 0 : reg(rs));
        return e;
    }
    if (op == 0x10 && rs == 0) { alu(rt, 0); return e; }
    if (op >= 0x20 && op <= 0x26) {
        // Every load stays hooked: consuming a tracked word may invalidate its
        // source halves/lineage. The EA itself consumes no GPR precision.
        e.defines = reg(rt);
        // Partial word merges consume the old rt. The maintained hook currently
        // resets partially merged shadows, but do not bake that limitation into
        // the compiler's provenance proof or cross a load-delay forwarding pair.
        if (op == 0x22 || op == 0x26) e.uses = reg(rt);
        return e;
    }
    if (op == 0x28 || op == 0x29 || op == 0x2A || op == 0x2B || op == 0x2E) {
        e.uses = reg(rt); return e;
    }
    if (op == 0x12) {
        if (rs == 0 || rs == 2) e.defines = reg(rt); // MFC2/CFC2
        else if (rs == 4 || rs == 6) e.uses = reg(rt); // MTC2/CTC2
        else if (rs < 16) e.barrier = true;
        return e; // GTE commands own their existing full precision path
    }
    if (op == 0x32 || op == 0x3A || op == 2 ||
        (op >= 4 && op <= 7) || (op == 1 && rt <= 1)) return e;
    e.barrier = true;
    return e;
}

struct PgxpFlowInstruction {
    uint32_t address, word;
    bool opaque = false, removable = true;
};
struct PgxpFlowBlock {
    uint32_t address;
    std::vector<PgxpFlowInstruction> instructions;
    std::vector<uint32_t> successors;
    bool boundary = false;
};

// Least fixed point over the real function CFG. A missing edge, caller or
// unsupported instruction is an all-register consumer. No memory/GTE hook is
// removed. In-place operands are read before the destination is killed.
inline std::set<uint32_t> pgxp_dead_alu_flow(const std::vector<PgxpFlowBlock>& blocks) {
    constexpr uint64_t All = ((UINT64_C(1) << 34) - 1u) & ~UINT64_C(1);
    std::map<uint32_t, uint64_t> live_in;
    for (const auto& b : blocks) live_in[b.address] = 0;
    auto out = [&](const PgxpFlowBlock& b) {
        uint64_t live = b.boundary || b.successors.empty() ? All : 0;
        for (uint32_t target : b.successors) {
            const auto found = live_in.find(target);
            live |= found == live_in.end() ? All : found->second;
        }
        return live;
    };
    auto transfer = [&](const PgxpFlowBlock& b, std::set<uint32_t>* dead) {
        uint64_t live = out(b);
        for (auto i = b.instructions.rbegin(); i != b.instructions.rend(); ++i) {
            const auto e = pgxp_flow_effect(i->word);
            if (i->opaque || e.barrier) { live = All; continue; }
            if (i->removable && e.pure_alu && !(live & e.defines)) {
                if (dead) dead->insert(i->address);
                // Its shadow result is never consumed: neither are its shadow
                // operands. Guest integer execution still takes place normally.
                continue;
            }
            live = (live & ~e.defines) | e.uses;
        }
        return live;
    };
    bool changed = true;
    std::size_t rounds = 0;
    while (changed) {
        if (++rounds > blocks.size() * 34u + 1u) return {}; // fail closed
        changed = false;
        for (auto b = blocks.rbegin(); b != blocks.rend(); ++b) {
            const uint64_t next = transfer(*b, nullptr);
            if (next != live_in[b->address]) { live_in[b->address] = next; changed = true; }
        }
    }
    std::set<uint32_t> dead;
    for (const auto& b : blocks) transfer(b, &dead);
    return dead;
}

} // namespace PSXRecomp
