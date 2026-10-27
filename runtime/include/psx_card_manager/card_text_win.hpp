// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PS1 title decoding and display-only metadata; never edits a save payload.
#pragma once
#include "card_storage_win.hpp"
#include <regex>
namespace cardtext {
inline std::string utf8(const std::wstring& wide) {
    int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),int(wide.size()),nullptr,0,nullptr,nullptr);
    if(n<=0)return {};
    std::string text(n,0);if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide.data(),int(wide.size()),text.data(),n,nullptr,nullptr))return {};
    return text;
}
inline std::string decode(const ps1card::Save& save) {
    const auto& raw=save.title_sjis;if(raw.empty())return "Unknown title";
    int n=MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(raw.data()),int(raw.size()),nullptr,0);
    if(n<=0)return "Unknown title";
    std::wstring wide(n,0);
    if(!MultiByteToWideChar(932,MB_ERR_INVALID_CHARS,reinterpret_cast<const char*>(raw.data()),int(raw.size()),wide.data(),n))return "Unknown title";
    bool readable=false;
    for(wchar_t& ch:wide) {
        // European PS1 titles use Shift-JIS full-width ASCII. Display equivalent
        // Latin glyphs from the existing font; keep every raw title byte intact.
        if(ch>=0xff01 && ch<=0xff5e)ch=wchar_t(ch-0xfee0);
        if(ch==0x3000)ch=L' ';
        if(ch==0xfffd || ch<32 || ch==127)return "Unknown title";
        if(ch!=L' ' && ch!=L'?')readable=true;
    }
    if(!readable)return "Unknown title";
    while(!wide.empty() && wide.back()==L' ')wide.pop_back();
    auto result=utf8(wide);return result.empty()?"Unknown title":result;
}
inline std::string hex(const ps1card::Bytes& bytes) {
    const char* digits="0123456789ABCDEF";std::string out;
    for(uint8_t b:bytes){if(!out.empty())out+=' ';out+=digits[b>>4];out+=digits[b&15];}return out;
}
inline std::string game_id(const ps1card::Save& s) {
    if(s.name.size()<12 || (s.name.rfind("BA",0) && s.name.rfind("BE",0) && s.name.rfind("BI",0)))return "Unknown";
    auto id=s.name.substr(2,10);
    if(!std::regex_match(id,std::regex("[A-Z]{4}-[0-9]{5}")))return "Unknown";
    return id;
}
} // namespace cardtext
