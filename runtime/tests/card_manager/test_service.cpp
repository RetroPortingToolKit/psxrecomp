#include "psx_card_manager/launcher_service.hpp"
#include "fixtures.hpp"
#include <iostream>
int main(int argc,char** argv)try {
    if(argc!=3)return 2;namespace fs=std::filesystem;
    auto root=fs::u8path(argv[1]);fs::create_directories(root);fs::create_directories(fs::u8path(argv[2]));
    auto source=ps1card::insert(fixture::blank(),fixture::save("TEST-SAVE",3));
    cardstore::create_verified(root/"source.mcd",ps1card::bytes(source));
    cardstore::create_verified(root/"target.mcd",ps1card::bytes(fixture::blank()));
    auto api=psx_card_manager::launcher_service();void* ctx=api->create();if(!ctx)return 1;
    char error[1024];
    auto call=[&](RecompCardOp op,int slot,size_t entry,int to,fs::path path,bool confirmed,uint64_t rev=UINT64_MAX) {
        auto text=path.u8string();if(rev==UINT64_MAX)rev=api->view(ctx,slot)->revision;
        RecompCardCommand c{op,slot,to,confirmed?1:0,entry,rev,text.c_str()};return api->command(ctx,&c,error,sizeof(error))!=0;
    };
    auto require=[](bool x){if(!x)throw std::runtime_error("Service assertion failed");};
    require(call(RECOMP_CARD_OPEN,0,0,0,root/"source.mcd",false));
    require(call(RECOMP_CARD_OPEN,1,0,0,root/"target.mcd",false));
    require(api->view(ctx,0)->count==1 && api->view(ctx,0)->saves[0].blocks==3);
    auto revision=api->view(ctx,0)->revision;
    require(call(RECOMP_CARD_COPY,0,0,1,{},false));
    require(api->view(ctx,1)->dirty && !api->view(ctx,0)->dirty);
    require(!call(RECOMP_CARD_OPEN,1,0,0,root/"target.mcd",false));
    require(!call(RECOMP_CARD_DISCARD,1,0,0,{},false));
    require(ps1card::parse(cardstore::read(root/"target.mcd").data).saves.empty());
    require(!call(RECOMP_CARD_SAVE,1,0,0,{},true)); // backup selection mandatory
    require(call(RECOMP_CARD_BACKUP_PRIMARY,0,0,0,root,false));
    require(call(RECOMP_CARD_BACKUP_SECONDARY,0,0,0,fs::u8path(argv[2]),false));
    require(!call(RECOMP_CARD_SAVE,1,0,0,{},false));
    require(call(RECOMP_CARD_SAVE,1,0,0,{},true));
    require(!api->view(ctx,1)->dirty);
    require(call(RECOMP_CARD_EXPORT,0,0,0,root/"save.mcs",false));
    require(call(RECOMP_CARD_DELETE,1,0,0,{},true));
    require(call(RECOMP_CARD_IMPORT,1,0,0,root/"save.mcs",false));
    require(call(RECOMP_CARD_SAVE_AS,1,0,0,root/"new.mcd",true));
    require(!call(RECOMP_CARD_SAVE_AS,1,0,0,root/"new.mcd",true));
    require(!call(RECOMP_CARD_COPY,0,0,1,{},false,revision-1));
    require(!call(RECOMP_CARD_COPY,0,0,2,{},false));
    require(cardstore::read(root/"source.mcd").data==ps1card::bytes(source));
    api->destroy(ctx);std::cout<<"PASS C service adapter: snapshots/drafts, stale guards, confirmed Save, Save As, MCS, missing backups, unchanged source\n";
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
