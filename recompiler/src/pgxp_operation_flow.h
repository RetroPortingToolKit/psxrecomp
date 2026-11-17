#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "pgxp_dataflow.h"

namespace PSXRecomp {
/* Precision summaries describe actual GTE/memory/input roots across direct
 * calls. No function is called integer-only merely because it lacks a GTE. */
constexpr uint64_t PgxpMemoryRoot = UINT64_C(1) << 32;
constexpr uint64_t PgxpGteRoot = UINT64_C(1) << 33;
constexpr uint64_t PgxpAllRoots = (UINT64_C(1) << 34) - 2;
struct PgxpOperationFunction {
    uint32_t address = 0;
    std::vector<PgxpFlowBlock> blocks;
    bool opaque = false;
};
struct PgxpOperationSummary {
    std::array<uint64_t, 32> outputs{};
    uint64_t stored = 0, geometry_inputs = 0;
    bool opaque = false, gte = false;
    bool operator==(const PgxpOperationSummary& x) const {
        return outputs == x.outputs && stored == x.stored && geometry_inputs == x.geometry_inputs && opaque == x.opaque && gte == x.gte;
    }
};
inline uint64_t pgxp_substitute(uint64_t roots, const std::array<uint64_t,32>& state) {
    uint64_t out = roots & (PgxpMemoryRoot | PgxpGteRoot);
    for (unsigned r=1;r<32;++r) if(roots & (UINT64_C(1)<<r)) out |= state[r];
    return out;
}
inline std::map<uint32_t,PgxpOperationSummary> pgxp_operation_summaries(const std::vector<PgxpOperationFunction>& functions) {
    std::map<uint32_t,PgxpOperationSummary> result;
    for(const auto& f:functions) result[f.address] = {};
    bool changed=true; unsigned round=0;
    while(changed && ++round <= functions.size()*68u+1u) {
        changed=false;
        for(const auto& f:functions) {
            PgxpOperationSummary s; s.opaque=f.opaque;
            std::map<uint32_t,std::array<uint64_t,32>> incoming;
            for(const auto& b:f.blocks) incoming[b.address] = {};
            if(f.blocks.empty()) {s.opaque=true;}
            else if(!incoming.count(f.address))s.opaque=true;
            else for(unsigned r=1;r<32;++r) incoming[f.address][r]=UINT64_C(1)<<r;
            bool local=true; unsigned iteration=0;
            while(local && ++iteration<=f.blocks.size()*68u+1u) {
                local=false;
                for(const auto& b:f.blocks) {
                    auto state=incoming[b.address];
                    for(const auto& i:b.instructions) {
                        const uint32_t op=i.word>>26,rs=(i.word>>21)&31u,rt=(i.word>>16)&31u;
                        auto effect=pgxp_flow_effect(i.word);
                        if(i.opaque || ((effect.uses|effect.defines)&((UINT64_C(1)<<32)|(UINT64_C(1)<<33))) || (effect.barrier && op!=3 && !(op==0 && (i.word&63u)==8 && rs==31))) {s.opaque=true;continue;}
                        if(op==3) {
                            const uint32_t callee=((i.address+4u)&0xf0000000u)|((i.word&0x03ffffffu)<<2);
                            const auto at=result.find(callee);
                            if(at==result.end()) {s.opaque=true;for(unsigned r=1;r<32;++r)state[r]=PgxpAllRoots;}
                            else {
                                const auto before=state; const auto& c=at->second;
                                for(unsigned r=1;r<32;++r)state[r]=pgxp_substitute(c.outputs[r],before);
                                s.stored|=pgxp_substitute(c.stored,before);s.geometry_inputs|=pgxp_substitute(c.geometry_inputs,before);
                                s.opaque|=c.opaque;s.gte|=c.gte;
                            }
                        } else if(op>=0x20 && op<=0x26) {if(rt)state[rt]=PgxpMemoryRoot;}
                        else if(op==0x28||op==0x29||op==0x2a||op==0x2b||op==0x2e) s.stored|=state[rt];
                        else if(op==0x12) {
                            s.gte=true;
                            if((rs==0||rs==2)&&rt)state[rt]=PgxpGteRoot;
                            else if(rs==4||rs==6)s.geometry_inputs|=state[rt];
                        } else if(op==0x32) {s.gte=true;s.geometry_inputs|=PgxpMemoryRoot;}
                        else if(op==0x3a) {s.gte=true;s.stored|=PgxpGteRoot;}
                        else if(effect.pure_alu)state[effect.destination]=pgxp_substitute(effect.uses,state);
                    }
                    if(b.boundary||b.successors.empty())for(unsigned r=1;r<32;++r)s.outputs[r]|=state[r];
                    for(uint32_t target:b.successors) {
                        auto at=incoming.find(target);
                        if(at==incoming.end()) {s.opaque=true;continue;}
                        for(unsigned r=1;r<32;++r){const auto v=at->second[r]|state[r];if(v!=at->second[r]){at->second[r]=v;local=true;}}
                    }
                }
            }
            if(local) s.opaque=true;
            const auto& old=result[f.address];
            for(unsigned r=1;r<32;++r)s.outputs[r]|=old.outputs[r];
            s.stored|=old.stored;s.geometry_inputs|=old.geometry_inputs;s.opaque|=old.opaque;s.gte|=old.gte;
            if(!(s==old)){result[f.address]=s;changed=true;}
        }
    }
    if(changed)for(auto& [_,s]:result)s.opaque=true;
    return result;
}

struct PgxpOperationAccess {std::string address;uint32_t kind,instruction=0;};
struct PgxpOperationPlan {
    bool valid=false;
    uint32_t reads=0,plain=0,exact=0,high=0,low=0,zero=0,alu=0,loads=0,stores=0;
    std::vector<PgxpOperationAccess> memory;
};
/* One closed native block, not a mini shadow CPU. The compiler proves the
 * final metadata kind; the host checks one borrowed read set, then publishes
 * one effect at the maintained observation boundary. Tracked inputs refuse. */
inline PgxpOperationPlan pgxp_integer_operation(const std::vector<PgxpFlowInstruction>& words) {
    enum Kind {Plain,Exact,High,Low,Zero,Unknown};
    struct Value {Kind kind=Plain; bool written=false, known=false, affine=true;uint32_t value=0,base=0,add=0;};
    std::array<Value,32> state{};state[0].known=true;
    for(unsigned r=1;r<32;++r)state[r].base=r;
    PgxpOperationPlan p;
    auto expr=[](const Value& v)->std::string {
        if(v.known)return std::to_string(v.value)+"u";
        if(!v.affine)return {};
        return "(uint32_t)(cpu->gpr["+std::to_string(v.base)+"]+"+std::to_string(v.add)+"u)";
    };
    auto use=[&](unsigned r){if(r&&!state[r].written)p.reads|=UINT32_C(1)<<r;};
    for(std::size_t n=0;n<words.size();++n) {
        const auto& i=words[n];if(i.opaque)return {};
        const uint32_t w=i.word,op=w>>26,rs=(w>>21)&31u,rt=(w>>16)&31u,rd=(w>>11)&31u,fn=w&63u,sh=(w>>6)&31u;
        const int32_t imm=(int16_t)w;
        Value out;out.written=true;out.affine=false;unsigned dst=0;
        const Value a=state[rs],b=state[rt];
        auto memory=[&](bool write,unsigned width)->bool {
            Value ea=a;if(ea.known)ea.value+=(uint32_t)imm;else ea.add+=(uint32_t)imm;
            if(ea.known) {
                const uint32_t physical=ea.value&0x1fffffffu;
                if(ea.value>=0xc0000000u || !(physical<0x00800000u ||
                    (physical>=0x1f800000u && physical<=0x1f800400u-width)))return false;
            }
            const auto address=expr(ea);if(address.empty())return false;
            p.memory.push_back({address,width|(write?0x100u:0u),write?w:0u});return true;
        };
        if(w==0)continue;
        if(op==0 && fn==8 && rs==31)continue; // original return boundary
        if(op==2||op==3||(op==1&&rt<=1)||(op>=4&&op<=7))continue;
        if(op>=0x20&&op<=0x26) {
            if((op==0x22||op==0x26)||!memory(false,op==0x20||op==0x24?1u:op==0x21||op==0x25?2u:4u))return {};
            // No deferred load value can escape this operation or feed its
            // immediate next instruction through a different architectural value.
            if(n+1>=words.size())return {};
            const uint32_t next=words[n+1].word;
            if(rt&&(((next>>21)&31u)==rt||((next>>16)&31u)==rt))return {};
            dst=rt;out.kind=(op==0x21||op==0x25)?High:Plain;p.loads+=rt!=0;
        } else if(op==0x28||op==0x29||op==0x2b) {
            if(op!=0x28 && b.kind!=Plain)return {};
            if(op!=0x28)use(rt);
            if(!memory(true,op==0x28?1u:op==0x29?2u:4u))return {};
            ++p.stores;continue;
        } else if(op==0x0f) {dst=rt;out.kind=Exact;out.known=true;out.value=(w&0xffffu)<<16;p.alu+=dst!=0;}
        else if(op==9) {
            dst=rt;use(rs);out.kind=imm==0?a.kind:a.kind==Plain?Plain:Unknown;
            out.affine=a.affine;out.base=a.base;out.add=a.add+(uint32_t)imm;
            out.known=a.known;out.value=a.value+(uint32_t)imm;
            if(imm==0)out.kind=a.kind;
            else if(a.kind==Exact&&a.known) {
                const uint32_t magnitude=(uint32_t)(imm<0?-imm:imm),lo=a.value&0xffffu;
                out.kind=(imm<0?lo>=magnitude:lo+magnitude<=0xffffu)?Exact:Plain;
            }
            p.alu+=dst!=0;
        } else if(op==0x0a||op==0x0b) {dst=rt;out.kind=Plain;p.alu+=dst!=0;}
        else if(op==0x0c||op==0x0d||op==0x0e) {
            dst=rt;use(rs);out.kind=a.kind==Plain?Zero:Unknown;
            if(a.known){out.known=true;out.value=op==0x0c?(a.value&(w&0xffffu)):op==0x0d?(a.value|(w&0xffffu)):(a.value^(w&0xffffu));}
            if(op==0x0d && (w&0xffffu))out.kind=Unknown;
            // Exact constant construction (the real LUI/ORI idiom).
            if(op==0x0d&&a.kind==Exact&&a.known&&(a.value&0xffffu)==0)out.kind=Exact;
            p.alu+=dst!=0;
        } else if(op==0) {
            dst=rd;
            if(fn==0||fn==2||fn==3) {
                use(rt);out.kind=Plain;
                if(rt && sh==16) {
                    if(b.kind==Zero||b.kind==Unknown)return {};
                    out.kind=fn==0?(b.kind==Exact||b.kind==Low?Exact:Low):(b.kind==Exact||b.kind==High?Exact:High);
                }
            } else if(fn==0x21||fn==0x23) {
                use(rs);use(rt);out.kind=a.kind==Plain&&b.kind==Plain?Plain:Unknown;
                if(rt==0||(fn==0x21&&rs==0)){out=rt==0?a:b;out.written=true;}
            } else if(fn>=0x24&&fn<=0x27) {
                use(rs);use(rt);out.kind=a.kind==Plain&&b.kind==Plain?Zero:Unknown;
                if(fn==0x25&&(rs==0||rt==0)){out=rt==0?a:b;out.written=true;}
            } else if(fn==0x2a||fn==0x2b)out.kind=Plain;
            else return {};
            p.alu+=dst!=0;
        } else return {};
        if(dst)state[dst]=out;
    }
    if(!p.alu&&!p.loads&&!p.stores)return {};
    for(unsigned r=1;r<32;++r)if(state[r].written) {
        const uint32_t bit=UINT32_C(1)<<r;
        switch(state[r].kind){case Plain:p.plain|=bit;break;case Exact:p.exact|=bit;break;case High:p.high|=bit;break;case Low:p.low|=bit;break;case Zero:p.zero|=bit;break;default:return {};}
    }
    p.valid=p.memory.size()<=256;return p;
}
} // namespace PSXRecomp
