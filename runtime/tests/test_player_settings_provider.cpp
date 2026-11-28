#include "config_loader.h"
#include "player_settings.h"
#include "recomp_launcher.h"
#include "host_enhancements.h"
#include "video_texture_filter.h"
#include "psx_path_utf8.h"
#include <map>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <fstream>
#include <chrono>
#include <algorithm>

/* Startup's real apply function writes these ordinary runtime owners. The
 * renderer's setter is observed without creating a context or window. */
static int g_video_post_aa, g_video_ss_milli=1000, g_title_ss_milli=1000;
static float g_video_bloom, g_video_pgxp_tolerance=0.5f, g_video_pgxp_depth_threshold=4096;
static std::string g_video_post_shader;
static int g_video_pgxp_tolerance_set, g_video_pgxp_position_fallback_set;
static int g_video_pgxp_position_fallback=1,g_video_pgxp_preserve_projection;
static int g_video_pgxp_depth_buffer,g_video_fmv_chroma,g_video_texture_lod,g_video_aniso=1;
static int g_video_accurate_blending,g_video_dithering=1,g_video_pgxp_color_correction,g_video_pgxp_seam;
static int g_frame_generation_method,g_video_dynres_min=720,g_video_match_max_lines,g_video_ss_disable_at_lines;
static int g_video_dynres;
/* The baseline the player's Internal resolution resolves over (host_enhancements.h). */
static struct { int internal_resolution,dynamic_resolution,dynamic_resolution_min,supersample_milli; }
    g_title_resolution={0,0,720,1000};
static int g_video_geometry_correction,g_video_perspective_texturing,controller_anti_deadzone;
static int g_video_pgxp_cpu_mode;
/* The title's look (main.cpp) and the player's host enhancement record
 * (host_enhancements.c), observed instead of linked. */
static PsxHostLook g_title_look={0,0,50,1,0,0,0,1};
static std::map<std::string,std::string> g_host_enh_player;
#ifndef PSX_PLAYER_SETTINGS_REAL_HOST
extern "C" void psx_host_enh_player_clear(void) { g_host_enh_player.clear(); }
extern "C" int psx_host_enh_player_set(const char* id,const char* value) { g_host_enh_player[id]=value; return 1; }
#endif
static bool g_jogcon_ffb_invert;
static int g_title_select_shortcuts=1,g_player_title_select_allowed=1;
static double g_fov_scale=1,g_mouse_sensitivity=1;
static int g_low_latency_input=1;
static bool g_mouse_capture_wanted;
static int texture_batching;
static void gl_renderer_set_texture_window_batching(int value) { texture_batching=value; }
#include "../src/launcher_player_settings.inc"
#include "../src/player_startup_settings.inc"

static int no_features(void*) { return 0; }
static int no_feature(void*,int,RecompLauncherCModFeature*) { return 0; }
static int no_option(void*,const char*,const char*,int,RecompLauncherCModOption*) { return 0; }
static int no_set(void*,const char*,const char*,const char*,const char*) { return 0; }
static int no_enable(void*,const char*,const char*,int) { return 0; }

