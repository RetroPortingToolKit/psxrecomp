#include "host_executable_identity.h"
#include "psx_sha256_file.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#if defined(__APPLE__) || defined(__linux__)
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
namespace {
std::string hex(const std::array<uint8_t,32>& digest) {
    std::string text;
    char byte[3];
    for (auto b:digest) { std::snprintf(byte,sizeof byte,"%02x",b); text+=byte; }
    return text;
}

#if defined(__APPLE__) || defined(__linux__)
// Runs in a copy of this binary and replaces that copy's pathname before the
// first identity query. The identity must stay the mapped image or fail
// closed, never describe the file now at the launch path.
int swapped_child(const std::string& mode,const std::filesystem::path& self,
                  const std::string& expected) {
    if (mode=="rename") std::filesystem::rename(self,self.string()+".moved");
    else { CHECK(mode=="unlink"); std::filesystem::remove(self); }
    {
        std::ofstream replacement(self,std::ios::binary);
        replacement<<"replacement image";
        CHECK(replacement);
    }
    std::array<uint8_t,32> actual{};
    actual.fill(0xa5);
    const auto untouched=actual;
    const bool identified=psx_running_executable_sha256(actual.data());
#if defined(__APPLE__)
    // The kernel's path for an unlinked vnode now names the replacement.
    const bool must_identify=mode=="rename";
#else
    const bool must_identify=true; // procfs opens the unlinked inode itself.
#endif
    CHECK(identified==must_identify);
    CHECK(identified ? hex(actual)==expected : actual==untouched);
    return 0;
}

struct ScratchDir {
    std::filesystem::path path;
    explicit ScratchDir(std::filesystem::path p) : path(std::move(p)) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path,ignored);
    }
};

void run_swapped(const std::filesystem::path& image,const char* mode,const std::string& expected) {
    // Beside the test binary, whose directory must allow exec for the test to
    // run at all; a temp directory may be mounted noexec.
    const ScratchDir dir(image.parent_path()/
        ("psx-image-identity-"+std::to_string(getpid())+"-"+mode));
    const auto copy=dir.path/"image";
    std::filesystem::copy_file(image,copy);
    std::filesystem::permissions(copy,std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::add);
    std::string path=copy.string(),flag="--swapped",how=mode,digest=expected;
    char* args[]={path.data(),flag.data(),how.data(),path.data(),digest.data(),nullptr};
    pid_t child=0;
    const int spawned=posix_spawn(&child,path.c_str(),nullptr,nullptr,args,environ);
    int status=0;
    const bool waited=spawned==0&&waitpid(child,&status,0)==child;
    CHECK(waited);
    CHECK(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
#endif
}

int main(int argc,char** argv) {
    try {
#if defined(__APPLE__) || defined(__linux__)
        if (argc==5&&!std::strcmp(argv[1],"--swapped"))
            return swapped_child(argv[2],std::filesystem::path(argv[3]),argv[4]);
#endif
        CHECK(argc==2); // Harness supplies the real binary independently.
        std::array<uint8_t,32> expected{},actual{},again{};
        CHECK(psx_sha256_file(std::filesystem::path(argv[1]),expected.data())==PsxFileHashResult::Ok);
        CHECK(!psx_running_executable_sha256(nullptr));
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
        CHECK(psx_running_executable_sha256(actual.data()));
        CHECK(actual==expected);
        // Source of identity cannot silently turn into cwd/argv lookup.
        const auto cwd=std::filesystem::current_path();
        std::filesystem::current_path(cwd.root_path());
        CHECK(psx_running_executable_sha256(again.data()));
        std::filesystem::current_path(cwd);
        CHECK(again==expected);
#else
        actual.fill(0xa5);again=actual;
        CHECK(!psx_running_executable_sha256(actual.data()));
        CHECK(actual==again);
#endif
#if defined(__APPLE__) || defined(__linux__)
        // The launch path can be replaced after exec. Identity is computed
        // once per process, so each swap runs in its own copy of this binary.
        run_swapped(argv[1],"rename",hex(expected));
        run_swapped(argv[1],"unlink",hex(expected));
#endif
        std::printf("%s running image identity PASS\n",hex(expected).c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr,"running image identity: %s\n",error.what());
        return 1;
    }
}
