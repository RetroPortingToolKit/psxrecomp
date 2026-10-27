#pragma once
#include "ps1_card.hpp"
namespace fixture {
inline ps1card::Card blank() {
    ps1card::Bytes b(ps1card::card_size,0);b[0]='M';b[1]='C';ps1card::seal(b.data());
    for(int n=1;n<=15;++n){auto* p=b.data()+n*128;ps1card::put32(p,0xa0);ps1card::put16(p+8,0xffff);ps1card::seal(p);}
    for(int n=16;n<=35;++n){auto* p=b.data()+n*128;ps1card::put32(p,0xffffffff);ps1card::seal(p);}return ps1card::parse(b);
}
inline ps1card::Save save(const std::string& name,int blocks=1) {
    ps1card::Save s;s.name=name;s.data.resize(blocks*8192,0x11);s.data[0]='S';s.data[1]='C';s.data[2]=0x11;s.data[3]=uint8_t(blocks);
    std::fill(s.data.begin()+4,s.data.begin()+68,0);std::memcpy(s.data.data()+4,"Test save",9);
    ps1card::put32(s.directory.data(),0x51);ps1card::put32(s.directory.data()+4,uint32_t(s.data.size()));ps1card::put16(s.directory.data()+8,0xffff);
    std::copy(name.begin(),name.end(),s.directory.begin()+10);ps1card::seal(s.directory.data());ps1card::presentation(s);return s;
}
}
