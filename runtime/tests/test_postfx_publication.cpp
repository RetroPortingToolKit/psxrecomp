#include "gte_projection_publication.hpp"
#include "postfx_debug_json.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
static int failures;
static void check(bool ok,const char* why) {
    if(!ok){std::fprintf(stderr,"%s\n",why);++failures;}
}
int main() {
    GteProjectionPublication state; GtePostfxProjection out{};
    state.publish(1,2,3); check(!state.read(out),"disabled projection is unavailable");
    state.enable(true); state.publish(1,2,3);
    check(state.read(out)&&out.h==1&&out.ofx==2&&out.ofy==3,"first enabled tuple");
    state.enable(false);state.enable(true);
    check(!state.read(out),"re-enabled session cannot reuse a stale tuple");
    state.publish(1,2,3);check(state.read(out),"unchanged values publish in the new epoch");
    std::atomic<bool> done{false},bad{false};
    std::thread writer([&]{for(int i=1;i<200000;++i)state.publish(i,i*2,i*3);done=true;});
    auto read=[&]{while(!done.load()){GtePostfxProjection p{};
        if(state.read(p)&&(p.ofx!=p.h*2||p.ofy!=p.h*3))bad=true;}};
    std::thread a(read),b(read);writer.join();a.join();b.join();
    check(!bad,"concurrent readers never see mixed projection fields");
    PfxDebugLive live{};std::strcpy(live.name,"u_\"x\n");live.v[0]=1.25f;
    char json[512];check(pfx_debug_json(json,sizeof json,"C:\\a\"b\n","bad\tpreset",&live,1)>0,"serialize escaped diagnostics");
    check(std::strstr(json,R"("path":"C:\\a\"b\u000a")")!=nullptr,"path round trips without mutation");
    check(std::strstr(json,R"("u_\"x\u000a":[1.25,0,0,0])")!=nullptr,"uniform names are escaped");
    std::printf("{%s}\n",json);
    for(int cap=0;cap<160;++cap) {
        char guarded[192];std::memset(guarded,'X',sizeof guarded);
        const int n=pfx_debug_json(guarded+1,cap,"C:\\a\"b\n","bad\tpreset",&live,1);
        check(guarded[0]=='X'&&guarded[cap+1]=='X',"small output buffer stays within bounds");
        if(!n&&cap)check(guarded[1]==0,"failed JSON output is empty");
    }
    live.v[0]=std::numeric_limits<float>::infinity();
    check(!pfx_debug_json(json,sizeof json,"","",&live,1)&&!json[0],"nonfinite values fail without invalid JSON");
    return failures?1:0;
}
