#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// Preserve the real frame's work outside a bounded drawing section. Matrix
// replay may change clipping and core packet layout. Tail relocation accepts
// only proved append-only packet chains attached through zero-length OT heads.
// Packet allocations may contain padding; only linked packet words are copied.
class PSXOTRelocation {
public:
    struct Node { uint32_t address, header; };
    struct Word { uint32_t address, value; };
    struct Range { uint32_t first, last; };
    std::vector<Node> prefix, core, complete;
    std::vector<Range> outputs;
    std::vector<Range> payloads;
    std::vector<Word> changes;
    std::vector<Word> prefix_payload;
    std::vector<Node> headers;
    std::vector<uint8_t> after_core;
    uint32_t cursor=0, final_cursor=0, ordering_table=0;
    uint32_t refusal=0;
    bool refuse(uint32_t line) {refusal=line;return false;}
    bool captured=false, ready=false;
    std::vector<Word> splice_writes;
    uint32_t relocated_first=0, relocated_last=0, relocated_destination=0;
    uint32_t arena_begin=0;

    static uint32_t word(const uint8_t* ram, uint32_t address) {
        uint32_t value;
        std::memcpy(&value,ram+(address&0x1fffffu),4);
        return value;
    }
    static bool walk(const uint8_t* ram, uint32_t bytes, uint32_t ot,
                     std::vector<Node>& nodes) {
        nodes.clear();
        if(bytes!=0x200000u)return false;
        uint32_t address=ot&0xffffffu;
        for(unsigned n=0;n<32768;++n) {
            if(address==0xffffffu) {
                std::vector<Range> spans;
                for(const auto& node:nodes)
                    spans.push_back({node.address,node.address+4u+(node.header>>24)*4u});
                std::sort(spans.begin(),spans.end(),[](const Range& a,const Range& b){return a.first<b.first;});
                for(size_t i=1;i<spans.size();++i)
                    if(spans[i].first<spans[i-1].last)return false;
                return true;
            }
            if((address&3u) || address>=bytes || bytes-address<4u) return false;
            const uint32_t header=word(ram,address);
            const uint32_t payload=(header>>24)*4u;
            if(payload>bytes-address-4u) return false;
            nodes.push_back({address,header});
            address=header&0xffffffu;
        }
        return false;
    }
    void clear() {captured=ready=false;after_core.clear();changes.clear();prefix_payload.clear();outputs.clear();payloads.clear();}
    bool begin(const uint8_t* ram,uint32_t bytes,uint32_t ot,uint32_t initial_cursor=0) {
        clear();ordering_table=ot;arena_begin=initial_cursor&0x1fffffu;
        if((arena_begin&3u) || arena_begin>=bytes)return false;
        if(!walk(ram,bytes,ot,prefix))return false;
        for(const auto& node:prefix)
            for(uint32_t offset=4;offset<=((node.header>>24)*4u);offset+=4)
                prefix_payload.push_back({node.address+offset,word(ram,node.address+offset)});
        return true;
    }
    bool end(const uint8_t* ram,uint32_t bytes,uint32_t current_cursor) {
        if(!walk(ram,bytes,ordering_table,core)) return false;
        // This candidate owns newly appended packets only. Reject a core that
        // reuses a prefix payload instead of silently restoring stale precision.
        for(const auto& saved:prefix_payload)
            if(word(ram,saved.address)!=saved.value)return false;
        headers=core;
        std::sort(headers.begin(),headers.end(),[](const Node& a,const Node& b){return a.address<b.address;});
        for(const auto& prior:prefix) {
            const auto current=std::lower_bound(headers.begin(),headers.end(),prior.address,
                [](const Node& node,uint32_t address){return node.address<address;});
            if(current==headers.end() || current->address!=prior.address ||
               ((current->header^prior.header)&0xff000000u))return false;
        }
        std::vector<uint32_t> prefix_addresses;
        for(const auto& node:prefix) prefix_addresses.push_back(node.address);
        std::sort(prefix_addresses.begin(),prefix_addresses.end());
        for(const auto& node:core) {
            const uint32_t length=(node.header>>24)*4u;
            if(length)payloads.push_back({node.address+4u,node.address+4u+length});
            if(length && !std::binary_search(prefix_addresses.begin(),prefix_addresses.end(),node.address))
                outputs.push_back({node.address+4u,node.address+4u+length});
        }
        std::sort(outputs.begin(),outputs.end(),[](const Range& a,const Range& b){return a.first<b.first;});
        std::vector<Range> merged;
        for(const auto& range:outputs) {
            if(!merged.empty() && range.first<=merged.back().last)
                merged.back().last=std::max(merged.back().last,range.last);
            else merged.push_back(range);
        }
        outputs.swap(merged);
        std::sort(payloads.begin(),payloads.end(),[](const Range& a,const Range& b){return a.first<b.first;});
        for(size_t i=1;i<payloads.size();++i)
            if(payloads[i].first<payloads[i-1].last)return false;
        for(const auto& node:core) {
            const auto next=std::upper_bound(payloads.begin(),payloads.end(),node.address,
                [](uint32_t address,const Range& range){return address<range.first;});
            if(next!=payloads.begin() && node.address<(next-1)->last)return false;
        }
        cursor=current_cursor;
        headers=core;
        std::sort(headers.begin(),headers.end(),[](const Node& a,const Node& b){return a.address<b.address;});
        after_core.assign(ram,ram+bytes);
        captured=true;
        return true;
    }
    bool protected_word(uint32_t address) const {
        const auto next=std::upper_bound(outputs.begin(),outputs.end(),address,
            [](uint32_t word,const Range& range){return word<range.first;});
        return next!=outputs.begin() && address<(next-1)->last;
    }
    bool prepare(const uint8_t* ram,uint32_t bytes,uint32_t producer_end=0) {
        final_cursor=producer_end&0x1fffffu;
        ready=false;changes.clear();
        if(!captured || after_core.size()!=bytes || (bytes&3u) ||
           !walk(ram,bytes,ordering_table,complete)) return false;
        for(uint32_t address=0;address<bytes;address+=4) {
            const uint32_t value=word(ram,address);
            if(value==word(after_core.data(),address)) continue;
            // Tail writes may link packets through OT/header words, but must
            // never replace the coordinates/colours emitted by the core.
            const auto payload=std::upper_bound(payloads.begin(),payloads.end(),address,
                [](uint32_t word,const Range& range){return word<range.first;});
            if(payload!=payloads.begin() && address<(payload-1)->last)return false;
            const auto header=std::lower_bound(headers.begin(),headers.end(),address,
                [](const Node& node,uint32_t word){return node.address<word;});
            if(header!=headers.end() && header->address==address &&
               ((value^header->header)&0xff000000u))return false;
            changes.push_back({address,value});
        }
        ready=true;
        return true;
    }
    bool matches(const uint8_t* ram,uint32_t bytes,uint32_t ot,uint32_t current_cursor) const {
        if(!ready || ot!=ordering_table || cursor!=current_cursor || after_core.size()!=bytes) return false;
        for(const auto& node:core)
            if(word(ram,node.address)!=node.header) return false;
        return true;
    }
    // Only append-only tail packets and ordinary zero-length OT head inserts
    // may move. This prepares all writes before touching the replay's RAM.
    bool prepare_splice(const uint8_t* ram,uint32_t bytes,uint32_t ot,
                        uint32_t current_cursor,uint32_t cursor_word) {
        refusal=0;splice_writes.clear();relocated_first=relocated_last=relocated_destination=0;
        if(!ready || ot!=ordering_table || after_core.size()!=bytes ||
           (cursor_word&0x1fffffu)>bytes-4u || word(ram,cursor_word)!=current_cursor)return refuse(__LINE__);
        const uint32_t old_first=cursor&0x1fffffu, new_first=current_cursor&0x1fffffu;
        if((old_first|new_first)&3u || old_first<arena_begin || new_first<arena_begin ||
           old_first>=bytes || new_first>=bytes)return refuse(__LINE__);
        std::vector<Node> live;
        if(!walk(ram,bytes,ot,live))return refuse(__LINE__);
        auto sorted=[](std::vector<Node> nodes){
            std::sort(nodes.begin(),nodes.end(),[](const Node&a,const Node&b){return a.address<b.address;});
            return nodes;
        };
        const auto old=sorted(core), now=sorted(live);
        auto find=[](const std::vector<Node>& nodes,uint32_t address)->const Node* {
            auto at=std::lower_bound(nodes.begin(),nodes.end(),address,
                [](const Node&n,uint32_t a){return n.address<a;});
            return at!=nodes.end() && at->address==address?&*at:nullptr;
        };
        // Static prefix data must survive; its links may point at newly emitted
        // core packets. Every zero-length OT node retains its identity.
        for(const auto& node:prefix) {
            const Node* actual=find(now,node.address);
            if(!actual || ((actual->header^node.header)&0xff000000u))return refuse(__LINE__);
        }
        for(const auto& saved:prefix_payload)
            if(word(ram,saved.address)!=saved.value)return refuse(__LINE__);
        for(const auto& node:old)if(!(node.header>>24)) {
            const Node* actual=find(now,node.address);
            if(!actual || actual->header>>24)return refuse(__LINE__);
        }
        std::vector<Node> added;
        for(const auto& node:complete)if(!find(old,node.address))added.push_back(node);
        added=sorted(added);
        if(added.empty())return refuse(__LINE__);
        uint32_t end=old_first;
        for(const auto& node:added) {
            // The original allocator can reserve more bytes than DMA reads
            // (e.g. an 8-byte DrawMode in a 16-byte allocation). Validate
            // linked packets, not uninitialized padding between allocations.
            if(node.address<end || !(node.header>>24))return refuse(__LINE__);
            end=node.address+4u+(node.header>>24)*4u;
            if(end>bytes || (final_cursor && end>final_cursor))return refuse(__LINE__);
        }
        if(final_cursor) {
            if(final_cursor<end || final_cursor<old_first || final_cursor>=bytes)return refuse(__LINE__);
            end=final_cursor;
        }
        const uint32_t length=end-old_first;
        if(length>bytes-new_first)return refuse(__LINE__);
        const uint32_t new_end=new_first+length;
        for(const auto& node:live) {
            const uint32_t node_end=node.address+4u+(node.header>>24)*4u;
            if(node.address<new_end && new_first<node_end)return refuse(__LINE__);
        }
        auto relocate=[&](uint32_t address){return new_first+(address-old_first);};
        auto is_tail=[&](uint32_t address){return address>=old_first && address<end;};
        // Map each tail-chain terminal to the corresponding CURRENT OT head.
        std::vector<Word> terminals;
        for(const auto& change:changes) {
            const Node* prior=find(old,change.address);
            if(!prior)continue;
            if(prior->header>>24 || change.value>>24 || !is_tail(change.value&0xffffffu))return refuse(__LINE__);
            const Node* current=find(now,change.address);
            if(!current || current->header>>24)return refuse(__LINE__);
            uint32_t link=change.value&0xffffffu;
            unsigned visited=0;
            while(is_tail(link)) {
                const Node* node=find(added,link);
                if(!node || ++visited>added.size())return refuse(__LINE__);
                const uint32_t next=node->header&0xffffffu;
                if(!is_tail(next)) {
                    if(next!=(prior->header&0xffffffu))return refuse(__LINE__);
                    terminals.push_back({node->address,current->header&0xffffffu});
                }
                link=next;
            }
            splice_writes.push_back({change.address,relocate(change.value&0xffffffu)});
        }
        if(terminals.empty())return refuse(__LINE__);
        // Every appended packet belongs to one proved head-insert chain.
        std::vector<uint32_t> owned;
        for(const auto& change:changes)if(find(old,change.address)) {
            uint32_t link=change.value&0xffffffu;
            while(is_tail(link)) {
                owned.push_back(link);
                if(owned.size()>added.size())return refuse(__LINE__);
                const Node* node=find(added,link);
                if(!node)return refuse(__LINE__);
                link=node->header&0xffffffu;
            }
        }
        std::sort(owned.begin(),owned.end());
        if(owned.size()!=added.size() || std::adjacent_find(owned.begin(),owned.end())!=owned.end())return refuse(__LINE__);
        for(const auto& node:added) {
            uint32_t link=node.header&0xffffffu;
            if(is_tail(link))link=relocate(link);
            else {
                auto terminal=std::find_if(terminals.begin(),terminals.end(),[&](const Word&w){return w.address==node.address;});
                if(terminal==terminals.end())return refuse(__LINE__);
                link=terminal->value;
            }
            splice_writes.push_back({relocate(node.address),(node.header&0xff000000u)|link});
            for(uint32_t offset=4;offset<=((node.header>>24)*4u);offset+=4) {
                uint32_t value=word(after_core.data(),node.address+offset);
                const auto change=std::lower_bound(changes.begin(),changes.end(),node.address+offset,
                    [](const Word& w,uint32_t address){return w.address<address;});
                if(change!=changes.end() && change->address==node.address+offset)value=change->value;
                splice_writes.push_back({relocate(node.address+offset),value});
            }
        }
        for(const auto& change:changes) {
            if(is_tail(change.address) || find(old,change.address))continue;
            if(change.address==(cursor_word&0x1fffffu)) {
                if((change.value&0x1fffffu)!=end)return refuse(__LINE__);
                splice_writes.push_back({change.address,(current_cursor&0xffe00000u)|new_end});
            } else {
                if(word(ram,change.address)!=word(after_core.data(),change.address))return refuse(__LINE__);
                splice_writes.push_back(change);
            }
        }
        relocated_first=old_first;relocated_last=end;relocated_destination=new_first;
        return true;
    }
    uint32_t shadow_destination(uint32_t address) const {
        return address>=relocated_first && address<relocated_last
            ?relocated_destination+(address-relocated_first):address;
    }
    void apply_splice(uint8_t* ram) const {
        for(const auto& change:splice_writes)std::memcpy(ram+change.address,&change.value,4);
    }
    void apply(uint8_t* ram) const {
        for(const auto& change:changes)
            std::memcpy(ram+change.address,&change.value,4);
    }
};
