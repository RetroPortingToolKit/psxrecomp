// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Host adapter: all PS1 parsing, draft operations and persistence stay in psxrecomp.
#pragma once
#include "recomp_card_manager.h"
#if defined(_WIN32)
#include "card_document.hpp"
#include "card_text_win.hpp"
#include <functional>
#include <cstdio>
namespace psx_card_manager {
struct Service {
    carddoc::Document cards[2];
    cardstore::Roots roots;
    RecompCardView views[2]{};
    std::vector<RecompCardSave> saves[2];
    std::vector<std::array<std::string,5>> text[2];
    std::string paths[2], diagnostics[2], backup_paths[2];
    Service() {try {roots=cardstore::environment_roots();}catch(...) {}}
    const RecompCardView* view(int slot) {
        if(slot<0 || slot>1)return nullptr;
        auto& d=cards[slot];auto& v=views[slot];v={};
        paths[slot]=d.snapshot.path.u8string();diagnostics[slot].clear();
        for(const auto& s:d.card.errors)diagnostics[slot]+=s+"\n";
        for(const auto& s:d.card.warnings)diagnostics[slot]+=s+"\n";
        v.path=paths[slot].c_str();v.error=d.error.c_str();v.diagnostics=diagnostics[slot].c_str();
        v.loaded=d.loaded();v.valid=d.card.valid();v.editable=d.loaded() && d.card.editable();
        v.writable=v.editable && d.snapshot.local && !d.snapshot.readonly;v.dirty=d.dirty();
        v.free_blocks=d.card.free_blocks;v.revision=d.revision;
        for(int n=0;n<15;++n)if(d.card.occupied[n])v.used_blocks|=uint16_t(1<<n);
        text[slot].clear();saves[slot].clear();text[slot].resize(d.card.saves.size());
        for(size_t n=0;n<d.card.saves.size();++n) {
            const auto& s=d.card.saves[n];auto& t=text[slot][n];
            t={s.name,cardtext::decode(s),s.region,cardtext::game_id(s),cardtext::hex(s.title_sjis)};
            RecompCardSave out{};out.name=t[0].c_str();out.title=t[1].c_str();out.region=t[2].c_str();out.game_id=t[3].c_str();out.raw_title=t[4].c_str();
            out.bytes=s.data.size();out.blocks=unsigned(s.blocks.size());out.icon_frames=unsigned(s.icons.size());
            for(size_t f=0;f<s.icons.size();++f)std::copy(s.icons[f].begin(),s.icons[f].end(),out.icons[f]);
            saves[slot].push_back(out);
        }
        v.count=saves[slot].size();v.saves=saves[slot].data();return &v;
    }
    void command(const RecompCardCommand& c) {
        if(c.slot<0 || c.slot>1)throw std::runtime_error("Invalid card slot");
        auto& d=cards[c.slot];auto path=std::filesystem::u8path(c.path?c.path:"");
        if(c.op==RECOMP_CARD_BACKUP_PRIMARY || c.op==RECOMP_CARD_BACKUP_SECONDARY) {
            cardstore::no_reparse(path);cardstore::local_volume(path);
            (c.op==RECOMP_CARD_BACKUP_PRIMARY?roots.primary:roots.secondary)=path;return;
        }
        if(c.op==RECOMP_CARD_OPEN) {if(d.dirty() && !c.confirmed)throw std::runtime_error("Unsaved card changes must be resolved first");auto next=carddoc::read(path);carddoc::replace(d,std::move(next));return;}
        if(c.revision!=d.revision)throw std::runtime_error("Card changed. Select the save again.");
        switch(c.op) {
        case RECOMP_CARD_DISCARD: if(!c.confirmed)throw std::runtime_error("Discard was not confirmed");carddoc::replace(d,carddoc::read(d.snapshot.path));break;
        case RECOMP_CARD_COPY:
            carddoc::drop(cards,{c.slot,c.entry,c.revision},c.destination);break;
        case RECOMP_CARD_DELETE:
            if(!c.confirmed)throw std::runtime_error("Delete was not confirmed");
            carddoc::edit(d,ps1card::erase(d.card,c.entry));break;
        case RECOMP_CARD_IMPORT:
            carddoc::edit(d,ps1card::insert(d.card,ps1card::read_mcs(cardstore::read(path).data)));break;
        case RECOMP_CARD_EXPORT:carddoc::export_save(d,c.entry,path);break;
        case RECOMP_CARD_SAVE:case RECOMP_CARD_SAVE_AS:
            if(roots.primary.empty() || roots.secondary.empty())throw std::runtime_error("Choose both backup folders before saving.");
            if(c.op==RECOMP_CARD_SAVE)carddoc::save(d,roots,c.confirmed!=0);
            else carddoc::save_as(d,path,roots,c.confirmed!=0);
            break;
        default:throw std::runtime_error("Unsupported card operation");
        }
    }
};
inline const RecompCardManager* launcher_service() {
    static const RecompCardManager api={RECOMP_CARD_MANAGER_ABI,
        []()->void*{try{return new Service;}catch(...){return nullptr;}},
        [](void* p){delete static_cast<Service*>(p);},
        [](void* p,int slot)->const RecompCardView*{try{return static_cast<Service*>(p)->view(slot);}catch(...){return nullptr;}},
        [](void* p,const RecompCardCommand* c,char* error,size_t cap)->int {
            try{if(!p || !c)throw std::runtime_error("Missing card service");static_cast<Service*>(p)->command(*c);if(cap)error[0]=0;return 1;}
            catch(const std::exception& e){if(cap)std::snprintf(error,cap,"%s",e.what());return 0;}
            catch(...){if(cap)std::snprintf(error,cap,"Card operation failed");return 0;}
        },
        [](void* p,int second)->const char*{auto& s=*static_cast<Service*>(p);int n=second?1:0;s.backup_paths[n]=(n?s.roots.secondary:s.roots.primary).u8string();return s.backup_paths[n].c_str();}
    };return &api;
}
} // namespace psx_card_manager
#else
// Unsupported persistence platforms do not advertise a writable editor.
namespace psx_card_manager {inline const RecompCardManager* launcher_service(){return nullptr;}}
#endif
