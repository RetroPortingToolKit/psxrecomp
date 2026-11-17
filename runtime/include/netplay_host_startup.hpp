#ifndef PSX_NETPLAY_HOST_STARTUP_HPP
#define PSX_NETPLAY_HOST_STARTUP_HPP
#include "psx_netplay.h"
#include "mod_netplay.h"
#include "host_executable_identity.h"
#include "host_fp_environment.h"
#include "build_manifest.h"
#include "autocompile.h"
#include "code_provider.h"
#include "psx_bios_backend.h"
#include "simulation_testing_overrides.hpp"
#include <string>
#include <cstdio>
/* Called on the simulation owner after device initialization, before transport
 * and any guest cycle. The caller has joined overlay cache initialization. */
static bool psx_install_required_host_profile(PsxNetplayConfig& cfg,uint32_t initialized,
                                               const PsxBiosBackend *builtin_bios,
                                               int renderer,int retained_banks,std::string& error){
    cfg.host_profile_required=1;error.clear();
    (void)psx_netplay_host_profile_register(nullptr,nullptr);
    auto refuse=[&](const char *why){error=why;
        if(code_provider_session_interpreted_sealed())(void)code_provider_session_end_interpreted();
        return false;};
    const PSXModNetplayProfile *title=psx_mod_netplay_profile();
    if(!title||!title->host_profiles)return refuse("This title has no qualified host simulation catalog.");
    if(psx_simulation_testing::first_configured_override())
        return refuse("Simulation testing overrides are not qualified for netplay host profiles.");
    if(!psx_host_fp_environment_is_default())return refuse("The host floating-point environment is not canonical.");
    PSXHostCPU cpu;psx_host_cpu_detect_sdl3(&cpu);
    unsigned fp=(unsigned)psx_host_fp_environment_contract();
    if(cpu.architecture==PSX_HOST_ARCH_UNKNOWN||fp!=(cpu.architecture==PSX_HOST_ARCH_X64?2u:1u))
        return refuse("This host floating-point and CPU contract is not supported.");
    uint32_t compiled=psx_host_compiled_backend();
    if(!compiled||(initialized&compiled)!=compiled)return refuse("The simulation backend has not initialized.");
    /* Caller established membership by exact pointer in the builtin registry. */
    if(!builtin_bios||!builtin_bios->image)return refuse("Native BIOS modules are not qualified for this host profile.");
    autocompile_shutdown();
    if(!code_provider_session_seal_interpreted()||!code_provider_session_interpreted_sealed())
        return refuse("The interpreted overlay session could not be sealed before gameplay.");
    uint8_t artifact[32];if(!psx_running_executable_sha256(artifact))
        return refuse("The running executable could not be identified safely.");
    PSXHostCapabilities caps;
    if(!psx_host_caps_init(&caps,&cpu,compiled,initialized&compiled,artifact))
        return refuse("Host simulation capabilities are invalid.");
    PSXModNetplayHostStartup context{};context.cpu=cpu;context.compiled_backends=compiled;
    context.available_backends=initialized&compiled;context.fp_contract=fp;
    context.interpreted_overlays_sealed=1;context.builtin_linked_bios=1;
    context.direct_loopback_scope=psx_netplay_direct_loopback_scope(&cfg);
    context.actual_executable_sha256=artifact;
    context.declared_build_manifest_sha256=psx_compiled_build_manifest_sha256();
    context.builtin_bios_image_id=builtin_bios->image->image_id;
    context.builtin_bios_image_wordsum=builtin_bios->image->image_wordsum;
    context.declared_build_manifest_schema=psx_compiled_build_manifest_schema();
    context.slot_count=cfg.slot_count;context.player_count=cfg.player_count;
    context.occupied_mask=cfg.occupied_mask;context.rollback=cfg.rollback;
    context.loading_enabled=psx_mod_netplay_loading();
    context.renderer=renderer;context.retained_texture_banks=retained_banks;
    context.aspect_index=psx_mod_netplay_aspect();context.observer=cfg.spectator?1:0;
    PSXHostProfileCatalog cat{};
    if(!title->host_profiles(&context,&cat)||!cat.rules||!cat.count||cat.count>PSX_HOST_MAX_OFFERS)
        return refuse("No qualified simulation profile is available for this host.");
    for(unsigned i=0;i<cat.count;++i){
        if(cfg.spectator&&!(cat.rules[i].flags&PSX_HOST_PROFILE_OBSERVER))continue;
        if(!cpu.features_known&&!(cat.rules[i].flags&PSX_HOST_PROFILE_PORTABLE))continue;
        if((cat.rules[i].flags&PSX_HOST_PROFILE_DIRECT_LOOPBACK)&&!context.direct_loopback_scope)continue;
        (void)psx_host_caps_offer(&caps,cat.rules+i,compiled);
    }
    if(!caps.offer_count||!psx_host_caps_validate(&caps,&cat))
        return refuse("No qualified simulation profile matches this CPU, backend and session route.");
    if(!psx_netplay_host_profile_register(&caps,&cat))
        return refuse("Host simulation policy could not be installed before transport.");
    char hex[65];const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){hex[i*2]=digits[artifact[i]>>4];hex[i*2+1]=digits[artifact[i]&15];}hex[64]=0;
    std::fprintf(stderr,"psxrecomp: actual-image-sha256=%s cpu-arch=%u features=%llx backend=%u fp=%u overlays=interpreted offers=%u\n",
        hex,cpu.architecture,(unsigned long long)cpu.features,compiled,fp,caps.offer_count);
    return true;
}
/* The initial admit barrier has completed. This is called exactly once before
 * guest dispatch; transport RUNNING alone is deliberately insufficient. */
static bool psx_log_required_host_profile_settled(uint64_t cycles,uint32_t tick,std::string& error){
    error.clear();
    PSXHostSelection selected{};
    if(psx_netplay_host_profile_failed()||!psx_netplay_host_profile_ready()||
       !psx_netplay_host_profile_selection(&selected)){
        error="The host simulation agreement did not settle before gameplay.";return false;
    }
    if(psx_netplay_is_spectator()){
        uint64_t initial_cycles=0;uint32_t initial_tick=0;
        if(!psx_netplay_host_profile_initial_settlement(&initial_cycles,&initial_tick)||initial_cycles){
            error="The observer did not settle its host profile before local guest execution.";return false;
        }
        cycles=initial_cycles;tick=initial_tick;
    }else if(cycles||tick){error="The host simulation agreement was reached after guest execution.";return false;}
    char hex[65];const char digits[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){hex[i*2]=digits[selected.identity[i]>>4];hex[i*2+1]=digits[selected.identity[i]&15];}hex[64]=0;
    std::fprintf(stderr,"psxrecomp: host profile settled profile=%s revision=%u selected_sha256=%s cycles=%llu sim_tick=%u before-enter-guest=1\n",
        selected.profile,selected.revision,hex,(unsigned long long)cycles,tick);
    return true;
}
#endif
