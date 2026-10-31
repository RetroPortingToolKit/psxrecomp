/* Real whole pack builder/loader/publisher, with deterministic disc-service
 * inputs. Separate executables select the maintained original and owned jobs.
 * The existing mod_resident_test supplies actual ISO/mod-runtime integration. */
#include "../src/mod_resident.cpp"
#include <iostream>

/* The stub read service below never creates an ISOReader. Its lifetime symbol
 * is required by the real preparation-scoped shared-reader holder. */
namespace PS1 { struct CHDState {}; ISOReader::~ISOReader() = default; }

static std::string fingerprint = "resident-whole-fixture-v1";
static std::vector<Bytes> inputs;
static std::vector<uint32_t> sizes;
static Bytes trace;
static unsigned reads, derives;
static std::string scenario;
static void check(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
static unsigned file_index(const char* name) {
    check(name && name[0]=='F' && name[1]>='0' && name[1]<='3', "unexpected disc request");
    return unsigned(name[1]-'0');
}
namespace PSXRecompV4 {
const std::string& mod_runtime_fingerprint() { return fingerprint; }
bool mod_runtime_read_disc_file_sectors(const std::string& path, uint32_t max,
    std::vector<uint8_t>& out, uint32_t& lba, uint32_t& size, std::string* error) {
    ++reads;
    const unsigned file=file_index(path.c_str());
    if ((scenario=="unreadable" && file==1) || (max && sizes[file]>max)) {
        if(error) *error="fixture disc request refused";
        return false;
    }
    out=inputs[file]; lba=20+file*8; size=sizes[file];
    return true;
}
}
extern "C" int psx_mod_disc_file_extent(const char* path, uint32_t* lba, uint32_t* size) {
    unsigned file=file_index(path); *lba=20+file*8; *size=sizes[file]; return 1;
}
extern "C" void psx_mod_counter_add(const char*, uint32_t) {}
extern "C" uint8_t psx_mod_read_byte(uint32_t) { return 0; }
extern "C" uint64_t psx_get_cycle_count(void) { return 0; }
extern "C" { uint64_t s_frame_count=0; }

static int derive_fixture(PSXResidentSink* sink,uint32_t file,const uint8_t* data,
                          uint32_t bytes,int stock,void*) {
    ++derives;
    put32(trace,file); put32(trace,bytes); put32(trace,uint32_t(stock));
    trace.insert(trace.end(),data,data+bytes);
    const Bytes before(data,data+bytes);
    for(unsigned i=0;i<21;++i) {
        Bytes derived(17+i*7,uint8_t(file*23+i));
        uint32_t meta[4]={file,i,bytes,uint32_t(stock)};
        if(!psx_resident_emit(sink,file,100+i,meta,derived.data(),uint32_t(derived.size())))return 0;
    }
    check(std::equal(before.begin(),before.end(),data),"derive input invalidated by owned-blob growth");
    // Both aliases and zero-byte outputs retain their normal metadata/order.
    if(!psx_resident_emit(sink,file,9,nullptr,data,bytes) ||
       !psx_resident_emit(sink,file,10,nullptr,nullptr,0))return 0;
    if(scenario=="wrong_file" && file==1)
        return psx_resident_emit(sink,0,11,nullptr,data,bytes);
    return !(scenario=="callback_fail" && file==1);
}

static Bytes observe(const PSXResidentPack* pack) {
    Bytes out;
    put32(out,pack?1:0); put32(out,reads); put32(out,derives);
    put32(out,uint32_t(trace.size()));out.insert(out.end(),trace.begin(),trace.end());
    if(!pack)return out;
    put32(out,psx_resident_file_count(pack));put32(out,psx_resident_modified_files(pack));
    for(uint32_t file=0;file<psx_resident_file_count(pack);++file) {
        uint32_t size=0,padded=0;
        const uint8_t* p=psx_resident_file(pack,file,&size,&padded);
        put32(out,size);put32(out,padded);put32(out,psx_resident_file_lba(pack,file));
        put32(out,uint32_t(psx_resident_file_stock(pack,file)));
        out.insert(out.end(),p,p+padded);
        uint32_t found=99;
        const auto* slice=psx_resident_find_lba(pack,20+file*8+1,padded-2048,&found);
        check(slice==p+2048 && found==file,"resident lba view changed");
    }
    put32(out,psx_resident_derived_count(pack));
    for(uint32_t i=0;i<psx_resident_derived_count(pack);++i) {
        uint32_t file,tag,meta[4],size;
        const uint8_t* p=psx_resident_derived(pack,i,&file,&tag,meta,&size);
        check(p!=nullptr,"valid derived blob reported as missing");
        put32(out,file);put32(out,tag);for(auto v:meta)put32(out,v);put32(out,size);
        if(size)out.insert(out.end(),p,p+size);
        if(tag==9)check(p==psx_resident_file(pack,file,nullptr,nullptr),"dedup pointer identity changed");
    }
    return out;
}
static void write_observation(const std::filesystem::path& path,const Bytes& bytes) {
    std::ofstream file(path,std::ios::binary);file.write((const char*)bytes.data(),std::streamsize(bytes.size()));
    check(bool(file),"cannot retain whole-job observation");
}

int main(int argc,char** argv) {
    check(argc==3,"require fresh private directory and scenario");
    const std::filesystem::path root=std::filesystem::absolute(argv[1]);scenario=argv[2];
    check(!std::filesystem::exists(root),"fixture root must be new");
    std::filesystem::create_directories(root);
    const std::string cache=(root/"cache").string();
#ifdef _WIN32
    _putenv_s("PSX_RESIDENT_CACHE",cache.c_str());
#else
    setenv("PSX_RESIDENT_CACHE",cache.c_str(),1);
#endif
    sizes={3000,3000,5000,3000};
    for(unsigned file=0;file<4;++file) {
        Bytes bytes((sizes[file]+2047u)&~2047u);
        for(unsigned i=0;i<bytes.size();++i)bytes[i]=uint8_t(i*7+(file==2?3:1));
        inputs.push_back(std::move(bytes));
    }
    std::vector<std::string> hashes;
    for(const auto& bytes:inputs)hashes.push_back(hex(sha(bytes.data(),bytes.size()).data()));
    PSXResidentFile files[4];
    for(unsigned i=0;i<4;++i)files[i]={i==0?"F0":i==1?"F1":i==2?"F2":"F3",sizes[i],hashes[i].c_str()};
    if(scenario=="modded_allow" || scenario=="modded_reject")inputs[1][23]^=0x80;
    if(scenario=="invalid_spec")files[0].stock_sha256="not-a-digest";
    PSXResidentSpec spec{};spec.struct_size=sizeof spec;spec.title="ResidentOwnedTest";
    spec.format=scenario.c_str();spec.files=files;spec.file_count=4;spec.derive=derive_fixture;
    spec.policy=scenario=="modded_reject"?PSX_RESIDENT_REQUIRE_STOCK:PSX_RESIDENT_ALLOW_MODIFIED;
    if(scenario=="size_limit")spec.max_file_bytes=1;
    if(scenario=="publish_error")std::ofstream(cache)<<"blocked cache directory";
    const bool expected=scenario=="stock" || scenario=="modded_allow" || scenario=="publish_error";
    auto* pack=psx_resident_prepare(&spec);check(bool(pack)==expected,"cold result differs");
    Bytes result=observe(pack);
    check(derives==(expected?4u:scenario=="modded_reject"?1u:
        scenario=="callback_fail" || scenario=="wrong_file"?2u:
        scenario=="unreadable"?1u:0u),"derived callback bypassed or duplicated");
    if(scenario=="invalid_spec")check(reads==0,"invalid spec read source data");
    if(pack) {
#if PSX_NATIVE_RESIDENT_PREPARATION
        check(pack->owned && pack->payload.capacity()==0 && pack->owned_blobs.size()==pack->blobs.size(),
              "cold native builder flattened or duplicated its complete payload");
#endif
        check(derives==4 && reads==4,"cold preparation bypassed an input or callback");
        if(scenario!="publish_error") {
            auto cold_view=observe(pack);
            pack=psx_resident_prepare(&spec);
            check(pack && derives==4 && reads==4,"verified cache re-executed derive/read");
            check(observe(pack)==cold_view,"warm public blob views differ");
            auto warm_view=observe(pack);result.insert(result.end(),warm_view.begin(),warm_view.end());
            // Real loader must reject corrupt payload and rebuild, preserving
            // the exact bytes of the same cache file after atomic replacement.
            const auto path=std::filesystem::path(records().back().path);
            {std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
             file.seekp(-1,std::ios::end);const char corrupt=char(0xa7);file.write(&corrupt,1);}
            pack=psx_resident_prepare(&spec);check(pack && derives==8 && reads==8,"corrupt cache was trusted");
            auto rebuilt=observe(pack);result.insert(result.end(),rebuilt.begin(),rebuilt.end());
        }
    } else {
        check(!std::filesystem::exists(root/"cache/ResidentOwnedTest"),"failed build published partial pack");
        const std::string error=records().empty()?"invalid spec":records().back().error;
        result.insert(result.end(),error.begin(),error.end());
    }
    write_observation(root/"observations.bin",result);
    std::cout<<"PASS whole resident preparation "<<scenario<<" reads="<<reads<<" callbacks="<<derives<<"\n";
}
