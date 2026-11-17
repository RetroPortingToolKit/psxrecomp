#pragma once
#include "pgxp_dataflow.h"
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace PSXRecomp {
/* A compiler receipt is a complete, reconstructible GPR definition, never a
 * guess that a no-GTE function is nongeometry. Incoming affine definitions
 * retain their complete canonical receipt until the next actual boundary;
 * unsupported uses stay canonical. Integers execute as ordinary native C. */
struct PgxpRegionValue {
    bool known = false, constant = false;
    uint32_t value = 0;
    // One affine definition from the still-canonical incoming receipt. A later
    // dependent definition materializes first; never collapse intermediate
    // carry kills or ordered scalar rounding into a net integer offset.
    bool incoming_add = false;
    uint8_t source = 0;
    int16_t addend = 0;
    uint8_t flags[2] = {0,0};
    bool operator==(const PgxpRegionValue& b) const {
        return known==b.known && (!known || (constant==b.constant &&
            incoming_add==b.incoming_add && (!incoming_add || (source==b.source && addend==b.addend)) &&
            (!constant || value==b.value) && flags[0]==b.flags[0] && flags[1]==b.flags[1]));
    }
    bool operator!=(const PgxpRegionValue& b) const { return !(*this==b); }
};
using PgxpRegionState = std::array<PgxpRegionValue,32>;
struct PgxpRegionPlan {
    std::map<uint32_t,PgxpRegionState> entry;
    std::set<uint32_t> roots;
    size_t definitions = 0;
};
/* A stack address and a return PC are integer/control dependencies, not
 * automatic precision roots. This recognizer certifies only their balanced
 * frame transport; it does not classify the rest of a geometry function.
 * The backwards pass still receives real caller/opaque demands. */
struct PgxpControlFrame {
    bool valid = false;
    uint32_t allocate = 0, save = 0, restore = 0, release = 0;
    int16_t bytes = 0, offset = 0;
    std::set<uint32_t> sites() const { return {allocate,save,restore,release}; }
};
inline PgxpControlFrame pgxp_control_frame(const std::vector<PgxpFlowBlock>& blocks,
                                          uint32_t entry) {
    std::map<uint32_t,PgxpFlowInstruction> words;
    for(const auto& b:blocks)for(const auto& i:b.instructions) {
        if(auto at=words.find(i.address);at!=words.end() &&
                (at->second.word!=i.word || at->second.opaque!=i.opaque))return {};
        words[i.address]=i;
    }
    auto first=words.find(entry);
    if(first==words.end() || first->second.opaque ||
            (first->second.word&0xffff0000u)!=0x27bd0000u)return {};
    const int16_t delta=(int16_t)first->second.word;
    if(delta>=0 || delta==-32768 || (delta&3))return {};
    PgxpControlFrame frame;frame.allocate=entry;frame.bytes=(int16_t)-delta;
    for(const auto& [pc,i]:words) {
        const auto w=i.word,op=w>>26,rs=(w>>21)&31u,rt=(w>>16)&31u;
        const auto e=pgxp_flow_effect(w);
        if(pc==entry)continue;
        if(op==0x2bu && rs==29u && rt==31u) {
            if(frame.save || i.opaque || (int16_t)w<0 ||
                    ((int16_t)w&3) || (int16_t)w>frame.bytes-4)return {};
            frame.save=pc;frame.offset=(int16_t)w;continue;
        }
        if(op==0x23u && rs==29u && rt==31u) {
            if(frame.restore || i.opaque)return {};
            frame.restore=pc;continue;
        }
        if((w&0xffff0000u)==0x27bd0000u && (int16_t)w==frame.bytes) {
            if(frame.release || i.opaque)return {};
            frame.release=pc;continue;
        }
        // The architectural EA consumes integer SP only. Other stack values
        // remain fully tracked, and any SP/RA copy or geometry input refuses.
        const bool memory=(op>=0x20u&&op<=0x2eu)||op==0x32u||op==0x3au;
        const uint64_t control=(UINT64_C(1)<<29)|(UINT64_C(1)<<31);
        if(e.defines&control)return {};
        if(e.uses&control)return {}; // includes a non-RA stack store's rt
        if(rs==29u && !memory)return {};
        // JAL's implicit RA write and JR's target are control flow only.
        if(op==0u && (w&63u)==8u && rs==31u)continue;
        if(op==3u)continue;
        if((op==0u && ((w>>11)&31u)==29u) ||
                (op>=8u&&op<=15u&&rt==29u))return {};
    }
    if(!frame.save || !frame.restore || !frame.release ||
            frame.save>=frame.restore || frame.restore>=frame.release)return {};
    if((int16_t)words.at(frame.restore).word!=frame.offset)return {};
    auto jr=words.find(frame.release-4u);
    if(jr==words.end() || jr->second.word!=0x03e00008u || jr->second.opaque)return {};
    // No branch may enter the deallocation slot without executing its JR.
    for(const auto& b:blocks) {
        for(auto successor:b.successors)if(successor==frame.release)return {};
        for(const auto& i:b.instructions)if(i.word==0x03e00008u &&
                i.address!=frame.release-4u)return {};
    }
    if(frame.save>entry+8u)return {};
    frame.valid=true;return frame;
}
struct PgxpGeometryDemand {
    std::set<uint32_t> dead;
    uint64_t inputs = 0;
};
/* Full unknown demand remains the default. A caller may remove a control
 * output root only after its actual continuation/observer contract proves it
 * dead. A callee demand is its precision INPUT set, never its integer readset.
 * Missing/indirect callees and explicit opaque observations consume everything.
 * This is compile-time flow, with no run-time readset or admission walk. */
inline PgxpGeometryDemand pgxp_geometry_demand(const std::vector<PgxpFlowBlock>& blocks,
        uint32_t entry, uint64_t outputs,
        const std::map<uint32_t,uint64_t>& callees) {
    constexpr uint64_t All=((UINT64_C(1)<<34)-1u)&~UINT64_C(1);
    const auto frame=pgxp_control_frame(blocks,entry);
    std::map<uint32_t,uint64_t> live_in;
    for(const auto& b:blocks)live_in[b.address]=0;
    auto transfer=[&](const PgxpFlowBlock& b,std::set<uint32_t>* dead) {
        uint64_t live=b.successors.empty()?outputs:0;
        for(auto target:b.successors) {
            const auto at=live_in.find(target);live|=at==live_in.end()?All:at->second;
        }
        for(auto i=b.instructions.rbegin();i!=b.instructions.rend();++i) {
            const uint32_t w=i->word,op=w>>26;
            auto effect=pgxp_flow_effect(w);
            if(i->opaque){live=All;continue;}
            if(op==3u) {
                const uint32_t target=((i->address+4u)&0xf0000000u)|((w&0x3ffffffu)<<2);
                const auto at=callees.find(target);live|=at==callees.end()?All:at->second;
                continue;
            }
            if(op==0u && (w&63u)==8u && ((w>>21)&31u)==31u)continue;
            if(frame.valid && i->address==frame.save) {
                // This word is overwritten by the real CPU store. It creates
                // no geometry provenance; the write observer ends any old one.
                if(dead)dead->insert(i->address);
                continue;
            }
            if(frame.valid && i->address==frame.restore) {
                if(!(live&(UINT64_C(1)<<31))) {
                    if(dead)dead->insert(i->address);
                    continue;
                }
            }
            if(effect.barrier){live=All;continue;}
            if(i->removable && effect.pure_alu && !(live&effect.defines)) {
                if(dead)dead->insert(i->address);
                continue;
            }
            live=(live&~effect.defines)|effect.uses;
        }
        return live;
    };
    bool changed=true;size_t rounds=0;
    while(changed) {
        if(++rounds>blocks.size()*34u+1u)return {};
        changed=false;
        for(auto b=blocks.rbegin();b!=blocks.rend();++b) {
            const auto next=transfer(*b,nullptr);
            if(live_in[b->address]!=next){live_in[b->address]=next;changed=true;}
        }
    }
    PgxpGeometryDemand result;result.inputs=live_in[entry];
    for(const auto& b:blocks)transfer(b,&result.dead);
    return result;
}
inline bool pgxp_region_scalar_from_half(const PgxpRegionValue& a, unsigned mode) {
    return !a.constant || (a.flags[mode]==3 &&
        (a.value==(uint32_t)(int32_t)(int16_t)a.value || a.value==(a.value&65535u)));
}
inline bool pgxp_region_definition(uint32_t w, const PgxpRegionState& in,
                                    unsigned& dst, PgxpRegionValue& result) {
    const unsigned op=w>>26,rs=(w>>21)&31u,rt=(w>>16)&31u,fn=w&63u;
    dst=op ? rt : (w>>11)&31u;
    if(!dst)return false;
    result={}; result.known=true;
    const auto& a=in[rs];
    const auto& b=in[rt];
    if(op==15) { result.constant=true;result.value=w<<16;result.flags[0]=result.flags[1]=3;return true; }
    if(op==10 || op==11 || (!op&&(fn==42||fn==43)))return true;
    // A truly plain incoming value has neither component nor scalar precision.
    const auto plain=[](const PgxpRegionValue& x){return x.known&&!x.incoming_add&&!x.flags[0]&&!x.flags[1];};
    if((op==8||op==9) && (int16_t)w!=0 && (rs==0 || plain(a))) {
        if(rs==0) { result.constant=true;result.value=(uint32_t)(int32_t)(int16_t)w; }
        else if(a.constant) { result.constant=true;result.value=a.value+(int16_t)w; }
        return true;
    }
    if(!op && (fn==32||fn==33||fn==34||fn==35) && rs && rt && plain(a)&&plain(b))return true;
    // LUI/address construction: scalar-bearing 16-bit values are not admitted.
    if((op==8||op==9) && a.known&&a.constant && (int16_t)w!=0 &&
            !pgxp_region_scalar_from_half(a,1)) {
        result.constant=true; result.value=a.value+(int16_t)w;
        const int32_t immediate=(int16_t)w;
        const uint32_t magnitude=uint32_t(immediate<0 ? -immediate : immediate);
        const uint32_t lo=immediate<0 ? (a.value&65535u)-magnitude : (a.value&65535u)+magnitude;
        const bool independent=(lo>>16)==0; // canonical unsigned half carry/borrow
        result.flags[0]=0;result.flags[1]=independent&&a.flags[1]?3:0;
        return true;
    }
    if((op==12||op==13||op==14) && a.known&&a.constant) {
        result.constant=true;
        const uint32_t imm=w&65535u;
        result.value=op==12 ? a.value&imm : op==13 ? a.value|imm : a.value^imm;
        for(unsigned mode=0;mode<2;++mode)for(unsigned half=0;half<2;++half) {
            const unsigned shift=half*16u,flag=1u<<half;
            if(((a.flags[mode]&flag) && !(((a.value^result.value)>>shift)&0x7ffu)) ||
                    !((result.value>>shift)&65535u))result.flags[mode]|=flag;
        }
        if(op==13 && !(a.value&65535u))result.flags[0]|=1,result.flags[1]|=1;
        return true;
    }
    // Unknown is not plain: retain the full incoming receipt until publication.
    // Only ADDIU is eligible here; trapping ADDI and dependent affine chains
    // keep the maintained canonical path. No entry readset/admission is needed.
    if(op==9 && rs && !a.known && (int16_t)w!=0) {
        result.incoming_add=true;result.source=(uint8_t)rs;result.addend=(int16_t)w;
        return true;
    }
    return false;
}
/* Return true only when the original hook can disappear. Every other precision
 * operation/observer first consumes a complete receipt and resets the analysis
 * to opaque; it may mutate any CPU/precision state synchronously. */
inline bool pgxp_region_step(const PgxpFlowInstruction& i, PgxpRegionState& state) {
    const auto e=pgxp_flow_effect(i.word);
    unsigned dst=0;PgxpRegionValue value;
    if(!i.opaque && e.pure_alu && pgxp_region_definition(i.word,state,dst,value)) {
        state[dst]=value;return true;
    }
    if(!i.opaque && !e.uses && !e.defines && !e.barrier && (i.word>>26)<0x10u)return false;
    state={};return false;
}
inline PgxpRegionPlan pgxp_region_plan(const std::vector<PgxpFlowBlock>& blocks,
                                        const std::set<uint32_t>& roots,
                                         const std::set<uint32_t>& discarded = {}) {
    PgxpRegionPlan plan;plan.roots=roots;
    std::map<uint32_t,PgxpRegionState> outgoing;
    std::map<uint32_t,std::vector<uint32_t>> predecessors;
    for(const auto& b:blocks) { plan.entry[b.address]={};for(auto s:b.successors)predecessors[s].push_back(b.address); }
    auto step=[&](const PgxpFlowInstruction& i,PgxpRegionState& state) {
        if(discarded.count(i.address)) {
            const auto effect=pgxp_flow_effect(i.word);
            if(effect.pure_alu)state[effect.destination]={};
            else state={}; // real memory observer still sees other live receipts
            return false;
        }
        return pgxp_region_step(i,state);
    };
    bool changed=true;size_t rounds=0;
    while(changed) {
        if(++rounds>blocks.size()*128u+1u)return {}; // finite lattice proof failed: canonical only
        changed=false;
        for(const auto& b:blocks) {
            PgxpRegionState input{};
            if(!roots.count(b.address) && !predecessors[b.address].empty()) {
                bool first=true;
                for(auto pred:predecessors[b.address]) {
                    const auto& s=outgoing[pred];
                    if(first){input=s;first=false;}
                    else for(unsigned r=1;r<32;++r)if(input[r]!=s[r])input[r]={};
                }
            }
            // An affine input refers to the OLD canonical source receipt.
            // Publish it on every CFG edge before any partial normalization can
            // overwrite that source while carrying the dependent destination.
            // Constants/plain recipes retain their existing inter-block carry.
            for(unsigned r=1;r<32;++r)if(input[r].incoming_add)input[r]={};
            auto output=input;
            for(const auto& i:b.instructions)step(i,output);
            if(b.boundary)output={};
            if(plan.entry[b.address]!=input || outgoing[b.address]!=output) {
                plan.entry[b.address]=input;outgoing[b.address]=output;changed=true;
            }
        }
    }
    for(const auto& b:blocks) { auto s=plan.entry[b.address];for(const auto& i:b.instructions)if(step(i,s))++plan.definitions; }
    plan.definitions+=discarded.size();
    return plan;
}
}
