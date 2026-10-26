#include "gpu_fmv_hd.h"
#include "psx_sdl.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#include "../src/png_write.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

static std::atomic<int> allocation_failure{-1};
static int thread_failure=-1, threads_started;
static unsigned failures;
extern "C" void *fmv_hd_test_malloc(size_t n) {
    return allocation_failure.fetch_sub(1)==0?nullptr:std::malloc(n);
}
extern "C" void *fmv_hd_test_realloc(void *p,size_t n) {
    return allocation_failure.fetch_sub(1)==0?nullptr:std::realloc(p,n);
}
extern "C" SDL_Thread *fmv_hd_test_start_thread(SDL_ThreadFunction fn,void *arg) {
    if(threads_started++==thread_failure)return nullptr;
    return SDL_CreateThread(fn,"fmv-test-decoder",arg);
}
static void check(bool ok,const char *why) {
    if(!ok){std::cerr<<why<<'\n';++failures;}
}
int main() {
    namespace fs=std::filesystem;
    const auto root=fs::temp_directory_path()/fs::path("psx-fmv-safety-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root/"movie/frames");
#ifdef _WIN32
    _putenv_s("PSX_FMV_HD_DIR",root.string().c_str());
#else
    setenv("PSX_FMV_HD_DIR",root.string().c_str(),1);
#endif
    std::ofstream(root/"liste.txt")<<"movie\n";
    FmvHdIndexHeader header{};
    std::memcpy(header.magic,"FMVHD003",8);
    header.frames=header.sig_w=header.sig_h=header.sub=header.fps_den=1;
    header.fps_num=30;header.src_w=header.src_h=header.disp_w=header.disp_h=2;
    auto index=[&](FmvHdIndexHeader h) {
        std::ofstream out(root/"movie/index.bin",std::ios::binary);
        out.write(reinterpret_cast<const char*>(&h),sizeof h);out.put(0);
        if(h.frames==2)out.put(0); /* deliberately identical source signatures */
        const int32_t lba=0;out.write(reinterpret_cast<const char*>(&lba),sizeof lba);
        if(h.frames==2){const int32_t next=10;out.write(reinterpret_cast<const char*>(&next),sizeof next);}
    };
    const uint8_t rgba[16]={0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255};
    FILE *image=std::fopen((root/"movie/frames/000000.png").string().c_str(),"wb");
    check(image && png_write_rgba(image,rgba,2,2),"create valid tiny movie frame");
    if(image)std::fclose(image);
    uint32_t original[4]={0xff000000u,0xff000000u,0xff000000u,0xff000000u};
    FmvHdImage output{};
    FmvHdTimeline timeline{1000, 1, 1, 1, 0, 0, 1};
    auto lookup=[&]{return gpu_fmv_hd_lookup(original,2,2,2,timeline,&output);};
    for(unsigned variant=0;variant<5;++variant) {
        auto bad=header;
        if(variant==0)bad.sig_w=UINT32_MAX;
        if(variant==1)bad.frames=UINT32_MAX;
        if(variant==2)bad.sub=UINT32_MAX;
        if(variant==3)bad.off_x=INT32_MAX;
        if(variant==4)bad.frames=3; // truncated signature payload
        index(bad);check(!lookup(),"malformed index falls back to original");
        gpu_fmv_hd_shutdown();
    }
    index(header);
    for(int fail=0;fail<6;++fail) {
        allocation_failure=fail;threads_started=0;
        (void)lookup();gpu_fmv_hd_shutdown();
    }
    allocation_failure=-1;
    for(int fail=0;fail<6;++fail) {
        thread_failure=fail;threads_started=0;
        check(!lookup(),"partial thread startup falls back and joins workers");
        gpu_fmv_hd_shutdown();
    }
    thread_failure=-1;
    for(int restart=0;restart<3;++restart) {
        threads_started=0;bool found=false;
        for(int i=0;i<500 && !found;++i){found=lookup()!=0;if(!found)SDL_Delay(2);}
        check(found && output.w==2 && output.h==2 && output.rgba[3]==255,
              "valid frame decodes after failure or session restart");
        gpu_fmv_hd_shutdown();gpu_fmv_hd_shutdown();
    }
#ifndef _WIN32
    /* The complete path does not name the pack. Truncating its trailing
     * separators would instead open the valid shorter root by accident. */
    const auto oversized_root=root.string()+std::string(4096,'/')+"missing";
    setenv("PSX_FMV_HD_DIR",oversized_root.c_str(),1);
    (void)lookup();
    char path_status[512]; gpu_fmv_hd_debug_json(path_status,sizeof path_status);
    check(std::strstr(path_status,"\"movies\":0")!=nullptr,
          "overlong root must not open a different truncated pack path");
    gpu_fmv_hd_shutdown();
    setenv("PSX_FMV_HD_DIR",root.string().c_str(),1);
#endif
    /* Host sleep must not advance the movie. Only captured guest cycles may
     * select the second HD subframe; loading an earlier timeline restarts it. */
    auto interpolated=header; interpolated.sub=2; index(interpolated);
    const uint8_t second[16]={255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255};
    image=std::fopen((root/"movie/frames/000001.png").string().c_str(),"wb");
    check(image && png_write_rgba(image,second,2,2),"create second movie subframe");
    if(image)std::fclose(image);
    timeline={1000,1,2,1,0,0,1};
    bool first=false;
    for(int i=0;i<200 && !first;++i){first=lookup() && output.rgba[0]==0;if(!first)SDL_Delay(2);}
    check(first,"first subframe decoded");
    SDL_Delay(50);
    check(lookup() && output.rgba[0]==0,"pause holds guest subframe despite host time");
    timeline.cycle+=600000; timeline.frame++;
    bool advanced=false;
    for(int i=0;i<200 && !advanced;++i){advanced=lookup() && output.rgba[0]==255;if(!advanced)SDL_Delay(2);}
    check(advanced,"guest cycles advance interpolated subframe");
    timeline.decode_serial++;
    check(lookup() && output.rgba[0]==0,
          "new guest MDEC decode restarts phase on identical source image");
    timeline.cycle+=600000;
    check(lookup() && output.rgba[0]==255,"phase advances again within same MDEC decode");
    timeline={1000,1,3,1,0,0,1};
    check(lookup() && output.rgba[0]==0,"state-load epoch restarts subframe");
    timeline.cycle+=600000;
    check(lookup() && output.rgba[0]==255,"post-load guest time advances subframe");
    timeline.epoch++;timeline.cycle=1000;
    check(lookup() && output.rgba[0]==0,"rollback epoch restarts subframe");
    original[0]=0xffffffffu;
    check(!lookup(),"unrecognised movie scanout falls back to native image");
    gpu_fmv_hd_shutdown();
    original[0]=0xff000000u;
    /* A SetLoc target identifies the stream, while its delivered frame
     * sectors naturally lie later on disc. */
    auto two=header;two.frames=2;two.stream_lba_hi=0;index(two);
    timeline={1000,1,4,2,0,0,2};
    bool mapped_first=false;
    for(int i=0;i<200 && !mapped_first;++i){mapped_first=lookup() && output.rgba[0]==0;if(!mapped_first)SDL_Delay(2);}
    check(mapped_first,"first identical source image uses first sector interval");
    timeline.delivered_lba=10;timeline.decode_serial++;timeline.cycle+=1000;timeline.frame++;
    bool mapped_second=false;
    for(int i=0;i<200 && !mapped_second;++i){mapped_second=lookup() && output.rgba[0]==255;if(!mapped_second)SDL_Delay(2);}
    check(mapped_second,"identical source image selects second HD frame by delivered sector");
    timeline.stream_generation++;timeline.delivered_lba=0;timeline.cycle+=1000;
    check(lookup() && output.rgba[0]==0,"CD seek generation restarts the same movie frame");
    gpu_fmv_hd_reset_tracking();
    check(lookup() && output.rgba[0]==0,"movie end/skip clears held frame identity");
    gpu_fmv_hd_shutdown();
    fs::remove_all(root);
    if(!failures)std::cout<<"PASS FMV bounds, lifecycle and guest timeline\n";
    return failures?1:0;
}
