#include "card_document.hpp"

#include "fixtures.hpp"
#include <iostream>
int main(int argc,char** argv)try {
    if(argc!=3)return 2;namespace fs=std::filesystem;
    fs::path root=fs::u8path(argv[1]),second=fs::u8path(argv[2]);fs::create_directories(root);fs::create_directories(second);
    cardstore::Roots backups{root/"primary",second/"secondary"};
    auto original=ps1card::insert(fixture::blank(),fixture::save("BESCES-01330TOMBA-00",3));
    auto target=ps1card::insert(fixture::blank(),fixture::save("OTHER-SAVE",1));
    auto a=root/"source.mcd",b=root/"target.mcr";
    cardstore::create_verified(a,ps1card::bytes(original));cardstore::create_verified(b,ps1card::bytes(target));
    carddoc::Document d[2]={carddoc::read(a),carddoc::read(b)};
    carddoc::Drag ref{0,0,d[0].revision};carddoc::drop(d,ref,1);
    if(!d[1].dirty() || d[0].dirty())throw std::runtime_error("Drag must copy to destination draft only");
    if(cardstore::read(b).data!=ps1card::bytes(target) || fs::exists(backups.primary))throw std::runtime_error("Draft edit wrote disk or backup");
    carddoc::export_save(d[1],1,root/"draft-export.mcs");
    if(fs::exists(backups.primary))throw std::runtime_error("Draft export created backups");
    bool refused=false;try{carddoc::save(d[1],backups,false);}catch(...){refused=true;}
    if(!refused || !d[1].dirty())throw std::runtime_error("Unconfirmed save allowed");
    carddoc::save(d[1],backups,true);
    if(d[1].dirty() || ps1card::parse(cardstore::read(b).data).saves.size()!=2)throw std::runtime_error("Save Card failed");
    carddoc::edit(d[1],ps1card::erase(d[1].card,1));
    if(!d[1].dirty() || ps1card::parse(cardstore::read(b).data).saves.size()!=2)throw std::runtime_error("Delete did not stay in memory");
    auto c=root/"saved-as.mcd";carddoc::save_as(d[1],c,backups,true);
    if(d[1].dirty() || d[1].snapshot.path!=cardstore::full_path(c) || cardstore::read(b).data==cardstore::read(c).data)throw std::runtime_error("Save As did not preserve original");
    refused=false;try{carddoc::save_as(d[1],b,backups,true);}catch(...){refused=true;}
    if(!refused)throw std::runtime_error("Save As replaced an existing file");
    auto before=cardstore::read(c).data;carddoc::edit(d[1],ps1card::insert(d[1].card,fixture::save("ADDED",1)));
    auto broken=backups;broken.secondary=root/"same-volume";
    refused=false;try{carddoc::save(d[1],broken,true);}catch(...){refused=true;}
    if(!refused || !d[1].dirty() || cardstore::read(c).data!=before)throw std::runtime_error("Backup failure lost draft or original");
    ++d[0].revision;refused=false;try{carddoc::drop(d,ref,1);}catch(...){refused=true;}
    if(!refused)throw std::runtime_error("Stale drag accepted");
    if(cardstore::read(a).data!=ps1card::bytes(original))throw std::runtime_error("Copy source modified");
    std::cout<<"PASS draft copy/drag/delete/export; explicit Save/Save As; cancellation and backup errors; source preserved\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
