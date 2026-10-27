// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// A card document has an immutable disk snapshot and an editable in-memory draft.
// No edit operation writes a file. Only explicit save operations cross storage.
#pragma once
#include "card_storage_win.hpp"
namespace carddoc {
struct Document {
    cardstore::Snapshot snapshot;
    ps1card::Card card;
    std::string error;
    int selected=-1;
    uint64_t revision=0;
    bool dirty()const{return error.empty() && card.valid() && snapshot.data.size()==ps1card::card_size && card.image!=snapshot.data;}
    bool loaded()const{return !snapshot.data.empty() && error.empty();}
};
inline Document read(const std::filesystem::path& path) {
    if(path.empty())throw std::runtime_error("No card selected. Use Open Card.");
    cardstore::local_volume(path);
    Document d;d.snapshot=cardstore::read(path);d.card=ps1card::parse(d.snapshot.data);return d;
}
inline void replace(Document& d,Document fresh) {fresh.revision=d.revision+1;d=std::move(fresh);}
inline void edit(Document& d,ps1card::Card next) {
    if(!next.editable())throw std::runtime_error("Invalid card draft; nothing changed");
    d.card=std::move(next);++d.revision;d.selected=-1;
}
inline void copy(const Document& source,size_t entry,Document& destination) {
    auto save=ps1card::selected(source.card,entry);
    edit(destination,ps1card::insert(destination.card,save));
    for(size_t n=0;n<destination.card.saves.size();++n)if(destination.card.saves[n].name==save.name)destination.selected=int(n);
}
inline void save(Document& d,const cardstore::Roots& roots,bool confirmed) {
    if(!confirmed)throw std::runtime_error("Save was not confirmed");
    if(!d.loaded() || !d.dirty())return;
    auto path=d.snapshot.path;auto selection=d.selected;
    cardstore::commit_existing(d.snapshot,nullptr,d.card,roots,cardstore::Fault::none,true);
    replace(d,read(path));d.selected=selection;
}
inline void save_as(Document& d,const std::filesystem::path& path,const cardstore::Roots& roots,bool confirmed) {
    if(!confirmed)throw std::runtime_error("Save As was not confirmed");
    cardstore::save_as(d.snapshot,d.card,path,roots);auto selection=d.selected;
    replace(d,read(path));d.selected=selection;
}
inline void export_save(const Document& d,size_t entry,const std::filesystem::path& path) {
    auto data=ps1card::export_mcs(d.card,entry);(void)ps1card::read_mcs(data);
    cardstore::publish(path,data,cardstore::Fault::none);
}
struct Drag {int source=-1;size_t entry=0;uint64_t revision=0;};
inline void drop(Document (&cards)[2],const Drag& drag,int target) {
    if(drag.source<0 || drag.source>1 || target<0 || target>1 || drag.source==target)throw std::runtime_error("Choose the other memory card as destination");
    if(cards[drag.source].revision!=drag.revision)throw std::runtime_error("Source changed during dragging. Drag the save again.");
    copy(cards[drag.source],drag.entry,cards[target]);
}
} // namespace carddoc
