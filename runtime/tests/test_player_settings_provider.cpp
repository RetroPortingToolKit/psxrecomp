#include "config_loader.h"
#include "player_settings.h"
#include "recomp_launcher.h"
#include <map>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <fstream>
#include <chrono>

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
static int g_video_geometry_correction,g_video_perspective_texturing,controller_anti_deadzone;
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
    const auto path=std::filesystem::temp_directory_path()/
        ("player-settings-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".toml");
    struct Cleanup { std::filesystem::path p; ~Cleanup(){std::error_code ec;std::filesystem::remove(p,ec);} } cleanup{path};
    { std::ofstream f(path); f<<"[video]\nadaptive_view=true\n[parappa_timing]\nmode='stock'\nextra_early=2\nextra_late=3\n"; }
    RecompLauncherCModProvider original{};
    original.feature_count=no_features;original.feature_get=no_feature;
    original.feature_option_get=no_option;original.feature_set_option=no_set;original.feature_enable=no_enable;
    player_settings_configure(path,player_settings_baseline(false,false,false,100,"cards"));
    const auto* provider=player_settings_provider(&original);
    assert(provider->feature_count(provider->ctx)==2);
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
        {"video.dynamic_resolution_min","2x"},{"video.match_display_max_lines","1080"},
        {"video.supersample_disable_at_lines","2160"},{"video.geometry_correction","true"},
        {"video.perspective_texturing","false"},{"runtime.bios_hle_keep_intro","true"},
        {"audio.buffer_ms","64"},{"controller.anti_deadzone","2048"},
        {"controller.experimental_jogcon_force_feedback_invert","true"},{"controller.select_shortcuts","false"}
    };
    int controls=0;
    for(int i=0;i<2;++i) {
        RecompLauncherCModFeature feature{};assert(provider->feature_get(provider->ctx,i,&feature));
        for(int j=0;j<feature.option_count;++j) {
            RecompLauncherCModOption option{};
            assert(provider->feature_option_get(provider->ctx,feature.package_id,feature.id,j,&option));
            auto edit=edits.find(option.id);assert(edit!=edits.end());
            assert(provider->feature_set_option(provider->ctx,feature.package_id,feature.id,option.id,edit->second.c_str()));
            ++controls;
        }
    }
    assert(controls==35);
    assert(!provider->feature_set_option(nullptr,kPlayerSettingsPackage,"general","audio.buffer_ms","2147483648"));
    assert(!provider->feature_set_option(nullptr,kPlayerSettingsPackage,"display","video.pgxp_tolerance","nan"));
    assert(!provider->feature_set_option(nullptr,kPlayerSettingsPackage,"display","video.match_display_max_lines","1"));
    auto prefs=PSXRecompV4::load_user_settings(path);
    assert(!prefs.parse_error && prefs.extra_player_settings.size()==28);
    assert(prefs.has_fast_boot && prefs.fast_boot && prefs.has_bios_hle && prefs.bios_hle);
    assert(prefs.fov_scale==1.5 && !prefs.low_latency_input && prefs.mouse_sensitivity==2.5 && prefs.mouse_capture);
    assert(prefs.memcard_dir=="cards/test");
    bool intro=false;int audio=100;
    apply_extra_player_settings(prefs,intro,audio);
    assert(intro && audio==64 && controller_anti_deadzone==2048 && g_jogcon_ffb_invert && !g_title_select_shortcuts);
    assert(g_video_post_aa==2 && g_video_bloom==0.75f && g_video_post_shader=="filters/test.pfx");
    assert(g_video_ss_milli==1500 && g_title_ss_milli==1500);
    assert(g_video_pgxp_tolerance==-1 && g_video_pgxp_tolerance_set && !g_video_pgxp_position_fallback && g_video_pgxp_position_fallback_set);
    assert(g_video_pgxp_preserve_projection && g_video_pgxp_depth_buffer && g_video_fmv_chroma);
    assert(g_video_texture_lod==1 && g_video_aniso==8 && g_video_accurate_blending && g_video_dithering==2);
    assert(g_video_pgxp_color_correction && g_video_pgxp_seam==2 && g_video_pgxp_depth_threshold==2048.5f);
    assert(!texture_batching && g_frame_generation_method==1 && g_video_dynres_min==PSX_IR_SCALE(2));
    assert(g_video_match_max_lines==1080 && g_video_ss_disable_at_lines==2160);
    assert(g_video_geometry_correction && !g_video_perspective_texturing && g_player_geometry_set && g_player_texture_set);
    PSXRecompV4::UserSettings old_snapshot;
    player_settings_merge(old_snapshot);
    assert(old_snapshot.extra_player_settings==prefs.extra_player_settings && old_snapshot.fast_boot);
    assert(old_snapshot.has_adaptive_view && old_snapshot.adaptive_view);
    assert(old_snapshot.has_parappa_timing_mode && old_snapshot.parappa_timing_extra_early==2 && old_snapshot.parappa_timing_extra_late==3);
    assert(PSXRecompV4::save_user_settings(path,old_snapshot));
    const auto again=PSXRecompV4::load_user_settings(path);
    assert(again.extra_player_settings==prefs.extra_player_settings && again.adaptive_view);
    puts("PASS:35 actual provider edits -> TOML load -> startup owners; bounds/old snapshot/stub preservation");
    return 0;
}
