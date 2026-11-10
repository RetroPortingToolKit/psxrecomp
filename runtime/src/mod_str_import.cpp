#include "mod_str_import.h"
#include "psx_sha256.h"
#include "psx_sha256_file.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <chrono>
#include <fstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace PSXRecompV4 {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
constexpr uint64_t SectorSize = 2336, RawSize = 2352;
constexpr uint64_t MaxInput = 1024ull * 1024 * 1024;

std::string hex(const uint8_t* digest) {
    std::string out;
    for (unsigned i=0;i<32;++i) {
        out += "0123456789abcdef"[digest[i] >> 4];
        out += "0123456789abcdef"[digest[i] & 15];
    }
    return out;
}
std::string file_hash(const fs::path& path) {
    uint8_t digest[32];
    return psx_sha256_file(path,digest)==PsxFileHashResult::Ok ? hex(digest) : std::string{};
}
unsigned u16(const uint8_t* p) { return p[0] | unsigned(p[1])<<8; }
unsigned u32(const uint8_t* p) { return u16(p) | u16(p+2)<<16; }

bool valid_str(const fs::path& path, ModStrProfile profile) {
    std::error_code ec;
    const auto size=fs::file_size(path,ec);
    if (ec || !size || size%SectorSize || size/SectorSize>profile.max_sectors) return false;
    std::ifstream in(path,std::ios::binary);
    std::array<uint8_t,SectorSize> s{};
    unsigned video=0,audio=0,frame=0,chunk=0,chunks=0,last_audio=0;
    for (unsigned i=0;i<size/SectorSize;++i) {
        if (!in.read(reinterpret_cast<char*>(s.data()),s.size()) ||
            !std::equal(s.begin(),s.begin()+4,s.begin()+4) || s[0]!=1 || s[1]!=1) return false;
        if ((s[2]&0x7f)==0x64) {
            if (s[3]!=1 || (audio && i-last_audio!=8)) return false;
            ++audio;last_audio=i;
        } else {
            if (s[2]!=0x48 || s[3] || u32(s.data()+8)!=0x80010160 ||
                u16(s.data()+24)!=320 || u16(s.data()+26)!=240 ||
                u16(s.data()+30)!=0x3800 || u16(s.data()+34)!=2) return false;
            unsigned next_frame=u32(s.data()+16), count=u16(s.data()+14), bytes=u32(s.data()+20);
            if (!count || count>12 || !bytes || bytes>count*2016u ||
                u16(s.data()+12)!=chunk) return false;
            if (!chunk) {
                if (video && next_frame!=frame+1) return false;
                frame=next_frame;chunks=count;
            } else if (next_frame!=frame || count!=chunks) return false;
            ++video;
            if (++chunk==chunks) chunk=0;
        }
    }
    return video && audio && !chunk;
}

// A raw 2352-byte encode works with both upstream host builds. The guest
// consumes the XA subheader and the actual 2048-byte MDEC or 2304-byte audio
// body; the virtual disc does not read their unused trailing EDC/ECC area.
bool canonical_str(const fs::path& raw,const fs::path& dst,ModStrProfile profile) {
    const auto length=fs::file_size(raw);
    if(!length || length%RawSize || length/RawSize>profile.max_sectors) return false;
    std::ifstream in(raw,std::ios::binary);
    std::ofstream out(dst,std::ios::binary);
    std::array<uint8_t,RawSize> sector{};
    constexpr uint8_t sync[]={0,255,255,255,255,255,255,255,255,255,255,0};
    for(uint64_t i=0;i<length/RawSize;++i) {
        if(!in.read(reinterpret_cast<char*>(sector.data()),sector.size()) ||
           !std::equal(std::begin(sync),std::end(sync),sector.begin()) || sector[15]!=2)
            return false;
        const unsigned end=24u+((sector[18]&4) ? 2304u : 2048u);
        std::fill(sector.begin()+end,sector.end(),0);
        if(!out.write(reinterpret_cast<const char*>(sector.data()+16),SectorSize)) return false;
    }
    out.close();return bool(out);
}

bool verified(const fs::path& dir, ModStrProfile profile) {
    std::ifstream stamp(dir/"sha256");
    std::string expected;
    return bool(stamp >> expected) && expected.size()==64 &&
        valid_str(dir/"video.str",profile) && file_hash(dir/"video.str")==expected;
}

#if defined(_WIN32)
std::wstring quote(const std::wstring& arg) {
    std::wstring result=L"\"";
    size_t backslashes=0;
    for(wchar_t c:arg) {
        if(c==L'\\') { ++backslashes;continue; }
        result.append(c==L'"' ? backslashes*2+1 : backslashes,L'\\');
        backslashes=0;result+=c;
    }
    result.append(backslashes*2,L'\\');return result+L'"';
}
#endif

bool encode(const fs::path& tool, const fs::path& src, const fs::path& dst,
            ModStrProfile profile, const ModPrepareProgress& progress, std::string& error) {
    std::vector<fs::path> args={tool,"-q","-t","strcd","-v","v2","-s","320x240",
        "-r",std::to_string(profile.fps),"-f","37800","-c","2","-b","4",
        "-F","1","-C","1","-x","2","-X"};
#if defined(_WIN32)
    args.push_back(src.filename());args.push_back(dst.filename());
#else
    args.push_back(src);args.push_back(dst);
#endif
#if defined(_WIN32)
    std::wstring line;
    for(const auto& a:args) { if(!line.empty())line+=L' ';line+=quote(a.wstring()); }
    STARTUPINFOW startup{}; startup.cb=sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(tool.c_str(),line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
                        nullptr,dst.parent_path().c_str(),&startup,&process)) {
        error="Could not start the MP4 encoder";return false;
    }
    CloseHandle(process.hThread);
