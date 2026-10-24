#pragma once
/* Optional shipped-backend catalog. No catalog means no behavior change.
 * A profile switch is a process handoff, never a live code/config transplant. */
#include "disc_identity.h"
#include "disc_path.h"
#include "host_sha256.h"
#include <toml.hpp>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>
#include <stdexcept>
#if defined(_WIN32)
#include <process.h>
#endif

namespace PSXRecompV4 {
namespace regional {
namespace fs = std::filesystem;
struct Profile { std::string id, sha256; uint64_t size; fs::path backend; };
inline fs::path root, self;
inline std::vector<Profile> profiles;
inline std::string selected_disc, pending_disc;
inline const Profile* pending = nullptr;
inline std::vector<std::string> launch_flags;
inline bool same_backend(const fs::path& a,const fs::path& b) {
    std::error_code error;
    return a==b || fs::equivalent(a,b,error);
}
inline fs::path inside(const fs::path& relative) {
    if(relative.empty() || relative.is_absolute()) throw std::runtime_error("Profile backend must be relative");
    auto p=fs::weakly_canonical(root/relative);
    auto rel=p.lexically_relative(root);
    if(rel.empty() || *rel.begin()=="..") throw std::runtime_error("Profile backend escapes package");
    return p;
}
inline void load(const char* argv0) {
    self=fs::weakly_canonical(fs::absolute(argv0));
#if defined(_WIN32)
    wchar_t module[32768];const DWORD length=GetModuleFileNameW(nullptr,module,32768);
    if(length>0 && length<32768)self=fs::weakly_canonical(fs::path(module));
#endif
    auto folder=self.parent_path();
    for(int i=0;i<4 && !folder.empty();++i,folder=folder.parent_path()) {
        auto file=folder/"regional-launch.toml";
        if(!fs::is_regular_file(file)) continue;
        root=fs::weakly_canonical(folder);
        const auto config=toml::parse(file.string());
        for(const auto& entry:toml::find(config,"profile").as_array()) {
            Profile p{toml::find<std::string>(entry,"id"),toml::find<std::string>(entry,"sha256"),
                toml::find<uint64_t>(entry,"size"),inside(toml::find<std::string>(entry,"backend"))};
            if(p.id.empty() || p.sha256.size()!=64 || !p.size) throw std::runtime_error("Invalid regional identity");
            for(const auto& old:profiles) if(old.id==p.id || same_backend(old.backend,p.backend)) throw std::runtime_error("Duplicate regional profile");
            profiles.push_back(p);
        }
        bool owns=false;for(const auto& p:profiles) owns|=same_backend(p.backend,self);
        if(!owns) throw std::runtime_error("Executable not declared in regional catalog");
        return;
    }
}
inline const Profile* identify(const fs::path& picked) {
    auto disc=resolve_disc_path(picked);
    auto id=identify_disc(picked,"",0,false,false);
    if(!id.opened || !id.has_header || disc.cue_fallback) return nullptr;
    for(const auto& p:profiles) {
        if(id.detected_serial!=p.id || fs::file_size(disc.data)!=p.size) continue;
        uint8_t digest[32];if(!host_sha256_file(disc.data,digest)) return nullptr;
        char hex[65];for(int i=0;i<32;++i) std::snprintf(hex+i*2,3,"%02x",digest[i]);
        if(p.sha256==hex && fs::is_regular_file(p.backend)) return &p;
    }
    return nullptr;
}
inline void remember(const Profile& profile,const std::string& disc) {
    toml::value state{{"profile",profile.id},{"disc",disc}};
    auto temp=root/("europe-selection-"+std::to_string(
#if defined(_WIN32)
        GetCurrentProcessId()
#else
        0
#endif
        )+".tmp");
    /* TOML continuation lines trim leading whitespace. Width-wrapping a
     * filename at a space would silently change the remembered disc path. */
    {std::ofstream out(temp,std::ios::binary);out<<toml::format(state,(std::numeric_limits<std::size_t>::max)());out.flush();if(!out)throw std::runtime_error("Cannot save regional selection");}
#if defined(_WIN32)
    if(!MoveFileExW(temp.c_str(),(root/"europe-selection.toml").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot commit regional selection");
#else
    fs::rename(temp,root/"europe-selection.toml");
#endif
}
inline int spawn(const Profile& target,const std::string& disc) {
#if defined(_WIN32)
    std::vector<std::wstring> args{target.backend.wstring(),L"--disc",fs::path(disc).wstring()};
    for(const auto& flag:launch_flags)args.push_back(fs::path(flag).wstring());
    /* _spawn passes argv through the Windows command-line decoder. Quote each
     * argument with the inverse CRT rules, including trailing backslashes. */
    for(auto& arg:args) {
        std::wstring quoted=L"\"";unsigned slashes=0;
        for(wchar_t ch:arg) {
            if(ch==L'\\'){++slashes;continue;}
            quoted.append(ch==L'"'?slashes*2+1:slashes,L'\\');slashes=0;quoted+=ch;
        }
        quoted.append(slashes*2,L'\\');quoted+=L'"';arg=quoted;
    }
    std::vector<const wchar_t*> pointers;for(const auto& arg:args)pointers.push_back(arg.c_str());pointers.push_back(nullptr);
    /* CRT spawn constructs a direct process invocation; no command shell. */
    if(_wspawnv(_P_NOWAIT,target.backend.c_str(),pointers.data())==-1) return 1;
    remember(target,disc);
    return 0;
#else
    (void)target;(void)disc;return 1; // Windows distribution opt-in only.
#endif
}
/* Called by the existing disc verification callback. Never touches cards or
 * settings. 1 requests a clean UI exit, -1 rejects, 0 stays in this backend. */
inline int picked(const char* path) {
    if(profiles.empty()) return 0;
    try {
        const auto disc=fs::absolute(path).lexically_normal().string();
        const auto* p=identify(disc);if(!p)return -1;
        if(!same_backend(p->backend,self)) {pending=p;pending_disc=disc;return 1;}
        remember(*p,disc);return 0;
    } catch(const std::exception& e){std::fprintf(stderr,"regional selection: %s\n",e.what());return -1;}
}
inline int handoff() {
    if(!pending)return 1;
    try{return spawn(*pending,pending_disc);}catch(const std::exception& e){std::fprintf(stderr,"regional handoff: %s\n",e.what());return 1;}
}
/* Before SDL/config initialization. Probe/select-only are CLI acceptance hooks,
 * exercising the exact production identity and persistence code without UI. */
inline int startup(int argc,char** argv) {
    try {
        load(argv[0]);if(profiles.empty())return -1;
        std::string disc;bool probe=false,select_only=false,explicit_disc=false,exec_probe=false,ui_probe=false;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--disc" && i+1<argc){disc=argv[++i];explicit_disc=true;}
            else if(arg=="--regional-probe")probe=true;
            else if(arg=="--regional-select-only")select_only=true;
            else if(arg=="--regional-exec-probe"){exec_probe=true;launch_flags.push_back(arg);}
            else if(arg=="--regional-ui-route-probe"){exec_probe=ui_probe=true;launch_flags.push_back("--regional-exec-probe");}
            else if(arg=="--game" && i+1<argc)++i;
            else launch_flags.push_back(arg);
        }
        if(disc.empty() && fs::exists(root/"europe-selection.toml")) {
            try {
                const auto state=toml::parse((root/"europe-selection.toml").string());
                disc=toml::find<std::string>(state,"disc");
            } catch(...) {std::fprintf(stderr,"Ignoring unreadable European selection record\n");}
        }
        if(disc.empty())return (probe||select_only)?1:-1;
        disc=fs::absolute(disc).lexically_normal().string();
        const auto* p=identify(disc);
        if(!p){std::fprintf(stderr,"Unsupported European disc or missing backend\n");return explicit_disc||probe||select_only?1:-1;}
        if(probe||select_only) {
            if(select_only)remember(*p,disc);
            std::printf("regional profile=%s backend=%s\n",p->id.c_str(),p->backend.filename().string().c_str());return 0;
        }
        if(!same_backend(p->backend,self)) {
            if(ui_probe){if(picked(disc.c_str())!=1)return 1;return handoff();}
            return spawn(*p,disc);
        }
        if(exec_probe){std::printf("regional entered=%s\n",p->id.c_str());return 0;}
        selected_disc=disc;remember(*p,disc);return -1;
    } catch(const std::exception& e){std::fprintf(stderr,"regional launcher: %s\n",e.what());return 1;}
}
} // namespace regional
} // namespace PSXRecompV4
