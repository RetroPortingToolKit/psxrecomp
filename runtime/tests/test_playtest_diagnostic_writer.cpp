#define PSX_DIAGNOSTIC_WRITER_TEST
#include "playtest_diagnostic_writer.h"
#include <algorithm>
#include <cstdio>
#include <sstream>
#include <vector>
using namespace psx_playtest_diagnostic;
#define CHECK(x) do {if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static std::atomic<bool> entered{false},released{false};
static void gate() {
    entered.store(true);
    while(!released.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
static std::string contents(const std::filesystem::path& path) {
    std::ifstream in(path);return std::string(std::istreambuf_iterator<char>(in),{});
}
static void reference(std::ostream& out,const Row& row) {
    out<<"N ";
    for(unsigned i=0;i<row.size();++i)out<<(i?",":"")<<row[i];
    out<<'\n';
}
int main(int argc,char **argv) {
    auto root=std::filesystem::u8path(argc>1?argv[1]:"writer-qa")/
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(root);
    Row row{};
    // Slow filesystem: producers return at capacity, drain exactly the owned
    // accepted prefix, and never borrow mutable frame memory.
    Writer<8> blocked;blocked.test_gate(gate);blocked.open(root/"blocked.csv","header\n");
    struct ReleaseOnExit {~ReleaseOnExit() {released.store(true);}} release_gate;
    CHECK(blocked && blocked.is_open());
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(!entered.load() && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    CHECK(entered.load());
    std::ostringstream expected;expected<<"header\n";
    for(unsigned i=0;i<7;++i) {
        row.fill(i);
        CHECK(blocked.native(row));reference(expected,row);
    }
    CHECK(!blocked.native(row)); // full: stop capture rather than block gameplay
    CHECK(!released.load()); // All submissions returned while disk was gated.
    CHECK(std::string(blocked.error_text()).find("queue full")!=std::string::npos);
    row.fill(-99);released.store(true);blocked.close();
    CHECK(contents(root/"blocked.csv")==expected.str());
    // Exercise ring wrap, including the barrier used by tests/tools.
    Writer<32> writer;writer.open(root/"ordered.csv","header\n");
    expected.str("");expected.clear();expected<<"header\n";
    for(unsigned i=0;i<300;++i) {
        row.fill(i);row[13]=-1234;
        CHECK(writer.native(row));reference(expected,row);
        if(i%8==7)writer.flush();
        CHECK(writer);
    }
    // No more game frames or explicit flush: the worker must flush on wall
    // time even when the producer is paused or hung.
    deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while(contents(root/"ordered.csv")!=expected.str() && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(contents(root/"ordered.csv")==expected.str());
    writer.close();CHECK(writer);
    CHECK(contents(root/"ordered.csv")==expected.str());
    // Failed destinations are reported; a later reset can start a new capture.
    writer.open(root/"missing"/"capture.csv","header\n");CHECK(!writer && !writer.is_open());
    writer.open(root/"recovered.csv","header\n");CHECK(writer && writer.is_open());
    CHECK(writer.native(row));
    writer.close();CHECK(writer);
    CHECK(contents(root/"recovered.csv").find("N ")!=std::string::npos);
    // A sidecar uses the same bounded/draining transport with its own schema;
    // it must not change the replayable input row width or prefix.
    Writer<8,std::array<int64_t,3>> sidecar;
    sidecar.open(root/"telemetry.csv","sequence,time,value\n","");
    CHECK(sidecar.native({7,1000,-42}));sidecar.close();CHECK(sidecar);
    CHECK(contents(root/"telemetry.csv")=="sequence,time,value\n7,1000,-42\n");
    if(argc>2 && std::string(argv[2])=="benchmark") {
        constexpr unsigned frames=10000,batch=100;
        std::vector<double> latencies;
        auto path=root/"bench.csv";Writer<> async;async.open(path,"");
        for(unsigned i=0;i<frames;++i) {
            row.fill(i);
            auto before=std::chrono::steady_clock::now();
            CHECK(async.native(row));
            latencies.push_back(std::chrono::duration<double,std::micro>(
                std::chrono::steady_clock::now()-before).count());
            if(i%batch==batch-1)async.flush();
        }
        async.close();CHECK(async);
        std::sort(latencies.begin(),latencies.end());
        double sum=0;for(auto t:latencies)sum+=t;
        printf("producer: mean %.3f us p99 %.3f us\n",sum/latencies.size(),latencies[latencies.size()*99/100]);
    }
    printf("bounded diagnostic writer checks passed\n");return 0;
}
