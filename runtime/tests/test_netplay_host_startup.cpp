/* Synthetic trusted policy only: this fixture grants no production profile. */
#include "netplay_host_startup.hpp"
#include "netplay_host_scope.h"
#include <cstdlib>
#include <cstring>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);std::exit(1);}}while(0)
static PSXModNetplayProfile title;
static PSXHostProfileRule rule;
static PSXHostCapabilities registered;
static PSXHostCPU cpu;
static PsxBiosImageInfo image;
static PsxBiosBackend bios;
static unsigned clears,installs,shutdowns,seals,ends,callbacks;
static bool fp_ok,seal_ok,image_ok,register_ok,callback_ok,scope,ready,failed,selection_ok;
static int fp_contract,sealed;
static int spectator,initial_settled;
static uint64_t initial_cycles;
static uint32_t initial_tick;
static PSXModNetplayHostStartup observed;
static PSXHostSelection selected;
extern "C" {
const PSXModNetplayProfile *psx_mod_netplay_profile(void){return &title;}
int psx_mod_netplay_loading(void){return 1;}
int psx_mod_netplay_aspect(void){return 0;}
int psx_host_fp_environment_is_default(void){return fp_ok;}
int psx_host_fp_environment_contract(void){return fp_contract;}
void psx_host_cpu_detect_sdl3(PSXHostCPU *out){*out=cpu;}
void autocompile_shutdown(void){++shutdowns;}
int code_provider_session_seal_interpreted(void){CHECK(shutdowns);++seals;if(seal_ok)sealed=1;return seal_ok;}
int code_provider_session_interpreted_sealed(void){return sealed;}
int code_provider_session_end_interpreted(void){++ends;sealed=0;return 1;}
int psx_running_executable_sha256(uint8_t out[32]){
    CHECK(sealed);if(!image_ok)return 0;std::memset(out,0x3a,32);return 1;
}
int psx_netplay_direct_loopback_scope(const PsxNetplayConfig *){return scope;}
int psx_netplay_host_profile_register(const PSXHostCapabilities *caps,const PSXHostProfileCatalog *cat){
    if(!caps||!cat){++clears;std::memset(&registered,0,sizeof registered);return 0;}
    CHECK(sealed);CHECK(psx_host_caps_validate(caps,cat));
    ++installs;if(register_ok)registered=*caps;return register_ok;
}
int psx_netplay_host_profile_ready(void){return ready;}
int psx_netplay_is_spectator(void){return spectator;}
int psx_netplay_host_profile_initial_settlement(uint64_t *c,uint32_t *t){
    if(!initial_settled)return 0;*c=initial_cycles;*t=initial_tick;return 1;
}
int psx_netplay_host_profile_failed(void){return failed;}
int psx_netplay_host_profile_selection(PSXHostSelection *out){if(!selection_ok)return 0;*out=selected;return 1;}
}
static int catalog(const PSXModNetplayHostStartup *context,PSXHostProfileCatalog *out){
    ++callbacks;CHECK(sealed);observed=*context;
    CHECK(context->actual_executable_sha256[0]==0x3a);
    CHECK(context->builtin_linked_bios&&context->interpreted_overlays_sealed);
    CHECK(!std::strcmp(context->builtin_bios_image_id,"TEST-BIOS"));
    CHECK(context->builtin_bios_image_wordsum==0x12345678);
    CHECK(context->compiled_backends==psx_host_compiled_backend());
    CHECK(context->available_backends==psx_host_compiled_backend());
    CHECK(context->declared_build_manifest_sha256!=nullptr);
    CHECK(context->declared_build_manifest_schema==psx_compiled_build_manifest_schema());
    CHECK(context->slot_count==2&&context->player_count==2&&context->occupied_mask==3);
    CHECK(!context->rollback&&context->loading_enabled&&context->renderer==1);
    CHECK(context->retained_texture_banks&&context->aspect_index==0);
    out->rules=&rule;out->count=1;return callback_ok;
}
static void reset(void){
    std::memset(&title,0,sizeof title);title.host_profiles=catalog;
    std::memset(&rule,0,sizeof rule);std::strcpy(rule.profile,"synthetic-fixture");
    rule.revision=7;rule.qualification[0]=1;rule.implementation_count=1;
    rule.implementations[0].backend=psx_host_compiled_backend();
    rule.implementations[0].architectures=1u<<PSX_HOST_ARCH_X64;
    cpu={PSX_HOST_ARCH_X64,1,PSX_HOST_SSE2};
    fp_ok=seal_ok=image_ok=register_ok=callback_ok=selection_ok=true;
    scope=ready=failed=false;fp_contract=2;sealed=0;
    clears=installs=shutdowns=seals=ends=callbacks=0;
    spectator=initial_settled=0;initial_cycles=0;initial_tick=0;
    image={};image.image_id="TEST-BIOS";image.image_wordsum=0x12345678;
    bios={};bios.image=&image;
    selected={};std::strcpy(selected.profile,rule.profile);selected.revision=7;selected.identity[0]=3;
}
static bool install(uint32_t initialized=PSX_HOST_BACKENDS_ALL,const PsxBiosBackend *builtin=&bios){
    PsxNetplayConfig config{};std::string error;
    config.slot_count=2;config.player_count=2;config.occupied_mask=3;
    bool ok=psx_install_required_host_profile(config,initialized,builtin,1,1,error);
    CHECK(config.host_profile_required==1);CHECK(clears==1);
    CHECK(ok==error.empty());return ok;
}
static void refusal(void){
    for(const char *name:psx_simulation_testing::OverrideNames){
        const char *prior=std::getenv(name);const bool present=prior!=nullptr;
        const std::string saved=prior?prior:"";
#ifdef _WIN32
        CHECK(_putenv_s(name,"0")==0);
#else
        CHECK(setenv(name,"0",1)==0);
#endif
        reset();CHECK(!install()&&!callbacks&&!shutdowns&&!seals&&!installs);
#ifdef _WIN32
        CHECK(_putenv_s(name,present?saved.c_str():"")==0);
#else
        CHECK(present?setenv(name,saved.c_str(),1)==0:unsetenv(name)==0);
#endif
    }
    reset();title.host_profiles=nullptr;CHECK(!install()&&!seals&&!installs);
    reset();fp_ok=false;CHECK(!install()&&!seals&&!installs);
    reset();fp_contract=1;CHECK(!install()&&!seals);
    reset();cpu.architecture=PSX_HOST_ARCH_UNKNOWN;CHECK(!install()&&!seals);
    reset();cpu.features_known=0;cpu.features=0;CHECK(!install()&&!installs&&ends==1);
    reset();cpu.features_known=0;cpu.features=0;rule.flags=PSX_HOST_PROFILE_PORTABLE;CHECK(install()&&installs==1);
    reset();rule.flags=PSX_HOST_PROFILE_OBSERVER;CHECK(install()&&installs==1);
    reset();CHECK(!install(0)&&!seals);
    reset();CHECK(!install(3,nullptr)&&!seals);
    reset();seal_ok=false;CHECK(!install()&&shutdowns==1&&seals==1&&!sealed);
    reset();image_ok=false;CHECK(!install()&&ends==1&&!sealed&&!installs);
    reset();callback_ok=false;CHECK(!install()&&callbacks==1&&ends==1&&!installs);
    reset();rule.qualification[0]=0;CHECK(!install()&&ends==1&&!installs);
    reset();rule.implementations[0].required_features=PSX_HOST_AVX2;
    CHECK(!install()&&ends==1&&!installs);
    reset();register_ok=false;CHECK(!install()&&ends==1&&installs==1&&!sealed);
    reset();rule.flags=PSX_HOST_PROFILE_DIRECT_LOOPBACK;CHECK(!install()&&!installs&&ends==1);
}
static void success(void){
    reset();CHECK(install()&&installs==1&&sealed&&shutdowns==1&&seals==1&&!ends);
    CHECK(registered.offer_count==1&&registered.build[0]==0x3a);
    CHECK(registered.available_backends==psx_host_compiled_backend());
    CHECK(observed.fp_contract==2&&!observed.direct_loopback_scope);
    reset();scope=true;rule.flags=PSX_HOST_PROFILE_DIRECT_LOOPBACK;
    CHECK(install()&&installs==1&&observed.direct_loopback_scope);
}
static void settlement(void){
    reset();std::string error;
    CHECK(!psx_log_required_host_profile_settled(0,0,error)); /* RUNNING is insufficient. */
    ready=true;selection_ok=false;CHECK(!psx_log_required_host_profile_settled(0,0,error));
    selection_ok=true;failed=true;CHECK(!psx_log_required_host_profile_settled(0,0,error));
    failed=false;CHECK(!psx_log_required_host_profile_settled(1,0,error));
    CHECK(!psx_log_required_host_profile_settled(0,1,error));
    CHECK(psx_log_required_host_profile_settled(0,0,error));
    spectator=1;CHECK(!psx_log_required_host_profile_settled(1000,200,error));
    initial_settled=1;initial_cycles=1;CHECK(!psx_log_required_host_profile_settled(1000,200,error));
    initial_cycles=0;initial_tick=0;CHECK(psx_log_required_host_profile_settled(1000,200,error));
}
static void routes(void){
    const char *bad[]={nullptr,"","localhost:1234","127.0.0.2:1","0.0.0.0:2",":3",
        "127.0.0.1","127.0.0.1:","127.0.0.1:0","127.0.0.1:65536",
        "127.0.0.1:-1","127.0.0.1:+1","127.0.0.1:1x","127.0.0.1:123456",
        "127.0.0.1:1 ","[::1]:1234"};
    for(auto p:bad)CHECK(!np_host_numeric_loopback(p));
    CHECK(np_host_numeric_loopback("127.0.0.1:1"));CHECK(np_host_numeric_loopback("127.0.0.1:65535"));
    CHECK(np_host_direct_loopback_scope(0,0,0,"127.0.0.1:4000",""));
    CHECK(np_host_direct_loopback_scope(0,0,0,"127.0.0.1:4000","127.0.0.1:4001"));
    CHECK(!np_host_direct_loopback_scope(1,0,0,"127.0.0.1:4000",""));
    CHECK(!np_host_direct_loopback_scope(2,0,0,"127.0.0.1:4000",""));
    CHECK(!np_host_direct_loopback_scope(0,1,0,"127.0.0.1:4000",""));
    CHECK(!np_host_direct_loopback_scope(0,0,1,"127.0.0.1:4000",""));
    CHECK(!np_host_direct_loopback_scope(0,0,0,"0.0.0.0:4000","127.0.0.1:4001"));
    CHECK(!np_host_direct_loopback_scope(0,0,0,"127.0.0.1:4000","192.168.1.1:4001"));
    CHECK(!np_host_direct_loopback_scope(0,0,0,"127.0.0.1:4000",nullptr));
}
int main(void){refusal();success();settlement();routes();std::puts("host startup: PASS");}
