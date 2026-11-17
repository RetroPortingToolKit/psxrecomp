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
                                        const std::set<uint32_t>& roots) {
    PgxpRegionPlan plan;plan.roots=roots;
    std::map<uint32_t,PgxpRegionState> outgoing;
    std::map<uint32_t,std::vector<uint32_t>> predecessors;
    for(const auto& b:blocks) { plan.entry[b.address]={};for(auto s:b.successors)predecessors[s].push_back(b.address); }
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
            for(const auto& i:b.instructions)pgxp_region_step(i,output);
            if(b.boundary)output={};
            if(plan.entry[b.address]!=input || outgoing[b.address]!=output) {
                plan.entry[b.address]=input;outgoing[b.address]=output;changed=true;
            }
        }
    }
    for(const auto& b:blocks) { auto s=plan.entry[b.address];for(const auto& i:b.instructions)if(pgxp_region_step(i,s))++plan.definitions; }
    return plan;
}
}
