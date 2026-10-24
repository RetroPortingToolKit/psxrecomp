// Exercise the production atomic writer and parser across formatting boundaries.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "regional_launch.h"
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if(argc!=2) return 2;
    using namespace PSXRecompV4::regional;
    root=fs::path(argv[1]);fs::create_directories(root);
    const Profile profile{"SCES-01330","",0,{}};
    std::vector<std::string> paths;
    for(unsigned n=0;n<160;++n) {
        paths.push_back("G:\\owned\\"+std::string(n,'d')+
            "\\Tombi! (Europe) (En,Fr,De,It,Nl) (Rev 1).cue");
    }
    paths.push_back("G:\\"+std::string(80,'x')+"\\Tombi! (Europe) Espa\xc3\xb1ol.cue");
    paths.push_back("G:\\"+std::string(80,'x')+"\\Tombi! #game [Rev 1].cue");
    for(const auto& path:paths) {
        remember(profile,path);
        const auto state=toml::parse((root/"europe-selection.toml").string());
        if(toml::find<std::string>(state,"disc")!=path ||
           toml::find<std::string>(state,"profile")!=profile.id) {
            std::fprintf(stderr,"FAIL: remembered path changed (%zu bytes)\n",path.size());
            return 1;
        }
    }
    std::printf("PASS: %zu production selection roundtrips preserve spaces, escapes and UTF-8\n",paths.size());
    return 0;
}