int main() {
    const std::string unicode_utf8="cards/\xE6\x97\xA5\xE6\x9C\xAC-\xC3\xA9";
    const auto unicode_path=std::filesystem::u8path(unicode_utf8);
    const auto unicode_baseline=player_settings_baseline(false,false,false,100,unicode_path);
    assert(unicode_baseline.at("memcard.dir")==unicode_utf8);
    PSXRecompV4::UserSettings unicode_preferences;
    unicode_preferences.has_memcard_dir=true;unicode_preferences.memcard_dir=unicode_path;
    std::string exposed_path;
    assert(player_setting_typed_get(unicode_preferences,"memcard.dir",exposed_path));
    assert(exposed_path==unicode_utf8);
    const auto path=std::filesystem::temp_directory_path()/
        ("player-settings-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".toml");
    struct Cleanup { std::filesystem::path p; ~Cleanup(){std::error_code ec;std::filesystem::remove(p,ec);} } cleanup{path};
    { std::ofstream f(path); f<<"[video]\nadaptive_view=true\n[parappa_timing]\nmode='stock'\nextra_early=2\nextra_late=3\n"; }
    RecompLauncherCModProvider original{};
    original.feature_count=no_features;original.feature_get=no_feature;
    original.feature_option_get=no_option;original.feature_set_option=no_set;original.feature_enable=no_enable;
    player_settings_configure(path,player_settings_baseline(false,false,false,100,"cards"));
    const auto* provider=player_settings_provider(&original);
    assert(provider->feature_count(provider->ctx)==3);
    assert(!provider->feature_option_get(provider->ctx,kPlayerSettingsPackage,"invalid",0,nullptr));
    const std::map<std::string,std::string> edits={
        {"video.fov_scale","1.5"},{"video.fast_boot","true"},{"video.bios_hle","true"},
        {"video.low_latency_input","false"},{"memcard.dir","cards/test"},
        {"controller.mouse_sensitivity","2.5"},{"controller.mouse_capture","true"},
        {"video.antialiasing_mode","fxaa_hq"},{"video.bloom","0.75"},
        {"video.post_shader","filters/test.pfx"},{"video.supersample","1.5"},
        {"video.pgxp_tolerance","-1"},{"video.pgxp_position_fallback","false"},
        {"video.pgxp_preserve_projection","true"},{"video.pgxp_depth_buffer","true"},
        {"video.fmv_chroma_smoothing","true"},{"video.texture_lod","mipmap"},
        {"video.anisotropic_filtering","8"},{"video.accurate_blending","true"},
        {"video.dithering","fine"},{"video.pgxp_color_correction","true"},
        {"video.pgxp_seam","wide"},{"video.pgxp_depth_threshold","2048.5"},
        {"video.texture_window_batching","false"},{"video.frame_generation_method","reprojection"},
        {"video.dynamic_resolution","true"},{"video.dynamic_resolution_min","2x"},{"video.match_display_max_lines","1080"},
        {"video.supersample_disable_at_lines","2160"},{"video.geometry_correction","true"},
        {"video.perspective_texturing","false"},{"runtime.bios_hle_keep_intro","true"},
        {"audio.buffer_ms","064"},{"controller.anti_deadzone","2048"},
        {"controller.experimental_jogcon_force_feedback_invert","true"},{"controller.select_shortcuts","false"},
        {"video.internal_resolution","8x"},{"video.crt_filter","trinitron"},{"video.scanlines","true"},
        {"video.scanline_strength","0.25"},{"video.antialiasing","false"},{"video.texture_filtering","xbr"},
        {"video.fmv_filter","bicubic"},{"video.pgxp_cpu_mode","true"},{"video.pgxp_culling","true"},
        {"audio.spu_hq","true"},{"video.rewind","true"},{"video.rewind_depth","100"},{"video.rewind_interval","4"}
    };
    int controls=0;
    std::map<std::string,std::string> placed; // option id -> "feature group/option group"
    std::string headings;                     // Display headings in order, collapsed
    for(int i=0;i<3;++i) {
        RecompLauncherCModFeature feature{};assert(provider->feature_get(provider->ctx,i,&feature));
        for(int j=0;j<feature.option_count;++j) {
            RecompLauncherCModOption option{};
            assert(provider->feature_option_get(provider->ctx,feature.package_id,feature.id,j,&option));
            placed[option.id]=std::string(feature.group)+"/"+option.group;
            if(!std::strcmp(feature.id,"display") && (headings.empty() || headings.substr(headings.rfind('|')+1)!=option.group))
                headings+=std::string("|")+option.group;
            auto edit=edits.find(option.id);assert(edit!=edits.end());
            assert(provider->feature_set_option(provider->ctx,feature.package_id,feature.id,option.id,edit->second.c_str()));
            ++controls;
        }
    }
    assert(controls==49);
    /* The launcher layout: Internal resolution on Display, the CRT filter,
     * Image filtering and Automatic resolution sections on Display, the four
     * PGXP settings on PGXP, and Enhanced audio and Rewind on General. */
    for(const char* id:{"video.geometry_correction","video.perspective_texturing",
                        "video.pgxp_cpu_mode","video.pgxp_culling"}) assert(placed.at(id)=="PGXP/PGXP");
    assert(placed.at("video.internal_resolution")=="Display/Display");
    for(const char* id:{"video.crt_filter","video.scanlines","video.scanline_strength"})
        assert(placed.at(id)=="Display/CRT filter");
    for(const char* id:{"video.antialiasing","video.texture_filtering","video.fmv_filter"})
        assert(placed.at(id)=="Display/Image filtering");
    assert(placed.at("audio.spu_hq")=="General/General");
    for(const char* id:{"video.rewind","video.rewind_depth","video.rewind_interval"})
        assert(placed.at(id)=="General/Rewind");
    for(const char* id:{"video.dynamic_resolution","video.dynamic_resolution_min","video.match_display_max_lines",
                        "video.supersample","video.supersample_disable_at_lines"})
        assert(placed.at(id)=="Display/Automatic resolution");
    assert(std::count_if(placed.begin(),placed.end(),[](const auto& item){return item.second.rfind("PGXP/",0)==0;})==4);
    assert(std::count_if(placed.begin(),placed.end(),[](const auto& item){return item.second=="Display/Automatic resolution";})==5);
    assert(headings=="|Display|CRT filter|Image filtering|Automatic resolution");
    assert(!provider->feature_set_option(nullptr,kPlayerSettingsPackage,"display","video.geometry_correction","true"));
    assert(!provider->feature_set_option(nullptr,kPlayerSettingsPackage,"general","audio.buffer_ms","2147483648"));
    assert(!provider->feature_set_option(nullptr,kPlayerSettingsPackage,"display","video.pgxp_tolerance","nan"));
    assert(!provider->feature_set_option(nullptr,kPlayerSettingsPackage,"display","video.match_display_max_lines","1"));
    // Leading-zero editor input must save as legal TOML and reload as integer64.
    auto prefs=PSXRecompV4::load_user_settings(path);
    assert(!prefs.parse_error && prefs.extra_player_settings.size()==42);
    assert(prefs.has_fast_boot && prefs.fast_boot && prefs.has_bios_hle && prefs.bios_hle);
    assert(prefs.fov_scale==1.5 && !prefs.low_latency_input && prefs.mouse_sensitivity==2.5 && prefs.mouse_capture);
    assert(prefs.memcard_dir=="cards/test");
    bool intro=false;int audio=100;
    apply_extra_player_settings(prefs,intro,audio);
    assert(intro && audio==64 && controller_anti_deadzone==2048 && g_jogcon_ffb_invert && !g_title_select_shortcuts);
    assert(g_video_post_aa==2 && g_video_bloom==0.75f && g_video_post_shader=="filters/test.pfx");
    assert(g_video_ss_milli==1500 && g_title_ss_milli==1500 && g_title_resolution.supersample_milli==1500);
    assert(g_video_dynres && g_title_resolution.dynamic_resolution && g_title_resolution.dynamic_resolution_min==PSX_IR_SCALE(2));
    assert(g_video_pgxp_tolerance==-1 && g_video_pgxp_tolerance_set && !g_video_pgxp_position_fallback && g_video_pgxp_position_fallback_set);
    assert(g_video_pgxp_preserve_projection && g_video_pgxp_depth_buffer && g_video_fmv_chroma);
    assert(g_video_texture_lod==1 && g_video_aniso==8 && g_video_accurate_blending && g_video_dithering==2);
    assert(g_video_pgxp_color_correction && g_video_pgxp_seam==2 && g_video_pgxp_depth_threshold==2048.5f);
    assert(!texture_batching && g_frame_generation_method==1 && g_video_dynres_min==PSX_IR_SCALE(2));
    assert(g_video_match_max_lines==1080 && g_video_ss_disable_at_lines==2160);
    assert(g_video_geometry_correction && !g_video_perspective_texturing && g_player_geometry_set && g_player_texture_set);
    assert(g_video_pgxp_cpu_mode && g_player_pgxp_cpu_set && g_player_pgxp_culling);
#ifdef PSX_PLAYER_SETTINGS_REAL_HOST
    psx_host_enh_reset();
    PsxHostEnhRequests real_requests;
    psx_host_enh_requests(&real_requests);
    assert(real_requests.resolution_value==PSX_IR_SCALE(8) && real_requests.crt_screen_kind==3);
    assert(real_requests.audio==1 && real_requests.rewind_depth==100);
#else
    assert(g_host_enh_player.at("video.internal_resolution")=="8x" && g_host_enh_player.at("video.crt_filter")=="trinitron");
    assert(g_host_enh_player.at("audio.spu_hq")=="true" && g_host_enh_player.at("video.rewind_depth")=="100");
#endif
    PSXRecompV4::UserSettings old_snapshot;
    player_settings_merge(old_snapshot);
    assert(old_snapshot.extra_player_settings==prefs.extra_player_settings && old_snapshot.fast_boot);
    assert(old_snapshot.has_adaptive_view && old_snapshot.adaptive_view);
    assert(old_snapshot.has_parappa_timing_mode && old_snapshot.parappa_timing_extra_early==2 && old_snapshot.parappa_timing_extra_late==3);
    assert(PSXRecompV4::save_user_settings(path,old_snapshot));
    // Combined #867/#872: volume and buffer settings share one TOML table.
    old_snapshot.has_volume=true; old_snapshot.volume=37;
    assert(PSXRecompV4::save_user_settings(path,old_snapshot));
    const auto again=PSXRecompV4::load_user_settings(path);
    assert(!again.parse_error && again.has_volume && again.volume==37);
    assert(again.extra_player_settings==prefs.extra_player_settings && again.adaptive_view);
    std::ifstream input(path); std::string text((std::istreambuf_iterator<char>(input)),{});
    const auto audio_table=text.find("[audio]");
    assert(audio_table!=std::string::npos && text.find("[audio]",audio_table+1)==std::string::npos);
    // The actual game.toml parser keeps the renderer enum identities too.
    const auto game_path=std::filesystem::path(path.string()+".game.toml");
    Cleanup game_cleanup{game_path};
    for(const auto& [name,mode]:std::map<std::string,int>{{"nearest",0},{"bilinear",1},{"stable",2},{"xbr",3}}) {
        { std::ofstream game(game_path);
          game<<"[game]\nname='Filter fixture'\nid='SLUS-TEST'\nexe='stock.exe'\n"
                "load_address='0x80010000'\nentry_pc='0x80010000'\ntext_size='0x1000'\n"
                "stack_base='0x801ffff0'\n[recompiler]\nseeds='seeds.txt'\n[video]\ntexture_filtering='"<<name<<"'\n"; }
        assert(PSXRecompV4::load_game_config(game_path).runtime.video_texture_filter==mode);
    }
    // Stable is the existing renderer mode2, not a second filtering path.
    RecompLauncherCModOption filtering{};
    bool found_filter=false;
    for(int i=0;provider->feature_option_get(nullptr,kPlayerSettingsPackage,"display",i,&filtering);++i)
        if(std::string(filtering.id)=="video.texture_filtering") { found_filter=true;break; }
    assert(found_filter && filtering.choice_count==4);
    bool found_stable=false;
    for(int i=0;i<filtering.choice_count;++i) {
        RecompLauncherCModChoice choice{};
        assert(provider->feature_choice_get(nullptr,kPlayerSettingsPackage,"display",filtering.id,i,&choice));
        if(std::string(choice.value)=="stable") {
            found_stable=true;
            assert(std::string(choice.label)=="Stable 3D (OpenGL)");
        }
    }
    assert(found_stable && std::string(filtering.description).find("bilinear")!=std::string::npos);
    assert(provider->feature_set_option(nullptr,kPlayerSettingsPackage,"display","video.texture_filtering","stable"));
    const auto stable_prefs=PSXRecompV4::load_user_settings(path);
    assert(!stable_prefs.parse_error && stable_prefs.extra_player_settings.at("video.texture_filtering")=="stable");
    apply_extra_player_settings(stable_prefs,intro,audio);
#ifdef PSX_PLAYER_SETTINGS_REAL_HOST
    PsxHostEnhRequests stable_requests;
    psx_host_enh_reset(); psx_host_enh_requests(&stable_requests);
    assert(stable_requests.image_texture_filter==PSX_TEXTURE_FILTER_STABLE_WORLD);
    assert(psx_host_enh_resolve(&g_title_look,&stable_requests).texture_filter==PSX_TEXTURE_FILTER_STABLE_WORLD);
#else
    assert(g_host_enh_player.at("video.texture_filtering")=="stable");
#endif
    g_title_look.texture_filter=PSX_TEXTURE_FILTER_STABLE_WORLD;
    const auto stable_baseline=player_settings_baseline(false,false,false,100,"cards");
    assert(stable_baseline.at("video.texture_filtering")=="stable");
    player_settings_configure(path,stable_baseline); // real next-launch re-read
    assert(s_player_setting_values.at("video.texture_filtering")=="stable");
    puts("PASS:36 actual provider edits -> TOML load -> startup owners; bounds/old snapshot/stub preservation");
    return 0;
}
