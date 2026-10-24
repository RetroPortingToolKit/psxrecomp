#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif
int main(int argc,char **argv) {
    if(argc<2)return 2;
#ifdef _WIN32
    const int pid=_getpid();
#else
    const int pid=getpid();
#endif
    std::ofstream(std::string(argv[argc-1])+".helper-pid")<<pid;
    const char *mode=std::getenv("PSX_VREC_STALL_MODE");
    if(mode && !std::strcmp(mode,"drain")) {
        char bytes[65536];
        while(std::fread(bytes,1,sizeof bytes,stdin)){}
    }
    std::this_thread::sleep_for(std::chrono::seconds(60));
}
