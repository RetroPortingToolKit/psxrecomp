#include "native_text.h"
#include "../third_party/json11/json11.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <set>

namespace PSXRecompV4 {
bool native_text_utf8(const std::string& text, std::vector<uint32_t>& out, std::string& error) {
    out.clear(); error.clear();
    for(size_t i=0;i<text.size();) {
        const uint8_t first=uint8_t(text[i++]);
        uint32_t cp=first, minimum=0; unsigned n=0;
        if(first>=0xc2 && first<=0xdf) {cp=first&31;n=1;minimum=0x80;}
        else if(first>=0xe0 && first<=0xef) {cp=first&15;n=2;minimum=0x800;}
        else if(first>=0xf0 && first<=0xf4) {cp=first&7;n=3;minimum=0x10000;}
        else if(first>=0x80 || first==0) {error="Invalid UTF-8 text";return false;}
        if(n>text.size()-i) {error="Truncated UTF-8 text";return false;}
        while(n--) {
            const uint8_t c=uint8_t(text[i++]);
            if((c&0xc0)!=0x80) {error="Invalid UTF-8 continuation";return false;}
            cp=(cp<<6)|(c&63);
        }
        if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) {
            error="Invalid Unicode scalar";return false;
        }
        out.push_back(cp);
    }
    return true;
}

