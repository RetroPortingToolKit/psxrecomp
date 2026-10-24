#include "video_recorder.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#endif
static unsigned failures;
static void check(bool ok,const char *why){if(!ok){std::fprintf(stderr,"%s\n",why);++failures;}}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    namespace fs=std::filesystem;
    const auto root=fs::path(argv[2])/fs::path("vrec-cancel-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    video_recorder_set_backend(VREC_BACKEND_SOFTWARE);
    for(const char *mode:{"blocked","drain"}) {
#ifdef _WIN32
        _putenv_s("PSX_VREC_STALL_MODE",mode);
#else
        setenv("PSX_VREC_STALL_MODE",mode,1);
#endif
        VrecOptions options{};vrec_options_default(&options);
        options.audio=0;options.queue_frames=2;
        options.width=options.height=2048;
        const auto path=root/(std::string(mode)+".mp4");
        std::snprintf(options.path,sizeof options.path,"%s",path.string().c_str());
        std::snprintf(options.ffmpeg,sizeof options.ffmpeg,"%s",argv[1]);
        char error[512]{};
        check(video_recorder_start(&options,error,sizeof error)!=0,error);
        uint32_t session=0;check(video_recorder_present_poll(&session)==VREC_PRESENT_CAPTURE,"capture requested");
        std::vector<uint32_t> pixels(2048u*2048u,0xff112233u);
        video_recorder_submit_frame(session,pixels.data(),2048,2048,2048*4,0,100);
        const auto pid_path=path.string()+".part.video.nut.helper-pid";
        for(unsigned i=0;i<1000 && !fs::exists(pid_path);++i)std::this_thread::sleep_for(std::chrono::milliseconds(2));
        int child=0;std::ifstream(pid_path)>>child;check(child>0,"real helper started");
        if(std::string(mode)=="drain") {
            for(unsigned i=0;i<1000;++i){VrecStatus status{};video_recorder_get_status(&status);if(status.frames_written)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}
        }
        auto begin=std::chrono::steady_clock::now();
        video_recorder_shutdown();
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        check(seconds<8,"shutdown of a blocked encoder is bounded");
        VrecStatus status{};video_recorder_get_status(&status);
        check(status.state==VREC_ERROR,"interrupted encoder reports an error");
        if(child>0) {
#ifdef _WIN32
            HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,(DWORD)child);
            check(!process || WaitForSingleObject(process,0)==WAIT_OBJECT_0,"owned encoder exited");
            if(process)CloseHandle(process);
#else
            check(kill(child,0)<0 && errno==ESRCH,"owned encoder reaped");
#endif
        }
        std::printf("%s encoder shutdown %.3f s\n",mode,seconds);
    }
    video_recorder_shutdown();
    fs::remove_all(root);
    return failures?1:0;
}
