// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Independent PS1 card format implementation. No emulator/tool code imported.
// Format reference: psx-spx, Memory Card Data Format (see docs/memory-card-manager.md).
#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ps1card {
constexpr size_t card_size=131072, block_size=8192, frame_size=128;
using Bytes=std::vector<uint8_t>;
using Image=std::array<uint8_t,card_size>;
using Icon=std::array<uint32_t,256>; // RGBA, least significant byte red
struct Save {
    std::string name, region;
    Bytes title_sjis, data;
    std::array<uint8_t,128> directory{};
    std::vector<int> blocks;
    std::vector<Icon> icons;
};
struct Card {
    Bytes image=Bytes(card_size,0); // heap storage, including during UI callbacks
    std::vector<Save> saves;
    std::vector<std::string> errors, warnings;
    std::array<bool,15> occupied{};
    int free_blocks=0;
    bool valid() const {return errors.empty();}
    bool editable() const {return valid() && warnings.empty();}
};
inline uint16_t u16(const uint8_t* p) {return uint16_t(p[0]|(p[1]<<8));}
inline uint32_t u32(const uint8_t* p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
inline void put16(uint8_t* p,uint16_t v) {p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
inline void put32(uint8_t* p,uint32_t v) {for(int n=0;n<4;++n)p[n]=uint8_t(v>>(n*8));}
inline uint8_t checksum(const uint8_t* p) {uint8_t v=0;for(int n=0;n<127;++n)v^=p[n];return v;}
inline void seal(uint8_t* p) {p[127]=checksum(p);}
inline bool free_state(uint32_t n) {return n>=0xa0 && n<=0xa3;}
inline bool active_state(uint32_t n) {return n>=0x51 && n<=0x53;}
inline std::string filename(const uint8_t* p) {
    size_t n=0;while(n<21 && p[10+n])++n;
    if(n==0 || n>20)throw std::runtime_error("Invalid PS1 file name length");
    for(size_t k=0;k<n;++k)if(p[10+k]<32 || p[10+k]>126)throw std::runtime_error("Non-ASCII PS1 file name");
    return std::string(reinterpret_cast<const char*>(p+10),n);
}
inline void presentation(Save& s) {
    if(s.name.rfind("BA",0)==0)s.region="America (file prefix)";
    else if(s.name.rfind("BE",0)==0)s.region="Europe (file prefix)";
    else if(s.name.rfind("BI",0)==0)s.region="Japan (file prefix)";
    else s.region="Unknown";
    if(s.data.size()<512 || s.data[0]!='S' || s.data[1]!='C' || s.data[2]<0x11 || s.data[2]>0x13)
        throw std::runtime_error("Unsupported or damaged save title/icon header");
    size_t n=0;while(n<64 && s.data[4+n])++n;
    s.title_sjis.assign(s.data.begin()+4,s.data.begin()+4+n);
    const int frames=s.data[2]-0x10;
    for(int f=0;f<frames;++f) {
        Icon pixels{};
        for(int i=0;i<256;++i) {
            uint8_t pair=s.data[128*(f+1)+i/2];
            int idx=(pair>>((i%2)*4))&15;
            uint16_t c=u16(s.data.data()+96+idx*2);
            auto scale=[](int v){return uint32_t((v*255+15)/31);};
            pixels[i]=scale(c&31)|(scale((c>>5)&31)<<8)|(scale((c>>10)&31)<<16)|(c?0xff000000u:0u);
        }
        s.icons.push_back(pixels);
    }
}
inline Card parse(const Bytes& input) {
    Card c;
    if(input.size()!=card_size) {c.errors.push_back("Only raw 128 KiB cards are supported; no automatic conversion or repair.");return c;}
    std::copy(input.begin(),input.end(),c.image.begin());
    if(input[0]!='M' || input[1]!='C' || input[127]!=checksum(input.data()))c.errors.push_back("Invalid card header or header checksum");
    std::array<uint32_t,16> states{};
    for(int b=1;b<=15;++b) {
        const auto* d=input.data()+b*128;states[b]=u32(d);
        if(d[127]!=checksum(d))c.errors.push_back("Directory checksum error at block "+std::to_string(b));
        if(!free_state(states[b]) && !active_state(states[b]))c.errors.push_back("Unknown allocation state at block "+std::to_string(b));
        c.occupied[b-1]=!free_state(states[b]);
        if(free_state(states[b]))++c.free_blocks;
    }
    // Remapped physical sectors require a different storage layer. Never edit
    // these unusual images as if their directory referred to raw sectors.
    for(int f=16;f<=35;++f)if(u32(input.data()+f*128)!=0xffffffffu) {
        c.warnings.push_back("Bad-sector remapping present: read-only in this editor");break;
    }
    std::set<int> owned;
    std::set<std::string> names;
    for(int head=1;head<=15;++head) {
        if(states[head]!=0x51)continue;
        try {
            Save s;const auto* first=input.data()+head*128;
            std::copy(first,first+128,s.directory.begin());s.name=filename(first);
            if(!names.insert(s.name).second)throw std::runtime_error("Duplicate PS1 file name");
            const uint32_t size=u32(first+4);
            if(size==0 || size>15*block_size || size%block_size)throw std::runtime_error("Invalid save allocation size");
            int b=head;
            while(true) {
                if(b<1 || b>15 || !owned.insert(b).second)throw std::runtime_error("Cyclic or shared block chain");
                const auto* d=input.data()+b*128;
                const uint16_t next=u16(d+8);
                if(s.blocks.empty()) {if(states[b]!=0x51)throw std::runtime_error("Invalid chain head");}
                else if(states[b]!=(next==0xffff?0x53u:0x52u))throw std::runtime_error("Invalid chain continuation");
                s.blocks.push_back(b);
                s.data.insert(s.data.end(),input.begin()+b*block_size,input.begin()+(b+1)*block_size);
                if(next==0xffff)break;
                if(next>14)throw std::runtime_error("Block pointer outside card");
                b=next+1;
            }
            if(s.data.size()!=size)throw std::runtime_error("Save size does not match block chain");
            presentation(s);c.saves.push_back(std::move(s));
        } catch(const std::exception& e) {c.errors.push_back("Block "+std::to_string(head)+": "+e.what());}
    }
    for(int b=1;b<=15;++b)if(active_state(states[b]) && !owned.count(b))c.errors.push_back("Orphan allocated block "+std::to_string(b));
    return c;
}
inline Bytes bytes(const Card& c) {return Bytes(c.image.begin(),c.image.end());}
inline const Save& selected(const Card& c,size_t n) {
    if(!c.editable() || n>=c.saves.size())throw std::runtime_error("Card is damaged, unsupported or selection is no longer valid");
    return c.saves[n];
}
inline Bytes export_mcs(const Card& c,size_t n) {
    const auto& s=selected(c,n);Bytes out(s.directory.begin(),s.directory.end());
    // A single-save file has no physical card chain; retain its complete data.
    put16(out.data()+8,0xffff);seal(out.data());out.insert(out.end(),s.data.begin(),s.data.end());return out;
}
inline Save read_mcs(const Bytes& input) {
    if(input.size()<128+block_size || input.size()>128+15*block_size || (input.size()-128)%block_size)
        throw std::runtime_error("Expected an MCS directory frame followed by complete 8 KiB save blocks");
    if(u32(input.data())!=0x51 || input[127]!=checksum(input.data()) || u32(input.data()+4)!=input.size()-128)
        throw std::runtime_error("Invalid MCS directory, checksum or length");
    Save s;std::copy(input.begin(),input.begin()+128,s.directory.begin());s.name=filename(input.data());
    s.data.assign(input.begin()+128,input.end());presentation(s);return s;
}
inline Card insert(const Card& destination,const Save& s) {
    if(!destination.editable())throw std::runtime_error("Destination card cannot be edited");
    if(s.name.empty() || s.name.size()>20 || filename(s.directory.data())!=s.name ||
       u32(s.directory.data())!=0x51 || s.directory[127]!=checksum(s.directory.data()))
        throw std::runtime_error("Invalid save file identity or directory checksum");
    for(const auto& other:destination.saves)if(other.name==s.name)
        throw std::runtime_error("A save with this internal name already exists. Nothing replaced; choose another destination card.");
    if(s.data.empty() || s.data.size()%block_size || s.data.size()>15*block_size)throw std::runtime_error("Invalid save data size");
    Save checked=s;presentation(checked);
    if(u32(s.directory.data()+4)!=s.data.size())throw std::runtime_error("Save size mismatch");
    std::vector<int> free;
    for(int b=1;b<=15;++b)if(!destination.occupied[b-1])free.push_back(b);
    size_t count=s.data.size()/block_size;
    if(count>free.size())throw std::runtime_error("Not enough free blocks on the destination card");
    Bytes out=bytes(destination);
    for(size_t k=0;k<count;++k) {
        int b=free[k];auto* d=out.data()+b*128;std::fill(d,d+128,0);
        put32(d,k==0?0x51:k+1==count?0x53:0x52);
        if(k==0) {put32(d+4,uint32_t(s.data.size()));std::copy(s.name.begin(),s.name.end(),d+10);}
        put16(d+8,k+1<count?uint16_t(free[k+1]-1):0xffff);seal(d);
        std::copy(s.data.begin()+k*block_size,s.data.begin()+(k+1)*block_size,out.begin()+b*block_size);
    }
    Card result=parse(out);if(!result.editable())throw std::runtime_error("Edited card failed structural validation");return result;
}
inline Card erase(const Card& source,size_t n) {
    const auto& s=selected(source,n);Bytes out=bytes(source);
    // BIOS-style deleted chain, with the whole save payload retained. Recovery
    // is deliberately not offered until overwritten-chain handling is tested.
    for(int b:s.blocks) {auto* d=out.data()+b*128;put32(d,u32(d)+0x50);seal(d);}
    Card result=parse(out);if(!result.editable())throw std::runtime_error("Deleted card failed structural validation");return result;
}
} // namespace ps1card
