/* Host-only scoped-reader regression; generated empty CUE/BIN fixture, no game data. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mod_disc_read_session.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <vector>
using namespace PSXRecompV4;
int main(int argc,char**argv) {
    assert(argc==2);
    const auto dir=std::filesystem::path(argv[1])/std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    assert(!std::filesystem::exists(dir));std::filesystem::create_directories(dir);
    /* Declared before the readers, so their handles close before removal on
     * Windows. Each successful run owns and removes only its unique fixture. */
    struct FixtureCleanup {
        std::filesystem::path dir;
        ~FixtureCleanup() {
            std::error_code ec;
            std::filesystem::remove_all(dir,ec);
            assert(!ec);
        }
    } cleanup{dir};
    {std::ofstream out(dir/"data.bin",std::ios::binary);std::vector<char> raw(2352*24,0);out.write(raw.data(),raw.size());}
    for(const char* name:{"first.cue","second.cue"}){
        std::ofstream out(dir/name);out<<"FILE \"data.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n";
    }
    const std::string first_path=(dir/"first.cue").string(),second_path=(dir/"second.cue").string();
    auto& state=mod_disc_read_session_state();
    PS1::ISOReader fresh;
    assert(!state.depth && !state.reader);
    assert(mod_disc_read_reader(first_path,fresh)==&fresh);
    {
        ModDiscReadSession outer;
        PS1::ISOReader a,b,c;
        auto* first=mod_disc_read_reader(first_path,a);
        assert(first && first!=&a && state.depth==1);
        { ModDiscReadSession inner;
          assert(mod_disc_read_reader(first_path,b)==first && state.depth==2); }
        assert(state.depth==1 && mod_disc_read_reader(first_path,c)==first);
        assert(!mod_disc_read_reader("missing-disc-must-not-exist.cue",c));
        assert(!state.reader && state.mount.empty());
        assert(mod_disc_read_reader(second_path,b) && state.mount==second_path);
        assert(mod_disc_read_reader(first_path,c) && state.mount==first_path);
    }
    assert(!state.depth && !state.reader && state.mount.empty());
    try { ModDiscReadSession scope; assert(mod_disc_read_reader(first_path,fresh)); throw std::runtime_error("test"); }
    catch(const std::runtime_error&) {}
    assert(!state.depth && !state.reader);
    puts("PASS fresh reader outside scope, reuse, nested scopes, missing/change mount, RAII cleanup");
}
