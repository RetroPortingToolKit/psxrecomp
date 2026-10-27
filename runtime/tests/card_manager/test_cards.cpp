#include "card_storage_win.hpp"
#include <iostream>
#include <functional>
using namespace ps1card;
static int checks=0;
static void check(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
static void rejected(std::function<void()> f,const char* why){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,why);}
static Card empty_card() {
    Bytes b(card_size,0);b[0]='M';b[1]='C';seal(b.data());
    for(int n=1;n<=15;++n){auto* d=b.data()+n*128;put32(d,0xa0);put16(d+8,0xffff);seal(d);}
    for(int n=16;n<=35;++n){auto* d=b.data()+n*128;put32(d,0xffffffff);seal(d);}
    return parse(b);
}
static Save sample(std::string name,int blocks,int frames=3) {
    Save s;s.name=name;s.data.resize(blocks*block_size,0x5a);s.data[0]='S';s.data[1]='C';s.data[2]=uint8_t(0x10+frames);s.data[3]=uint8_t(blocks);
    std::fill(s.data.begin()+4,s.data.begin()+68,0);std::memcpy(s.data.data()+4,"Test save",9);
    for(int i=0;i<16;++i)put16(s.data.data()+96+i*2,uint16_t(i*2));
    put32(s.directory.data(),0x51);put32(s.directory.data()+4,uint32_t(s.data.size()));put16(s.directory.data()+8,0xffff);
    std::copy(name.begin(),name.end(),s.directory.begin()+10);seal(s.directory.data());presentation(s);return s;
}
int main(int argc,char** argv)try {
    auto blank=empty_card();check(blank.editable()&&blank.free_blocks==15,"Empty card");
    auto a=insert(blank,sample("BESCES-01330TOMBA-00",3));
    check(a.valid()&&a.saves.size()==1&&a.saves[0].blocks.size()==3&&a.free_blocks==12,"Multi-block allocation");
    check(a.saves[0].icons.size()==3 && a.saves[0].title_sjis.size()==9,"Icon frames and title");
    auto mcs=export_mcs(a,0);auto s=read_mcs(mcs);check(s.data==a.saves[0].data&&s.name==a.saves[0].name,"MCS lossless export/import");
    auto b=insert(a,sample("BASCUS-94236TOMBA-01",2,1));
    check(b.saves.size()==2&&b.free_blocks==10,"Two different saves");
    auto c=erase(b,0);check(c.saves.size()==1&&c.free_blocks==13&&c.saves[0].data==b.saves[1].data,"Whole chain deletion preserves other save");
    check(std::equal(b.image.begin()+8192,b.image.begin()+4*8192,c.image.begin()+8192),"Deleted payload retained");
    auto round=insert(empty_card(),s);check(round.saves[0].data==s.data,"Copy data equality");
    auto full=insert(blank,sample("BISCPS-00000FULL",15));check(full.free_blocks==0,"Full card");
    rejected([&]{insert(full,sample("OTHER",1));},"Full card must reject");
    rejected([&]{insert(a,s);},"Name conflict must reject without replacement");
    auto oversized=s;oversized.name=std::string(200,'X');
    rejected([&]{insert(blank,oversized);},"Oversized save identity must reject");
    auto bad=bytes(a);bad[255]^=1;check(!parse(bad).valid(),"Directory checksum");
    bad=bytes(a);put16(bad.data()+128+8,0);seal(bad.data()+128);check(!parse(bad).valid(),"Cyclic chain");
    bad=bytes(a);put16(bad.data()+128+8,15);seal(bad.data()+128);check(!parse(bad).valid(),"Out of range chain");
    bad=bytes(a);put16(bad.data()+128+8,0xffff);seal(bad.data()+128);check(!parse(bad).valid(),"Truncated chain");
    bad=bytes(b);std::copy(bad.begin()+138,bad.begin()+159,bad.begin()+4*128+10);seal(bad.data()+4*128);check(!parse(bad).valid(),"Duplicate names");
    bad=bytes(a);bad[0]=0;check(!parse(bad).valid(),"Bad header");
    check(!parse(Bytes(100,0)).valid(),"Wrong card length");
    bad=mcs;bad.pop_back();rejected([&]{read_mcs(bad);},"Truncated MCS");
    bad=mcs;bad[127]^=1;rejected([&]{read_mcs(bad);},"MCS checksum");
    bad=mcs;put32(bad.data()+4,8192);seal(bad.data());rejected([&]{read_mcs(bad);},"MCS declared size");
    bad=bytes(a);put32(bad.data()+16*128,64);seal(bad.data()+16*128);check(!parse(bad).editable(),"Remapping read-only");
    // Deterministic malformed-input corpus: a parser must not loop or index
    // past its buffers on arbitrary links, flags, sizes or icon headers.
    for(unsigned n=0;n<2000;++n) {
        bad=bytes(a);uint32_t x=n*1664525u+1013904223u;
        for(int k=0;k<8;++k){x=x*1664525u+1013904223u;bad[x%bad.size()]^=uint8_t(x>>24);}
        (void)parse(bad);
    }
    check(true,"Malformed input corpus");
    if(argc==3) {
        namespace fs=std::filesystem;fs::path root=fs::u8path(argv[1]),second=fs::u8path(argv[2]);
        fs::create_directories(root);fs::create_directories(second);
        auto source=root/"source.mcd";cardstore::create_verified(source,bytes(b));
        auto snap=cardstore::read(source);cardstore::Roots roots{root/"backup-primary",second/"backup-secondary"};
        auto changed=erase(b,0);auto output=root/"new-card.mcd";
        cardstore::commit_new(snap,nullptr,changed,output,roots,cardstore::Fault::none);
        check(cardstore::read(source).data==snap.data,"Original unchanged on success");
        check(cardstore::read(output).data==bytes(changed),"Published full card");
        rejected([&]{cardstore::commit_new(snap,nullptr,changed,source,roots,cardstore::Fault::none);},"Cannot overwrite source");
        rejected([&]{cardstore::commit_new(snap,nullptr,changed,output,roots,cardstore::Fault::none);},"Cannot overwrite existing target");
        auto fail=root/"backup-failure.mcd";
        rejected([&]{cardstore::commit_new(snap,nullptr,changed,fail,roots,cardstore::Fault::backup);},"Backup failure");check(!fs::exists(fail),"Backup failure no output");
        fail=root/"interrupted.mcd";
        rejected([&]{cardstore::commit_new(snap,nullptr,changed,fail,roots,cardstore::Fault::before_publish);},"Interruption");check(!fs::exists(fail),"No partial final card");
        check(cardstore::read(source).data==snap.data,"Original unchanged after failures");
        auto same=roots;same.secondary=root/"same-medium";
        rejected([&]{cardstore::commit_new(snap,nullptr,changed,root/"same.mcd",same);},"Second storage required");
        {cardstore::Locked held(source);rejected([&]{cardstore::read(source);},"Second reader/writer blocked");}
        check(SetFileAttributesW(cardstore::native(source).c_str(),FILE_ATTRIBUTE_READONLY)!=0,"Set read-only fixture");
        rejected([&]{cardstore::commit_new(snap,nullptr,changed,root/"readonly.mcd",roots,cardstore::Fault::none);},"Read-only edit refused");
        SetFileAttributesW(cardstore::native(source).c_str(),FILE_ATTRIBUTE_NORMAL);
        rejected([&]{cardstore::read(root/"missing.mcd");},"Missing card not created");
        check(!fs::exists(root/"missing.mcd"),"Missing remains missing");
        rejected([&]{cardstore::local_volume(L"\\\\invalid-test-host\\missing\\card.mcd");},"NAS output refused");
        auto export_path=root/"export.mcs";cardstore::export_save(snap,0,export_path);
        check(read_mcs(cardstore::read(export_path).data).data==b.saves[0].data,"Disk MCS round trip");
        auto target=root/"destination.mcr";cardstore::create_verified(target,bytes(blank));
        auto target_snap=cardstore::read(target);
        auto copied=insert(blank,selected(b,0));
        cardstore::commit_new(target_snap,&snap,copied,root/"copied.mcd",roots,cardstore::Fault::none);
        check(parse(cardstore::read(root/"copied.mcd").data).saves[0].data==b.saves[0].data,"Transactional cross-card copy");
        auto imported_source=cardstore::read(export_path);
        cardstore::commit_new(target_snap,&imported_source,insert(blank,read_mcs(imported_source.data)),root/"imported.mcd",roots,cardstore::Fault::none);
        check(parse(cardstore::read(root/"imported.mcd").data).saves[0].data==b.saves[0].data,"Transactional MCS import");
        check(cardstore::read(target).data==target_snap.data && cardstore::read(source).data==snap.data,"Both original cards preserved");
        auto unavailable=roots;unavailable.secondary=root/"a-regular-file";
        cardstore::create_verified(unavailable.secondary,Bytes(10,0));
        rejected([&]{cardstore::commit_new(snap,nullptr,changed,root/"bad-backup.mcd",unavailable);},"Invalid second backup destination");
        check(!fs::exists(root/"bad-backup.mcd"),"Invalid backup cannot publish");
        auto mismatch=snap;mismatch.data[100]^=1;
        rejected([&]{cardstore::commit_new(mismatch,nullptr,changed,root/"stale.mcd",roots,cardstore::Fault::none);},"Stale card snapshot");
        // Direct transactions are allowed only on isolated artificial cards.
        auto direct=root/"direct.mcd";cardstore::create_verified(direct,bytes(b));
        auto direct_snap=cardstore::read(direct);
        cardstore::commit_existing(direct_snap,nullptr,changed,roots,cardstore::Fault::none,true);
        check(cardstore::read(direct).data==bytes(changed),"Direct atomic working-card update");
        rejected([&]{cardstore::commit_existing(direct_snap,nullptr,changed,roots,cardstore::Fault::none,true);},"Stale direct snapshot refused");
        direct_snap=cardstore::read(direct);
        rejected([&]{cardstore::commit_existing(direct_snap,nullptr,b,roots,cardstore::Fault::replace,true);},"Replacement failure refused");
        check(cardstore::read(direct).data==direct_snap.data,"Failed update preserves whole old card");
        auto restored=insert(changed,selected(b,0));
        cardstore::commit_existing(direct_snap,&snap,restored,roots,cardstore::Fault::none,true);
        check(parse(cardstore::read(direct).data).saves.size()==2,"Direct cross-card copy");
        direct_snap=cardstore::read(direct);
        auto without_first=erase(parse(direct_snap.data),0);
        cardstore::commit_existing(direct_snap,nullptr,without_first,roots,cardstore::Fault::none,true);
        direct_snap=cardstore::read(direct);
        cardstore::commit_existing(direct_snap,&imported_source,insert(without_first,read_mcs(imported_source.data)),roots,cardstore::Fault::none,true);
        check(parse(cardstore::read(direct).data).saves[1].data==b.saves[1].data,"Direct import preserves other saves");
        direct_snap=cardstore::read(direct);
        auto files=[](const fs::path& p){size_t n=0;for(const auto& x:fs::directory_iterator(p))if(x.is_regular_file())++n;return n;};
        auto before=files(roots.primary);
        (void)cardstore::read(direct);cardstore::export_save(direct_snap,0,root/"direct-export.mcs");
        check(files(roots.primary)==before,"Read refresh and export create no backups");
        {cardstore::Locked held(direct);cardstore::backups(roots,held,cardstore::Fault::none);before=files(roots.primary);cardstore::backups(roots,held,cardstore::Fault::none);}
        check(files(roots.primary)==before,"Identical backup versions reused");
        SetFileAttributesW(cardstore::native(direct).c_str(),FILE_ATTRIBUTE_READONLY);
        rejected([&]{cardstore::commit_existing(direct_snap,nullptr,changed,roots,cardstore::Fault::none,true);},"Direct read-only card refused");
        SetFileAttributesW(cardstore::native(direct).c_str(),FILE_ATTRIBUTE_NORMAL);
        rejected([&]{cardstore::commit_existing(direct_snap,nullptr,b,roots);},"Unconfirmed writes refused");
    }
    std::cout<<"PASS "<<checks<<" assertions; 2000 malformed images; no production writes\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
