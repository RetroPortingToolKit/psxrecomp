// Reuse the real typed provider/startup fixture, with the production host owner.
#define PSX_PLAYER_SETTINGS_REAL_HOST
#define main unused_provider_fixture_main
#include "test_player_settings_provider.cpp"
#undef main
#include "legacy_player_settings.h"
#include "mod_packages.h"
#include "pgxp_session.h"
#include "toml.hpp"

static std::string read(const std::filesystem::path& p) {
    std::ifstream in(p,std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),{});
}
static void write(const std::filesystem::path& p,const std::string& text) {
    std::ofstream out(p,std::ios::binary);out<<text;assert(out.good());
}
static void state(const std::filesystem::path& root,bool enabled) {
    std::ofstream out(root/"mods/state.toml");
    out<<"format_version=2\n[quick_mods]\n'custom/keep'=true\n[sources]\nfixture='shared/source'\n";
    auto feature=[&](const char* package,const char* id,const char* values) {
        out<<"\n[[feature]]\npackage_id='"<<package<<"'\nid='"<<id<<"'\nenabled="<<(enabled?"true":"false")<<"\n"<<values;
    };
    feature("psx.enhancement.internal-resolution","internal-resolution",
            "[feature.values]\nresolution='8x'\nminimum='720p'\nsupersample='1.5'\ndynamic_resolution='on'\n");
    feature("psx.enhancement.crt-filter","crt-filter",
            "[feature.values]\nscreen_model='composite'\nscanlines='on'\nscanline_strength='37'\n");
    feature("psx.enhancement.image-filtering","image-filtering",
            "[feature.values]\nsmoothing='sharp'\ntexture_filtering='xbr'\nfmv_filtering='bicubic'\ndithering='native'\n");
    feature("psx.enhancement.enhanced-audio","enhanced-audio","");
    feature("psx.enhancement.pgxp","pgxp",
            "[feature.values]\ngeometry='true'\nperspective='false'\ncpu_mode='true'\nculling='true'\n");
    feature("psx.feature.rewind","rewind","[feature.values]\nbuffer='100'\ninterval='4'\n");
    feature("psx.enhancement.post-processing","post-processing",
            "[feature.values]\nantialiasing='fxaa_hq'\nbloom='on'\nbloom_strength='75'\nshader='on'\n"
            "[feature.resources]\nshader='shaders/owner.pfx'\n");
    feature("fixture.custom","keep","[feature.values]\nchoice='untouched'\n");
    assert(out.good());
}
static PSXRecompV4::UserSettings startup(const std::filesystem::path& path,
                                        const std::filesystem::path& title = {}) {
    std::string error;
    assert(PSXRecompV4::preserve_legacy_player_settings(path,&error,title));
    auto prefs=PSXRecompV4::load_user_settings(path);assert(!prefs.parse_error);
    bool intro=false;int audio=100;apply_extra_player_settings(prefs,intro,audio);
    player_settings_configure(path,player_settings_baseline(false,false,intro,audio,"cards"));
    return prefs;
}
static PsxHostEnhRequests requests() {
    psx_host_enh_reset();PsxHostEnhRequests r{};psx_host_enh_requests(&r);return r;
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/
        ("legacy-player-settings-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root/"mods");
    struct Cleanup { std::filesystem::path p;~Cleanup(){std::error_code ec;std::filesystem::remove_all(p,ec);} } cleanup{root};
    const auto path=root/"settings.toml", card=root/"native.mcr";
    write(card,"native persistent progress fixture");
    state(root,true);
    PSXRecompV4::ModPackageManager saved(root/"mods");assert(saved.load_state());
    assert(saved.selections().at("psx.enhancement.pgxp").features.at("pgxp").has_enabled);
    const auto old_state=read(root/"mods/state.toml"),old_card=read(card);
    write(path,"settings_format=2\n[video]\ninternal_resolution='2x'\ngeometry_correction=false\nadaptive_view=true\n"
               "[audio]\nvolume=37\nbuffer_ms=123\n[controller]\nmouse_sensitivity=2.5\n"
               "[memcard]\ndir='custom/cards'\n[owner_extra]\nkeep=['one','two']\n");
    auto prefs=startup(path);
    if (!prefs.extra_player_settings.count("video.crt_filter")) {
        std::fputs("FAIL: saved CRT choice was lost before the Settings provider/startup applied it\n",stderr);
        return 1;
    }
    assert(prefs.extra_player_settings.at("video.crt_filter")=="composite");
    assert(prefs.extra_player_settings.at("video.internal_resolution")=="2x");
    assert(prefs.extra_player_settings.at("video.geometry_correction")=="false");
    assert(prefs.has_volume && prefs.volume==37 && prefs.memcard_dir=="custom/cards" && prefs.mouse_sensitivity==2.5);
    assert(toml::find<std::vector<std::string>>(toml::parse(path.string()),"owner_extra","keep")==std::vector<std::string>({"one","two"}));
    auto r=requests();
    PsxHostLook baseline{3,0,60,1,PSX_TEXTURE_FILTER_BILINEAR,0,0,2};
    const auto look=psx_host_enh_resolve(&baseline,&r);
    assert(look.screen_kind==2 && look.scanlines && look.scanline_strength_pct==37);
    assert(!look.present_linear && look.texture_filter==PSX_TEXTURE_FILTER_XBR && look.fmv_filter==3 && look.spu_hq && look.dithering==1);
    PsxHostResolution resolution{1080,0,PSX_IR_NATIVE,1000};
    const auto scaled=psx_host_enh_resolve_resolution(&resolution,&r);
    assert(scaled.internal_resolution==PSX_IR_SCALE(2) && scaled.dynamic_resolution && scaled.dynamic_resolution_min==720 && scaled.supersample_milli==1500);
    PsxHostPostLook post_baseline{0,0,"title.pfx"};
    const auto post=psx_host_enh_resolve_post(&post_baseline,&r);
    assert(post.antialiasing==2 && post.bloom==0.75f && std::string(post.shader)=="shaders/owner.pfx");
    assert(r.rewind && r.rewind_depth==100 && r.rewind_interval==4);
    assert(!g_video_geometry_correction && !g_video_perspective_texturing && g_video_pgxp_cpu_mode && g_player_pgxp_culling);
    PSXPgxpSessionInputs input{};input.env_geometry=input.env_texture=input.env_cpu_mode=input.env_culling=-1;
    input.video_geometry=g_video_geometry_correction;input.video_texture=g_video_perspective_texturing;
    input.player_geometry_set=g_player_geometry_set;input.player_texture_set=g_player_texture_set;
    input.video_cpu_mode=g_video_pgxp_cpu_mode;input.player_cpu_mode_set=g_player_pgxp_cpu_set;input.player_culling=g_player_pgxp_culling;
    auto arm=psx_pgxp_session_resolve(&input);assert(!arm.geometry && !arm.texture && arm.cpu_mode && !arm.culling);
    RecompLauncherCModProvider original{};original.feature_count=no_features;original.feature_get=no_feature;
    original.feature_option_get=no_option;original.feature_set_option=no_set;original.feature_enable=no_enable;
    const auto* provider=player_settings_provider(&original);
    bool default_label=false,display_label=false;
    for (int i=0;;++i) {
        RecompLauncherCModChoice c{};
        if (!provider->feature_choice_get(provider->ctx,kPlayerSettingsPackage,"display","video.internal_resolution",i,&c)) break;
        if (std::string(c.value)=="default") default_label=std::string(c.label)=="Game default";
        if (std::string(c.value)=="display") display_label=std::string(c.label)=="Match display";
    }
    assert(default_label && display_label);
    bool found=false;
    for (int i=0;;++i) {
        RecompLauncherCModOption option{};
        if (!provider->feature_option_get(provider->ctx,kPlayerSettingsPackage,"display",i,&option)) break;
        if (std::string(option.id)=="video.crt_filter") { assert(std::string(option.value)=="composite");found=true; }
    }
    assert(found);
    const auto once=read(path);const auto stamp=std::filesystem::last_write_time(path);
    startup(path);assert(read(path)==once && std::filesystem::last_write_time(path)==stamp);
    assert(read(root/"mods/state.toml")==old_state && read(card)==old_card);
    // A new provider edit owns its choice on every later launch, despite stale mod state.
    assert(provider->feature_set_option(provider->ctx,kPlayerSettingsPackage,"display","video.crt_filter","raw"));
    assert(provider->feature_set_option(provider->ctx,kPlayerSettingsPackage,"general","audio.spu_hq","false"));
    prefs=startup(path);r=requests();assert(psx_host_enh_resolve(&baseline,&r).screen_kind==0 && !psx_host_enh_resolve(&baseline,&r).spu_hq);
    assert(prefs.legacy_builtin_settings_migrated);
    PSXRecompV4::UserSettings older_snapshot;player_settings_merge(older_snapshot);
    assert(older_snapshot.legacy_builtin_settings_migrated);
    auto consumed=toml::parse(path.string());consumed["video"].as_table().erase("crt_filter");
    { std::ofstream out(path);out<<consumed; }
    prefs=startup(path);assert(!prefs.extra_player_settings.count("video.crt_filter"));
    assert(read(root/"mods/state.toml")==old_state && read(card)==old_card);

    // Disabled mods left the title baseline. Dormant saved option values must
    // not turn a nontrivial title baseline off or reactivate an enhancement.
    state(root,false);write(path,"[video]\nadaptive_view=true\n[audio]\nvolume=37\n");
    g_video_geometry_correction=g_video_perspective_texturing=g_video_pgxp_cpu_mode=1;
    g_player_geometry_set=g_player_texture_set=0;
    prefs=startup(path);r=requests();
    for (const char* id:{"video.crt_filter","video.antialiasing","audio.spu_hq","video.geometry_correction",
                        "video.perspective_texturing","video.pgxp_cpu_mode","video.pgxp_culling","video.bloom","video.post_shader"})
        assert(!prefs.extra_player_settings.count(id));
    assert(prefs.extra_player_settings.at("video.internal_resolution")=="1x" && !r.rewind);
    baseline={3,1,63,1,PSX_TEXTURE_FILTER_BILINEAR,3,1,2};
    const auto unchanged=psx_host_enh_resolve(&baseline,&r);
    assert(unchanged.screen_kind==3 && unchanged.scanlines && unchanged.scanline_strength_pct==63 && unchanged.present_linear);
    assert(unchanged.texture_filter==PSX_TEXTURE_FILTER_BILINEAR && unchanged.fmv_filter==3 && unchanged.spu_hq && unchanged.dithering==2);
    resolution={1440,1,720,2000};const auto title_resolution=psx_host_enh_resolve_resolution(&resolution,&r);
    assert(title_resolution.internal_resolution==PSX_IR_SCALE(1) && !title_resolution.dynamic_resolution && title_resolution.supersample_milli==1000);
    post_baseline={1,0.5f,"title.pfx"};const auto title_post=psx_host_enh_resolve_post(&post_baseline,&r);
    assert(title_post.antialiasing==1 && title_post.bloom==0.5f && std::string(title_post.shader)=="title.pfx");
    input={};input.env_geometry=input.env_texture=input.env_cpu_mode=input.env_culling=-1;
    input.video_geometry=g_video_geometry_correction;input.video_texture=g_video_perspective_texturing;input.video_cpu_mode=g_video_pgxp_cpu_mode;
    input.player_geometry_set=g_player_geometry_set;input.player_texture_set=g_player_texture_set;input.player_cpu_mode_set=g_player_pgxp_cpu_set;
    arm=psx_pgxp_session_resolve(&input);assert(arm.geometry && arm.texture && arm.cpu_mode);

    // All former saved resolution spellings remain loadable, including Auto.
    for (const char* choice:{"default","native","720p","1080p","1440p","4k","5k","8k","display","auto","32x"}) {
        write(root/"mods/state.toml",std::string("format_version=2\n[[feature]]\npackage_id='psx.enhancement.internal-resolution'\nid='internal-resolution'\nenabled=true\n[feature.values]\nresolution='")+choice+"'\n");
        write(path,"");prefs=startup(path);assert(prefs.extra_player_settings.at("video.internal_resolution")==choice);
        r=requests();int expected=PSX_IR_UNSET;if (std::string(choice)!="default") assert(psx_ir_parse(choice,&expected));
        assert(r.resolution_value==expected);
        if (std::string(choice)=="default") {
            const auto title_default=psx_host_enh_resolve_resolution(&resolution,&r);
            assert(title_default.internal_resolution==1440 && title_default.dynamic_resolution && title_default.supersample_milli==2000);
        }
    }
    write(root/"mods/state.toml","format_version=1\n[[package]]\nid='psx.enhancement.internal-resolution'\nenabled=true\n[package.values]\nresolution='3x'\n");
    write(path,"");prefs=startup(path);assert(prefs.extra_player_settings.at("video.internal_resolution")=="3x");
    // A title may move its fresh PGXP default to Settings without changing what
    // a player's explicit old Off meant, including profiles migrated earlier.
    const auto title=root/"game.toml";
    const std::string baseline_off="[legacy_player_settings.pgxp_disabled_baseline]\n"
        "geometry_correction=false\nperspective_texturing=false\npgxp_cpu_mode=false\npgxp_culling=false\n";
    write(title,baseline_off);state(root,false);
    const auto off_state=read(root/"mods/state.toml");
    const char* pgxp_keys[]={"video.geometry_correction","video.perspective_texturing",
                            "video.pgxp_cpu_mode","video.pgxp_culling"};
    for (bool migrated:{false,true}) {
        write(path,migrated ? "legacy_builtin_settings_migrated=true\n" : "");
        prefs=startup(path,title);
        for (const char* key:pgxp_keys) {
            if (!prefs.extra_player_settings.count(key)) {
                std::fputs("FAIL: explicit old PGXP Off did not preserve the declared title baseline\n",stderr);
                return 1;
            }
            assert(prefs.extra_player_settings.at(key)=="false");
        }
        const auto off_once=read(path);const auto off_stamp=std::filesystem::last_write_time(path);
        startup(path,title);assert(read(path)==off_once && std::filesystem::last_write_time(path)==off_stamp);
    }
    // Individual explicit canonical keys win; the declared baseline need not
    // be all Off. Dormant old option values cannot override that baseline.
    write(title,"[legacy_player_settings.pgxp_disabled_baseline]\ngeometry_correction=false\n"
        "perspective_texturing=false\npgxp_cpu_mode=false\npgxp_culling=true\n");
    write(path,"legacy_builtin_settings_migrated=true\n[video]\ngeometry_correction=true\n");
    prefs=startup(path,title);
    assert(prefs.extra_player_settings.at(pgxp_keys[0])=="true");
    assert(prefs.extra_player_settings.at(pgxp_keys[1])=="false");
    assert(prefs.extra_player_settings.at(pgxp_keys[2])=="false");
    assert(prefs.extra_player_settings.at(pgxp_keys[3])=="true");
    assert(read(root/"mods/state.toml")==off_state && read(card)==old_card);
    // Existing On/options retain their original meaning, independent of the
    // title's disabled baseline or its new fresh defaults.
    write(title,baseline_off);state(root,true);write(path,"");prefs=startup(path,title);
    assert(prefs.extra_player_settings.at(pgxp_keys[0])=="true");
    assert(prefs.extra_player_settings.at(pgxp_keys[1])=="false");
    assert(prefs.extra_player_settings.at(pgxp_keys[2])=="true");
    assert(prefs.extra_player_settings.at(pgxp_keys[3])=="true");
    // v1 explicit Off without options differs from a modern version-only pin.
    write(root/"mods/state.toml","format_version=1\n[[package]]\nid='psx.enhancement.pgxp'\nenabled=false\n");
    write(path,"legacy_builtin_settings_migrated=true\n");prefs=startup(path,title);
    for (const char* key:pgxp_keys) assert(prefs.extra_player_settings.at(key)=="false");
    write(root/"mods/state.toml","format_version=2\n[[package]]\nid='psx.enhancement.pgxp'\nversion='1.0.0'\n");
    write(path,"");prefs=startup(path,title);
    for (const char* key:pgxp_keys) assert(!prefs.extra_player_settings.count(key));
    // A partial or mistyped baseline is rejected before publishing preferences.
    state(root,false);write(path,"legacy_builtin_settings_migrated=true\n");
    const auto unmodified=read(path);std::string baseline_error;
    write(title,"[legacy_player_settings.pgxp_disabled_baseline]\ngeometry_correction=false\n");
    assert(!PSXRecompV4::preserve_legacy_player_settings(path,&baseline_error,title) && read(path)==unmodified);
    write(title,baseline_off+"unexpected=true\n");
    assert(!PSXRecompV4::preserve_legacy_player_settings(path,&baseline_error,title) && read(path)==unmodified);
    assert(read(card)==old_card && read(root/"mods/state.toml")==off_state);

    // Invalid new settings and malformed inputs are preserved, never overwritten.
    write(path,"[video]\ninternal_resolution='owner-invalid-value'\n");
    std::string error;
    assert(PSXRecompV4::preserve_legacy_player_settings(path,&error));
    assert(toml::find<std::string>(toml::parse(path.string()),"video","internal_resolution")=="owner-invalid-value");
    write(path,"[broken\n");const auto broken=read(path);
    assert(!PSXRecompV4::preserve_legacy_player_settings(path,&error) && read(path)==broken);
    write(path,"[audio]\nvolume=37\n");const auto good=read(path);
    write(root/"mods/state.toml","[broken\n");
    assert(!PSXRecompV4::preserve_legacy_player_settings(path,&error) && read(path)==good && read(card)==old_card);
    std::filesystem::remove(root/"mods/state.toml");std::filesystem::remove(path);
    assert(PSXRecompV4::preserve_legacy_player_settings(path,&error) && !std::filesystem::exists(path));
    puts("PASS: real saved mod state -> absent typed preferences -> persistence/provider/startup/host/PGXP; new choices win, baseline follows disabled mods, repeated launch no writes, unrelated state/cards and malformed input preserved");
}