bool NativeTextCatalog::load(const std::string& source,const std::string& language,std::string& error) {
    entries_.clear(); error.clear();
    if(source.size()>8*1024*1024) {error="Translation catalog exceeds 8 MiB";return false;}
    const auto json=json11::Json::parse(source,error);
    if(!error.empty())return false;
    if(!json.is_object() || json["schema"].int_value()!=1 ||
       json["language"].string_value().empty() ||
       (!language.empty() && json["language"].string_value()!=language) || !json["entries"].is_array()) {
        error="Expected native-text schema 1 and the selected language";return false;
    }
    std::map<std::string,NativeTextEntry> pending;
    for(const auto& item:json["entries"].array_items()) {
        NativeTextEntry e{item["id"].string_value(),item["original"].string_value(),
            item["translation"].string_value(),item["source_sha256"].string_value()};
        if(!item.is_object() || e.id.empty() || e.id.size()>160 || e.original.empty() ||
           !item["translation"].is_string() || !pending.emplace(e.id,e).second) {
            error="Invalid or duplicate translation ID: "+e.id;return false;
        }
        if(e.source_sha256.size()!=64 || !std::all_of(e.source_sha256.begin(),e.source_sha256.end(),
           [](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) {
            error="Missing source SHA-256: "+e.id;return false;
        }
        std::vector<uint32_t> check;
        if(!native_text_utf8(e.original,check,error) || !native_text_utf8(e.translation,check,error)) {
            error=e.id+": "+error;return false;
        }
    }
    entries_=std::move(pending);return true;
}
const NativeTextEntry* NativeTextCatalog::find(const std::string& id) const {
    const auto i=entries_.find(id);return i==entries_.end()?nullptr:&i->second;
}

bool native_text_encode(const std::string& text,const NativeGlyphMap& glyphs,
        const NativeTextLayout& l,std::vector<uint16_t>& words,std::string& error) {
    words.clear();error.clear();
    if(!l.columns || !l.rows || l.columns>1024 || l.rows>128) {error="Invalid native text layout";return false;}
    std::vector<uint32_t> cp;
    if(!native_text_utf8(text,cp,error))return false;
    if(cp.empty()) {error="Empty translated message";return false;}
    std::vector<uint16_t> pending;
    uint32_t column=0,row=0;
    auto next_line=[&](bool page) {
        if(pending.empty() || column==0)return false;
        if(page || row+1>=l.rows) {pending.back()|=l.page_end;row=0;}
        else {pending.back()|=l.line_end;++row;}
        column=0;return true;
    };
    for(size_t i=0;i<cp.size();++i) {
        const uint32_t c=cp[i];
        if(c=='\n' || c=='\f') {
            if(!next_line(c=='\f')) {error="Empty translated line/page";return false;}
            continue;
        }
        if(c==' ') {
            size_t n=0;while(i+1+n<cp.size() && cp[i+1+n]!=' ' && cp[i+1+n]!='\n' && cp[i+1+n]!='\f')++n;
            if(n>l.columns) {error="A translated word exceeds the native line width";return false;}
            if(!column)continue;
            if(column+1+n>l.columns) {next_line(false);continue;}
        } else if(column>=l.columns) {
            error="A translated word exceeds the native line width";return false;
        }
        uint16_t glyph=l.space;
        if(c!=' ') {
            const auto found=glyphs.find(c);
            if(found==glyphs.end()) {
                char code[32];std::snprintf(code,sizeof code,"U+%04X",unsigned(c));
                error=std::string("Unsupported native glyph ")+code;return false;
            }
            glyph=found->second;
        }
        pending.push_back(glyph|l.style|l.progressive);++column;
        if(pending.size()>32768) {error="Translated message exceeds native buffer limit";return false;}
    }
    if(pending.empty() || !column) {error="Translation ends with an empty line/page";return false;}
    pending.back()|=l.text_end|l.page_end;
    words=std::move(pending);return true;
}

bool native_font_compose(NativeFontAtlas& a,uint16_t base,uint16_t dest,
        NativeGlyphMark mark,std::string& error) {
    error.clear();
    if(!a.width || !a.height || !a.cell_width || !a.cell_height ||
       a.width%a.cell_width || a.height%a.cell_height || a.width%2 ||
       uint64_t(a.width)*a.height!=uint64_t(a.packed.size())*2 ||
       a.cell_width>64 || a.cell_height>64) {error="Invalid native font atlas";return false;}
    const uint32_t cols=a.width/a.cell_width,cells=cols*(a.height/a.cell_height);
    if(base>=cells || dest>=cells || base==dest) {error="Glyph cell outside native atlas";return false;}
    auto get=[&](unsigned id,unsigned x,unsigned y) {
        const size_t p=((id/cols)*a.cell_height+y)*a.width+(id%cols)*a.cell_width+x;
        return uint8_t((a.packed[p/2]>>((p&1)*4))&15);
    };
    std::vector<uint8_t> cell(a.cell_width*a.cell_height);
    std::array<unsigned,16> colors{};
    unsigned top=a.cell_height,bottom=0;
    for(unsigned y=0;y<a.cell_height;++y)for(unsigned x=0;x<a.cell_width;++x) {
        if(get(dest,x,y)) {error="Destination glyph cell is occupied";return false;}
        const auto v=get(base,x,y);cell[y*a.cell_width+x]=v;
        if(v) {++colors[v];top=std::min(top,y);bottom=std::max(bottom,y);}
    }
    if(top==a.cell_height) {error="Source font glyph is blank";return false;}
    const auto original=cell;
    if(mark==NativeGlyphMark::Inverted) std::reverse(cell.begin(),cell.end());
    else {
        if(a.cell_width<8 || a.cell_height<12) {error="Native cell too small for an accent";return false;}
        // Keep the native shape and baseline whenever the cell already has
        // headroom. Capitals with less than three rows borrow bottom padding.
        const unsigned shift=top<3?3-top:0;
        if(bottom+shift>=a.cell_height) {error="No native-font headroom for accent";return false;}
        if(shift) {
            std::fill(cell.begin(),cell.end(),0);
            for(unsigned y=0;y+shift<a.cell_height;++y)
                std::copy_n(original.data()+y*a.cell_width,a.cell_width,cell.data()+(y+shift)*a.cell_width);
            top+=shift;
        }
        unsigned ink=1;
        for(unsigned c=2;c<16;++c)if(colors[c]>colors[ink])ink=c;
        const unsigned x=a.cell_width/2-2,y=top>=3?top-3:0;
        auto dot=[&](unsigned px,unsigned py){cell[py*a.cell_width+px]=uint8_t(ink);};
        if(mark==NativeGlyphMark::Acute) {dot(x+2,y);dot(x+3,y);dot(x+1,y+1);dot(x+2,y+1);}
        else if(mark==NativeGlyphMark::Tilde) {dot(x,y+1);dot(x+1,y);dot(x+2,y);dot(x+3,y+1);dot(x+4,y+1);dot(x+5,y);}
        else {dot(x,y);dot(x+1,y);dot(x+4,y);dot(x+5,y);}
    }
    for(unsigned y=0;y<a.cell_height;++y)for(unsigned x=0;x<a.cell_width;++x) {
        const size_t p=((dest/cols)*a.cell_height+y)*a.width+(dest%cols)*a.cell_width+x;
        const unsigned shift=(p&1)*4;
        a.packed[p/2]=uint8_t((a.packed[p/2]&~(15u<<shift))|(cell[y*a.cell_width+x]<<shift));
    }
    return true;
}
} // namespace PSXRecompV4
