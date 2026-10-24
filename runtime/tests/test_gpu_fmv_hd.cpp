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
    std::memcpy(header.magic,"FMVHD002",8);
    header.frames=header.sig_w=header.sig_h=header.sub=header.fps_den=1;
    header.fps_num=30;header.src_w=header.src_h=header.disp_w=header.disp_h=2;
    auto index=[&](FmvHdIndexHeader h) {
        std::ofstream out(root/"movie/index.bin",std::ios::binary);
        out.write(reinterpret_cast<const char*>(&h),sizeof h);out.put(0);
    };
    const uint8_t rgba[16]={0,0,0,255,0,0,0,255,0,0,0,255,0,0,0,255};
    FILE *image=std::fopen((root/"movie/frames/000000.png").string().c_str(),"wb");
    check(image && png_write_rgba(image,rgba,2,2),"create valid tiny movie frame");
    if(image)std::fclose(image);
    uint32_t original[4]={0xff000000u,0xff000000u,0xff000000u,0xff000000u};
    FmvHdImage output{};
    auto lookup=[&]{return gpu_fmv_hd_lookup(original,2,2,2,&output);};
    for(unsigned variant=0;variant<5;++variant) {
        auto bad=header;
        if(variant==0)bad.sig_w=UINT32_MAX;
        if(variant==1)bad.frames=UINT32_MAX;
        if(variant==2)bad.sub=UINT32_MAX;
        if(variant==3)bad.off_x=INT32_MAX;
        if(variant==4)bad.frames=2; // truncated signature payload
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
    fs::remove_all(root);
    if(!failures)std::cout<<"PASS FMV bounds, allocation/thread failure, real decode, joined shutdown and restart\n";
    return failures?1:0;
}