#else
    std::vector<std::string> storage;
    for(const auto& a:args)storage.push_back(a.string());
    std::vector<char*> argv;
    for(auto& a:storage)argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid;
    if(posix_spawn(&pid,tool.c_str(),nullptr,nullptr,argv.data(),environ)!=0) {
        error="Could not start the MP4 encoder";return false;
    }
#endif
    const auto deadline=Clock::now()+std::chrono::minutes(3);
    bool success=false;
    for (;;) {
#if defined(_WIN32)
        const auto wait=WaitForSingleObject(process.hProcess,100);
        if(wait==WAIT_OBJECT_0) {
            DWORD code=1;GetExitCodeProcess(process.hProcess,&code);success=code==0;break;
        }
#else
        int status=0;
        const auto wait=waitpid(pid,&status,WNOHANG);
        if(wait==pid) {success=WIFEXITED(status) && WEXITSTATUS(status)==0;break;}
        if(wait<0 && errno!=EINTR) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
        std::error_code ec;
        const auto size=fs::file_size(dst,ec);
        if (progress && !progress(-1.0f,"Encoding native movie")) error="Preparation canceled.";
        else if (!ec && size>uint64_t(profile.max_sectors)*RawSize) error="Movie exceeds the original slot length";
        else if (Clock::now()>=deadline) error="MP4 conversion timed out";
        else continue;
#if defined(_WIN32)
        TerminateProcess(process.hProcess,1);
        WaitForSingleObject(process.hProcess,5000);
#else
        kill(pid,SIGKILL);
        while(waitpid(pid,nullptr,0)<0 && errno==EINTR) {}
#endif
        break;
    }
#if defined(_WIN32)
    CloseHandle(process.hProcess);
#endif
    if (!success && error.empty()) error="Encoder could not read this MP4";
    return success;
}

struct Temporary {
    fs::path path;
    ~Temporary() { if(!path.empty()) { std::error_code ec;fs::remove_all(path,ec); } }
};
}

bool mod_import_str(const fs::path& source,const fs::path& encoder,const fs::path& cache,
                    ModStrProfile profile,bool cached_only,const ModPrepareProgress& progress,
                    fs::path& output,std::string& error) {
    output.clear();error.clear();
    try {
        if (!profile.fps || profile.fps>30 || !profile.max_sectors || profile.max_sectors>150000)
            throw std::runtime_error("Invalid native movie profile");
        std::error_code ec;
        const auto size=fs::file_size(source,ec);
        if (ec || !size || size>MaxInput || !fs::is_regular_file(source))
            throw std::runtime_error("Select a readable MP4 under 1 GiB");
        if (!encoder.is_absolute() || !fs::is_regular_file(encoder))
            throw std::runtime_error("MP4 encoder is unavailable");
        const auto source_hash=file_hash(source), encoder_hash=file_hash(encoder);
        if (source_hash.empty() || encoder_hash.empty()) throw std::runtime_error("Cannot read MP4 or encoder");
        const auto identity="mode2-mdec2-xa37800-v2:"+source_hash+":"+encoder_hash+":"+
            std::to_string(profile.fps)+":"+std::to_string(profile.max_sectors);
        uint8_t digest[32];psx_sha256_compute(reinterpret_cast<const uint8_t*>(identity.data()),identity.size(),digest);
        const auto key=hex(digest);
        const auto entry=cache/key;
        if (verified(entry,profile)) {output=entry/"video.str";return true;}
        if (cached_only) throw std::runtime_error("Convert this MP4 with Play before joining a session");
        if (fs::exists(entry)) throw std::runtime_error("The cached movie failed verification");
        fs::create_directories(cache);
        Temporary temp;
        for(unsigned i=0;i<32 && temp.path.empty();++i) {
            const auto candidate=cache/(key+".tmp."+std::to_string(Clock::now().time_since_epoch().count())+"."+std::to_string(i));
            if(fs::create_directory(candidate))temp.path=candidate;
        }
        if(temp.path.empty())throw std::runtime_error("Cannot create movie cache staging directory");
        const auto staging=fs::absolute(temp.path);
        const auto input=staging/"source.mp4", raw=staging/"encoded.raw";
        fs::copy_file(source,input);
        if(file_hash(input)!=source_hash) throw std::runtime_error("MP4 changed during import");
        if(!encode(encoder,input,raw,profile,progress,error)) return false;
        if(!canonical_str(raw,staging/"video.str",profile) || !valid_str(staging/"video.str",profile))
            throw std::runtime_error("Encoder did not produce a compatible native movie");
        if(file_hash(encoder)!=encoder_hash) throw std::runtime_error("Encoder changed during import");
        fs::remove(input);fs::remove(raw);
        const auto encoded_hash=file_hash(temp.path/"video.str");
        if (encoded_hash.empty()) throw std::runtime_error("Cannot hash the native movie");
        { std::ofstream stamp(temp.path/"sha256");stamp<<encoded_hash<<'\n';stamp.close();
          if(!stamp)throw std::runtime_error("Cannot save native movie verification"); }
        if(progress && !progress(-1.0f,"Verifying native movie")) {error="Preparation canceled.";return false;}
        fs::rename(temp.path,entry,ec);
        if(ec && !verified(entry,profile)) throw std::runtime_error("Cannot publish the movie cache");
        if(!ec)temp.path.clear();
        output=entry/"video.str";return true;
    } catch(const std::exception& e) {error=e.what();return false;}
}
}
