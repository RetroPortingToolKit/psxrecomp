#include "native_text.h"
#include <iostream>
using namespace PSXRecompV4;
static unsigned failures;
static void check(bool ok,const char* message) {
    if(!ok) {std::cerr<<"FAIL: "<<message<<'\n';++failures;}
}
int main() {
    std::string error;
    NativeTextCatalog catalog;
    const std::string entry=R"({"id":"intro.x.001","original":"Hello!","translation":"¡Hola, señor!","source_sha256":")"+std::string(64,'a')+R"("})";
    const std::string json=R"({"schema":1,"language":"es","entries":[)"+entry+"]}";
    check(catalog.load(json,"es",error),"plain Unicode JSON catalog");
    check(catalog.find("intro.x.001") && catalog.find("intro.x.001")->original=="Hello!","stable ID retains readable source");
    check(!catalog.load(json,"fr",error) && catalog.entries().empty(),"language mismatch fails without stale entries");
    check(!catalog.load(R"({"schema":1,"language":"es","entries":[)"+entry+","+entry+"]}","es",error),"ambiguous IDs rejected");
    check(!catalog.load("{bad}","es",error),"malformed JSON rejected");
    std::vector<uint32_t> points;
    check(!native_text_utf8(std::string("\xc0\xaf",2),points,error),"overlong UTF-8 rejected");
    check(!native_text_utf8(std::string("\xed\xa0\x80",3),points,error),"surrogates rejected");
    check(native_text_utf8("áñü¿¡",points,error) && points.size()==5,"Spanish scalars retained");
    NativeGlyphMap glyphs;for(unsigned c='A';c<='Z';++c)glyphs[c]=uint16_t(c);
    NativeTextLayout layout;layout.columns=5;layout.rows=2;layout.space=0x1ff;
    layout.line_end=0x4000;layout.page_end=0x2000;layout.text_end=0x8000;
    std::vector<uint16_t> words;
    check(native_text_encode("AB CD EF GH IJ KL",glyphs,layout,words,error),"word wrapping paginates native cells");
    unsigned lines=0,pages=0;
    for(auto w:words) {lines+=(w&0x4000)!=0;pages+=(w&0x2000)!=0;}
    check(lines==1 && pages==2 && (words.back()&0x8000),"line/page/final controls reach original renderer");
    check(!native_text_encode("ABCDEF",glyphs,layout,words,error) && words.empty(),"overlong native word cannot overflow box");
    check(!native_text_encode("AΩ",glyphs,layout,words,error) && words.empty(),"unsupported glyph never becomes a space");
    NativeFontAtlas atlas;atlas.width=32;atlas.height=16;atlas.packed.resize(256);
    // One distinctive native indexed cell; destination starts blank.
    atlas.packed[4*16+2]=0x33;atlas.packed[5*16+2]=0x23;
    const auto original=atlas.packed;
    check(native_font_compose(atlas,0,1,NativeGlyphMark::Acute,error),"accent composed from native indexed font");
    for(unsigned row=0;row<16;++row)for(unsigned x=0;x<8;++x)
        check(atlas.packed[row*16+x]==original[row*16+x],"original font cell preserved");
    check(atlas.packed[4*16+10]==0x33 && atlas.packed[5*16+10]==0x23,"base glyph palette and outline copied exactly");
    const auto composed=atlas.packed;
    check(!native_font_compose(atlas,0,1,NativeGlyphMark::Tilde,error) && atlas.packed==composed,"occupied glyph cannot be overwritten");
    NativeFontAtlas dotted;dotted.width=32;dotted.height=16;dotted.packed.resize(256);
    dotted.packed[2*16+2]=0x33;dotted.packed[7*16+2]=0x23;
    check(native_font_compose(dotted,0,1,NativeGlyphMark::Acute,error,4),"native i dot can be omitted before accenting");
    check(dotted.packed[2*16+2]==0x33 && dotted.packed[2*16+10]==0 && dotted.packed[7*16+10]==0x23,
          "dot removal preserves source and destination stem");
    for(auto mark:{NativeGlyphMark::Grave,NativeGlyphMark::Circumflex,NativeGlyphMark::Cedilla}) {
        NativeFontAtlas portuguese;portuguese.width=32;portuguese.height=16;portuguese.packed=original;
        check(native_font_compose(portuguese,0,1,mark,error),"Portuguese diacritic fits native cell");
        for(unsigned row=0;row<16;++row)for(unsigned x=0;x<8;++x)
            check(portuguese.packed[row*16+x]==original[row*16+x],"Portuguese composition preserves source cell");
        check(portuguese.packed[4*16+10]==0x33 && portuguese.packed[5*16+10]==0x23,"Portuguese glyph retains source pixels and palette");
    }
    std::cout<<(failures?"FAIL":"PASS")<<": native translation catalog, Unicode, layout and font composition\n";
    return failures?1:0;
}
