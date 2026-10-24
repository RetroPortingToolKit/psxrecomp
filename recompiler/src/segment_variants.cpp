#include "segment_variants.h"

#include <algorithm>
#include <set>

#include "fmt/format.h"

namespace PSXRecomp {

PS1Executable segment_view(const PS1Executable& exe, uint32_t seg) {
    PS1Executable view = exe;
    seg &= kSegmentMask;
    view.header.load_address = seg | (exe.header.load_address & kPhysMask);
    view.header.initial_pc = seg | (exe.header.initial_pc & kPhysMask);
    return view;
}

std::vector<uint32_t> direct_successors(const ControlFlowGraph& cfg) {
    std::set<uint32_t> out;
    auto leave = [&](uint32_t target) {
        if (target != 0 && !cfg.blocks.count(target)) out.insert(target);
    };
    for (const auto& [addr, block] : cfg.blocks) {
        (void)addr;
        const ControlFlowInstr& x = block.exit_instr;
        switch (x.type) {
            case ControlFlowType::Branch:
                leave(x.target);
                leave(x.address + 8u);
                break;
            case ControlFlowType::Jump:
                leave(x.target);
                break;
            case ControlFlowType::JumpLink:
                leave(x.target);
                leave(x.address + 8u);
                break;
            case ControlFlowType::JumpLinkReg:
                leave(x.address + 8u);
                break;
            case ControlFlowType::None:
                if (block.successors.empty()) leave(block.end_addr + 4u);
                break;
            default:  // jr / jr $ra: indirect, through dispatch
                break;
        }
    }
    return {out.begin(), out.end()};
}

namespace {

Function rebase_function(const Function& f, uint32_t seg) {
    auto at = [seg](uint32_t a) { return a ? (seg | (a & kPhysMask)) : 0u; };
    Function v = f;
    v.start_addr = at(f.start_addr);
    v.end_addr = seg | (f.end_addr & kPhysMask);
    v.name = fmt::format("func_{:08X}", v.start_addr);
    v.alias_walk_lo = at(f.alias_walk_lo);
    for (uint32_t& e : v.alias_group_entries) e = at(e);
    v.producer_lo = at(f.producer_lo);
    v.producer_hi = f.producer_hi ? (seg | (f.producer_hi & kPhysMask)) : 0u;
    return v;
}

}  // namespace

std::vector<SegmentVariantPlan> plan_segment_variants(
    const PS1Executable& exe,
    const std::vector<Function>& home_functions,
    const std::map<uint32_t, ControlFlowGraph>& home_cfgs,
    const std::map<uint32_t, uint32_t>& home_continuations,
    const std::vector<uint32_t>& seeds) {
    const uint32_t link = exe.link_segment();
    std::map<uint32_t, const Function*> by_start;
    std::map<uint32_t, std::vector<uint32_t>> alias_groups;  // host -> members
    for (const Function& f : home_functions) {
        by_start.emplace(f.start_addr, &f);
        if (f.alias_walk_lo != 0) alias_groups[f.alias_walk_lo].push_back(f.start_addr);
    }

    std::map<uint32_t, SegmentVariantPlan> plans;
    std::map<uint32_t, std::set<uint32_t>> roots;
    for (uint32_t pc : seeds) {
        const uint32_t seg = pc & kSegmentMask;
        SegmentVariantPlan& plan = plans[seg];
        plan.segment = seg;
        plan.seeds.push_back(pc);
        const uint32_t home = link | (pc & kPhysMask);
        if (!maps_physical(pc)) {
            plan.unplaced.emplace_back(pc, fmt::format(
                "segment 0x{:08X} does not map physical memory (only KUSEG below "
                "0x20000000, KSEG0 and KSEG1 do), so it is no alias of the home PC "
                "0x{:08X}", seg, home));
            continue;
        }
        uint32_t root = 0;
        if (by_start.count(home)) {
            root = home;
        } else {
            auto cont = home_continuations.find(home);
            if (cont != home_continuations.end() && by_start.count(cont->second))
                root = cont->second;
        }
        if (root == 0) {
            plan.unplaced.emplace_back(pc, fmt::format(
                "its home PC 0x{:08X} is not a dispatch entry of the home compile "
                "(no function starts there and it is no CPS continuation)", home));
            continue;
        }
        roots[seg].insert(root);
    }

    std::vector<SegmentVariantPlan> out;
    for (auto& [seg, plan] : plans) {
        std::set<uint32_t> included;
        std::vector<uint32_t> work(roots[seg].begin(), roots[seg].end());
        while (!work.empty()) {
            const uint32_t start = work.back();
            work.pop_back();
            if (!included.insert(start).second) continue;
            const Function* f = by_start.at(start);
            // Alias entries share one emitted body with their host's other
            // entries: compile the group together.
            if (f->alias_walk_lo != 0) {
                for (uint32_t member : alias_groups[f->alias_walk_lo]) work.push_back(member);
            }
            auto cfg = home_cfgs.find(start);
            if (cfg == home_cfgs.end()) continue;
            for (uint32_t succ : direct_successors(cfg->second)) {
                if (by_start.count(succ)) work.push_back(succ);
            }
        }
        for (uint32_t start : included) plan.functions.push_back(rebase_function(*by_start.at(start), seg));
        std::sort(plan.functions.begin(), plan.functions.end(),
                  [](const Function& a, const Function& b) { return a.start_addr < b.start_addr; });
        out.push_back(std::move(plan));
    }
    return out;
}

namespace {

// Every config code site the emitter matches exactly, each passed through
// `at`. Kinds matched by physical address are left as written.
template <typename At>
CodeGenConfig move_exact_config_sites(const CodeGenConfig& cfg, At at) {
    auto move_set = [&](std::set<uint32_t>& s) {
        std::set<uint32_t> moved;
        for (uint32_t a : s) moved.insert(at(a));
        s = std::move(moved);
    };
    CodeGenConfig v = cfg;
    move_set(v.ws_sprite_tag_funcs);
    move_set(v.data_shard_funcs);
    move_set(v.mod_function_entry_funcs);
    move_set(v.hot_funcs);
    move_set(v.load_charge_batch_funcs);
    move_set(v.ws_backdrop_unsquash_funcs);
    move_set(v.ws_cull_bias_sites);
    move_set(v.ws_cull_range_sites);
    move_set(v.ws_cull_a1_sites);
    move_set(v.ws_cull_screen_x_sites);
    move_set(v.ws_cull_slti_sites);
    move_set(v.ws_cull_slti_lower_sites);
    move_set(v.ws_cull_bltz_sites);
    move_set(v.ws_cull_bgez_sites);
    move_set(v.ws_cull_clip_edge_x_load_sites);
    move_set(v.ws_cull_negsub_sites);
    move_set(v.ws_cull_vxrange_sites);
    move_set(v.ws_cull_depth_sites);
    move_set(v.ws_cull_plane_nx_sites);
    move_set(v.ws_cull_xclip_load_sites);
    move_set(v.ws_cull_nclip_keep_sites);
    move_set(v.ws_cull_nclip_exact_sites);
    move_set(v.ws_cull_branch_keep_sites);
    move_set(v.ws_backdrop_x_sites);
    move_set(v.persist_init_store_sites);
    {
        std::map<uint32_t, std::array<uint32_t, 4>> moved;
        for (const auto& [func, data] : v.vsync_query_hle_funcs) moved[at(func)] = data;
        v.vsync_query_hle_funcs = std::move(moved);
    }
    v.ws_bg2d_init_func = at(v.ws_bg2d_init_func);
    v.ws_bg2d_count_site = at(v.ws_bg2d_count_site);
    v.ws_bg2d_startcol_site = at(v.ws_bg2d_startcol_site);
    v.ws_bg2d_startx_site = at(v.ws_bg2d_startx_site);
    v.ws_bg2d_stream_left_site = at(v.ws_bg2d_stream_left_site);
    v.ws_bg2d_stream_right_site = at(v.ws_bg2d_stream_right_site);
    v.ws_bg2d_bufbase_site = at(v.ws_bg2d_bufbase_site);
    v.ws_bg2d_cap_site = at(v.ws_bg2d_cap_site);
    // Physically matched, kept as written: ws_signed_x_bound_sites,
    // ws_cull_keep_sites, ws_cull_sxy_sites, ws_cull_angle_sites,
    // ws_aspect_cone.sites, draw_distance_clamp_sites.
    return v;
}

}  // namespace

CodeGenConfig rebase_codegen_config(const CodeGenConfig& cfg,
                                    const PS1Executable& exe, uint32_t seg) {
    seg &= kSegmentMask;
    const uint32_t link = exe.link_segment();
    CodeGenConfig v = move_exact_config_sites(cfg, [&](uint32_t a) {
        return (a != 0 && exe.contains_phys(a) && (a & kSegmentMask) == link)
                   ? (seg | (a & kPhysMask)) : a;
    });
    // The split pre-pass ran on the home compile: the closure is already made
    // of its final pieces.
    v.split_mid_function_targets = false;
    return v;
}

CodeGenConfig overlay_codegen_config(const CodeGenConfig& cfg,
                                     const PS1Executable& exe) {
    const uint32_t seg = exe.link_segment();
    return move_exact_config_sites(cfg, [&](uint32_t a) {
        return (a != 0 && maps_physical(a) && exe.contains_phys(a))
                   ? (seg | (a & kPhysMask)) : a;
    });
}

}  // namespace PSXRecomp
