// SYSTEM.CNF beyond 16 MiB wins over unrelated early cdrom: strings.
// The launcher's SBI row and the missing-SBI warning agree.
#include "disc_identity.h"
#include "disc_content_hash.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs=std::filesystem;
static void both32(uint8_t* p,uint32_t value) {
    for(unsigned i=0;i<4;i++) {p[i]=(uint8_t)(value>>(8*i));p[7-i]=p[i];}
}
// boot is the SYSTEM.CNF file name. With decoy, an unrelated "cdrom:" string
// sits in the early image; without it, a copy of the real line does, so an
// image whose TOC cannot be mounted still names its serial to the early scan.
static void fixture(const fs::path& path,bool raw,const char* boot="SLPS_009.98",bool decoy=true) {
    const unsigned unit=raw?2352:2048,offset=raw?24:0;
    std::ofstream file(path,std::ios::binary);
    file.seekp(10000ull*unit-1);file.put(0);
    if(raw) {
        uint8_t sync[16]={};std::memset(sync+1,255,10);sync[15]=2;
        file.seekp(0);file.write((char*)sync,sizeof sync);
    }
    auto write=[&](unsigned lba,const std::vector<uint8_t>& data) {
        file.seekp((uint64_t)lba*unit+offset);
        file.write((const char*)data.data(),data.size());
    };
    std::vector<uint8_t> pvd(2048);
    pvd[0]=1;std::memcpy(pvd.data()+1,"CD001",5);pvd[6]=1;
    auto* root=pvd.data()+156;root[0]=34;both32(root+2,20);both32(root+10,2048);
    root[25]=2;root[28]=root[31]=root[32]=1;
    write(16,pvd);
    const std::string cnf=std::string("BOOT = cdrom:")+boot+";1\r\n";
    std::vector<uint8_t> directory(2048);
    directory[0]=46;both32(directory.data()+2,9000);both32(directory.data()+10,(uint32_t)cnf.size());
    directory[28]=directory[31]=1;directory[32]=12;
    std::memcpy(directory.data()+33,"SYSTEM.CNF;1",12);write(20,directory);
    write(9000,std::vector<uint8_t>(cnf.begin(),cnf.end()));
    const std::string early=decoy?"BOOT = cdrom:SLUS_999.99;1\r\n":cnf;
    write(40,std::vector<uint8_t>(early.begin(),early.end()));
}
// An uncompressed CHD v5 fixture with one MODE2_RAW track. The libchdr
// decoder is the independent format reader; expected identity is the raw BIN.
static void raw_chd(const fs::path& raw,const fs::path& output) {
    const unsigned frames=(unsigned)(fs::file_size(raw)/2352), unit=2448;
    const std::string meta="TRACK:1 TYPE:MODE2_RAW SUBTYPE:NONE FRAMES:"+std::to_string(frames)+
        " PREGAP:0 PGTYPE:MODE2_RAW PGSUB:NONE POSTGAP:0";
    const unsigned meta_at=124+4*frames;
    const unsigned first=(meta_at+16+(unsigned)meta.size()+1+unit-1)/unit;
    std::vector<uint8_t> prefix((size_t)first*unit);
    auto be=[&](size_t at,uint64_t value,unsigned bytes) {
        for(unsigned i=0;i<bytes;i++)prefix[at+bytes-1-i]=(uint8_t)(value>>(8*i));
    };
    std::memcpy(prefix.data(),"MComprHD",8);be(8,124,4);be(12,5,4);
    be(32,(uint64_t)frames*unit,8);be(40,124,8);be(48,meta_at,8);
    be(56,unit,4);be(60,unit,4);
    for(unsigned i=0;i<frames;i++)be(124+4*i,first+i,4);
    std::memcpy(prefix.data()+meta_at,"CHT2",4);be(meta_at+4,meta.size()+1,4);
    std::memcpy(prefix.data()+meta_at+16,meta.c_str(),meta.size()+1);
    std::ofstream file(output,std::ios::binary);file.write((char*)prefix.data(),prefix.size());
    std::ifstream source(raw,std::ios::binary);std::vector<char> sector(unit);
    for(unsigned i=0;i<frames;i++){source.read(sector.data(),2352);file.write(sector.data(),sector.size());}
}
// The launcher's SBI File row is OK only when the disc mounted with its SBI
// loaded (ae_disc_verify: toc_opened && sbi_loaded). The missing-SBI warning
// must say the same, whether the disc is unlisted, has no .sbi, has a good one,
// or has one the reader cannot take (empty, wrong magic, cut short): that
// last case leaves the TOC unmounted, and the warning used to read it as
// "loaded".
enum class Companion {none,good,empty,bad_magic,cut_short};
static void write_companion(const fs::path& path,Companion kind) {
    std::vector<uint8_t> bytes;
    switch(kind) {
    case Companion::none: return;
    case Companion::good:       // SBI\0, then one record: MSF 00:02:20, type 1, ten Q bytes
        bytes={'S','B','I',0,0x00,0x02,0x20,0x01,0x41,0x01,0x01,0x00,0x02,0x20,0x00,0x00,0x00,0x00};break;
    case Companion::empty:      bytes={'S','B','I',0};break;
    case Companion::bad_magic:  bytes={'N','O','P','E',0x00,0x02,0x20,0x01,0x41,0x01,0x01,0x00,0x02,0x20,0x00,0x00,0x00,0x00};break;
    case Companion::cut_short:  bytes={'S','B','I',0,0x00,0x02,0x20,0x01,0x41};break;
    }
    std::ofstream file(path,std::ios::binary);
    file.write((const char*)bytes.data(),bytes.size());
}
static void sbi_row_and_warning(const fs::path& root) {
    struct Case {const char* name;bool listed;Companion companion;bool extra_track;
                 int toc;int loaded;};   // toc, loaded: 1/0 expected, -1 = only agreement
    const Case cases[]={
        {"unlisted-no-sbi",    false,Companion::none,     false, 1,0},
        {"no-sbi",             true, Companion::none,     false, 1,0},
        {"good-sbi",           true, Companion::good,     false, 1,1},
        {"empty-sbi",          true, Companion::empty,    false, 0,0},
        {"bad-magic-sbi",      true, Companion::bad_magic,false, 0,0},
        {"cut-short-sbi",      true, Companion::cut_short,false, 0,0},
        {"unlisted-empty-sbi", false,Companion::empty,    false, 0,0},
        {"track-missing",      true, Companion::none,     true, -1,-1},
        {"track-missing-sbi",  true, Companion::good,     true, -1,-1},
    };
    // The listed serial is Soul Reaver (Europe), SLES-01301, on kKnownLibcryptSerials.
    std::setvbuf(stdout,nullptr,_IONBF,0);
    int misses=0;
    const auto listed=root/"listed.bin",plain=root/"plain.bin";
    fixture(listed,true,"SLES_013.01",false);
    fixture(plain,true,"SLPS_009.98",false);
    const std::string cue_main="FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
    const std::string cue_track="FILE \"disc.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
                                "FILE \"missing track 02.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n";
    for(const Case& c:cases) {
        const auto dir=root/c.name;fs::create_directory(dir);
        fs::copy_file(c.listed?listed:plain,dir/"disc.bin");
        {std::ofstream cue(dir/"disc.cue",std::ios::binary);cue<<(c.extra_track?cue_track:cue_main);}
        write_companion(dir/"disc.sbi",c.companion);
        for(const char* pick:{"disc.cue","disc.bin"}) {
            const auto id=PSXRecompV4::identify_disc(dir/pick,"",0,false,false);
            const bool row_ok=id.toc_opened&&id.sbi_loaded;   // the row ae_disc_verify draws
            std::printf("sbi case %-18s pick=%-8s toc_opened=%d sbi_loaded=%d row_ok=%d warning=%s\n",
                        c.name,pick,id.toc_opened,id.sbi_loaded,row_ok,
                        id.sbi_warning.empty()?"(none)":id.sbi_warning.substr(0,22).c_str());
            // Count every miss so that one run lists them all, then assert once.
            auto expect=[&](bool ok,const char* what) {
                if(!ok) {++misses;std::printf("  MISS %s\n",what);}
            };
            expect(id.opened && id.has_header,"image opens with an ISO header");
            expect(id.detected_serial==(c.listed?"SLES-01301":"SLPS-00998"),"boot serial");
            if(c.toc>=0) expect(id.toc_opened==(c.toc==1),"toc_opened");
            if(c.loaded>=0) expect(id.sbi_loaded==(c.loaded==1),"sbi_loaded");
            if(c.listed) {   // warning exactly when the row is not OK
                expect(id.sbi_warning.empty()==row_ok,"row and warning agree");
                if(!row_ok) expect(id.sbi_warning.find("Missing SBI: SLES-01301")==0,"warning text");
            } else {
                expect(id.sbi_warning.empty(),"no warning for a serial off the list");
            }
        }
        fs::remove_all(dir);
    }
    fs::remove(listed);fs::remove(plain);
    assert(misses==0);
}
int main() {
    auto root=fs::temp_directory_path()/ ("psx-disc-identity-"+std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(root);
    for(bool raw:{false,true}) {
        auto image=root/(raw?"disc.bin":"disc.iso");fixture(image,raw);
        auto id=PSXRecompV4::identify_disc(image,"",0,false,false);
        assert(id.opened && id.has_header && id.detected_serial=="SLPS-00998" && id.region=="NTSC-J");
        std::string digest, error;
        assert(PSXRecompV4::disc_content_sha256(image,digest,&error));
        const std::vector<std::string> required{digest};
        assert(PSXRecompV4::required_disc_content_matches(image,required,&error));
        if(raw) {
            auto cue=root/"renamed.cue";
            {std::ofstream file(cue);file<<"FILE \"disc.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n";}
            std::string cue_digest;
            assert(PSXRecompV4::disc_content_sha256(cue,cue_digest,&error) && cue_digest==digest);
            assert(PSXRecompV4::required_disc_content_matches(cue,required,&error));
            auto chd=root/"lossless.chd";raw_chd(image,chd);
            std::string chd_digest;
            assert(PSXRecompV4::disc_content_sha256(chd,chd_digest,&error) && chd_digest==digest);
            assert(PSXRecompV4::required_disc_content_matches(chd,required,&error));
            const std::vector<std::string> required_fps{id.disc_fp};
            assert(!id.disc_fp.empty());
            assert(PSXRecompV4::identify_disc(cue,"",0,false,false).disc_fp==id.disc_fp);
            assert(PSXRecompV4::identify_disc(chd,"",0,false,false).disc_fp==id.disc_fp);
            assert(PSXRecompV4::required_disc_content_matches(image,required,&error,required_fps));
            assert(PSXRecompV4::required_disc_content_matches(cue,required,&error,required_fps));
            assert(PSXRecompV4::required_disc_content_matches(chd,required,&error,required_fps));
            auto write_cue=[&](const std::string& body){std::ofstream file(cue);file<<body;};
            const std::string original_cue="FILE \"disc.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n";
            auto audio=root/"extra-audio.bin";
            {std::ofstream f(audio,std::ios::binary);std::vector<char> silence(2352*2);f.write(silence.data(),silence.size());}
            write_cue(original_cue+"FILE \"extra-audio.bin\" BINARY\n TRACK 02 AUDIO\n INDEX 01 00:00:00\n");
            // Data-track bytes still match; the altered mounted disc must not.
            assert(PSXRecompV4::required_disc_content_matches(cue,required,&error));
            assert(!PSXRecompV4::required_disc_content_matches(cue,required,&error,required_fps));
            write_cue("FILE \"disc.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:01\n");
            assert(!PSXRecompV4::required_disc_content_matches(cue,required,&error,required_fps));
            // The current CUE reader treats data-track MODE1/2352 as the
            // same raw2352 mount; descriptive text alone is not content.
            write_cue("FILE \"disc.bin\" BINARY\n TRACK 01 MODE1/2352\n INDEX 01 00:00:00\n");
            assert(PSXRecompV4::required_disc_content_matches(cue,required,&error,required_fps));
            write_cue(original_cue);fs::remove(audio);
            assert(PSXRecompV4::required_disc_content_matches(cue,required,&error,required_fps));
            // A decoded-sector read failure must reject instead of accepting
            // a hash of the readable prefix.
            fs::resize_file(chd,fs::file_size(chd)-1000);
            assert(!PSXRecompV4::required_disc_content_matches(chd,required,&error));
            fs::remove(chd);
            // A modified non-EXE asset retains valid ISO/serial metadata. Full
            // original-image policy must reject it with every mod switched off.
            {std::fstream file(image,std::ios::binary|std::ios::in|std::ios::out);file.seekp(9999ull*2352+100);file.put(1);}
            auto modified=PSXRecompV4::identify_disc(cue,"SLPS-00998",0,false,false);
            assert(modified.has_header && modified.serial_matches);
            assert(!PSXRecompV4::required_disc_content_matches(cue,required,&error));
            assert(error.find("Use the verified original disc")!=std::string::npos);
            // Recheck bytes when the same selected filename changes.
            {std::fstream file(image,std::ios::binary|std::ios::in|std::ios::out);file.seekp(9999ull*2352+100);file.put(0);}
            assert(PSXRecompV4::required_disc_content_matches(cue,required,&error));
            fs::remove(cue);
        }
        assert(!PSXRecompV4::required_disc_content_matches(root/"missing.bin",required,&error));
        assert(!PSXRecompV4::required_disc_content_matches({},required,&error));
        assert(PSXRecompV4::required_disc_content_matches(image,{},&error));
    }
    sbi_row_and_warning(root);
    fs::remove(root/"disc.iso");fs::remove(root/"disc.bin");fs::remove(root);
}
