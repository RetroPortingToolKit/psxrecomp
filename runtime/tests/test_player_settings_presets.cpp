// Exercise UI presets against the runtime's real typed provider,
// TOML loader, startup consumers and ModPackageManager, using its own manifests.
#define main draw_fixture_main
#include "psx_settings_draw_test.cpp"
#undef main
#define main provider_fixture_main
#include "test_player_settings_provider.cpp"
#undef main
#include "mod_packages.h"
#include "pgxp_session.h"
static PSXRecompV4::ModPackageManager manager;
static std::vector<std::pair<std::string,std::string>> catalog;
static std::string provider_error;
static bool reject_edit=false;
static bool same_selection(const PSXRecompV4::ModSelection& a,const PSXRecompV4::ModSelection& b) {
    if (a.enabled!=b.enabled || a.version!=b.version || a.values!=b.values || a.features.size()!=b.features.size()) return false;
    for (const auto& [id,f]:a.features) {
        const auto found=b.features.find(id);if(found==b.features.end())return false;
        const auto& g=found->second;
        if(f.enabled!=g.enabled || f.has_enabled!=g.has_enabled || f.values!=g.values || f.resources!=g.resources)return false;
    }return true;
}
static int package_count(void*) { return (int)catalog.size(); }
static int package_feature(void*,int i,RecompLauncherCModFeature* out) {
    if (i<0 || i>=(int)catalog.size()) return 0;
    const auto& [pkg,id]=catalog[i];const auto* f=manager.selected_feature(pkg,id);const auto* p=manager.selected_package(pkg);
    *out={};snprintf(out->package_id,sizeof out->package_id,"%s",pkg.c_str());snprintf(out->id,sizeof out->id,"%s",id.c_str());
    snprintf(out->name,sizeof out->name,"%s",f->name.c_str());snprintf(out->group,sizeof out->group,"%s",f->group.c_str());
    out->enabled=manager.feature_enabled(pkg,id);
    for (const auto& o:p->options) out->option_count+=o.feature_id==id;
    return 1;
}
static int package_option(void*,const char* pkg,const char* id,int i,RecompLauncherCModOption* out) {
    const auto* p=manager.selected_package(pkg);if (!p)return 0;
    for (const auto& o:p->options) if (o.feature_id==id && i--==0) {
        *out={};snprintf(out->id,sizeof out->id,"%s",o.id.c_str());snprintf(out->label,sizeof out->label,"%s",o.label.c_str());
        snprintf(out->description,sizeof out->description,"%s",o.description.c_str());
        snprintf(out->value,sizeof out->value,"%s",manager.feature_option_value(pkg,id,o.id).c_str());
        out->type=o.type==PSXRecompV4::ModOptionType::Boolean?RECOMP_MOD_OPTION_BOOLEAN:o.type==PSXRecompV4::ModOptionType::Choice?RECOMP_MOD_OPTION_CHOICE:RECOMP_MOD_OPTION_INTEGER;
        out->choice_count=(int)o.choices.size();out->min_value=o.min_value;out->max_value=o.max_value;out->step=o.step;
        out->disabled=!o.disabled_by.empty() && manager.feature_option_value(pkg,id,o.disabled_by)=="true";
        return 1;
    }
    return 0;
}
static int package_choice(void*,const char* pkg,const char* id,const char* option,int i,RecompLauncherCModChoice* out) {
    const auto* p=manager.selected_package(pkg);if (!p || i<0)return 0;
    for (const auto& o:p->options) if (o.feature_id==id && o.id==option && i<(int)o.choices.size()) {
        *out={};snprintf(out->value,sizeof out->value,"%s",o.choices[i].value.c_str());snprintf(out->label,sizeof out->label,"%s",o.choices[i].label.c_str());return 1;
    } return 0;
}
static int package_set(void*,const char* pkg,const char* id,const char* option,const char* value) {
    if (reject_edit) { provider_error="Fixture: package version changed before write";return 0; }
    return manager.set_feature_option(pkg,id,option,value,&provider_error) && manager.save_state(&provider_error);
}
static int package_enable(void*,const char* pkg,const char* id,int enabled) {
    return manager.set_feature_enabled(pkg,id,enabled!=0,&provider_error) && manager.save_state(&provider_error);
}
static const char* package_error(void*) { return provider_error.c_str(); }
static void reopen(const std::filesystem::path& root,const std::filesystem::path& settings) {
    manager.set_root(root);assert(manager.scan(&provider_error) && manager.load_state(&provider_error));
    catalog.clear();for (const auto& [id,versions]:manager.packages())
        for (const auto& f:manager.selected_package(id)->features) catalog.push_back({id,f.id});
    player_settings_configure(settings,player_settings_baseline(false,false,false,100,"cards"));
}
int main(int argc,char** argv) {
    assert(argc==3);const auto root=std::filesystem::u8path(argv[1]);const auto src=std::filesystem::u8path(argv[2]);
    std::filesystem::create_directories(root/"mods/bundled");
    // Test inputs are the exact shipped manifests, not hand-written copies.
    for (const auto& pkg:std::filesystem::directory_iterator(src/"mods/builtin/packages"))
        std::filesystem::copy(pkg.path(),root/"mods/bundled"/pkg.path().filename(),std::filesystem::copy_options::recursive|std::filesystem::copy_options::overwrite_existing);
    std::filesystem::create_directories(root/"mods/installed/fixture.custom/1.0.0");
    {std::ofstream f(root/"mods/installed/fixture.custom/1.0.0/manifest.toml");
     f<<"format_version=5\nid='fixture.custom'\nversion='1.0.0'\nname='Unrelated custom fixture'\nauthor='fixture'\nresolver='declarative'\n[[target]]\ngame_id='*'\n[[feature]]\nid='custom'\nname='Custom'\ndefault_enabled=true\n[[option]]\nfeature='custom'\nid='keep'\nlabel='Keep custom'\ntype='boolean'\ndefault='false'\n";}
    const auto settings=root/"settings.toml";
    { std::ofstream f(settings);f<<"[video]\nrenderer='opengl'\nadaptive_view=true\npgxp_tolerance=0.25\npgxp_depth_threshold=4321\n[audio]\nvolume=37\nbuffer_ms=123\n[controller]\nmouse_sensitivity=2.5\nanti_deadzone=789\n[memcard]\ndir='custom/cards'\n[parappa_timing]\nmode='stock'\nextra_early=2\nextra_late=3\n"; }
    reopen(root/"mods",settings);
    RecompLauncherCModProvider original{};original.feature_count=package_count;original.feature_get=package_feature;
    original.feature_option_get=package_option;original.feature_choice_get=package_choice;original.feature_set_option=package_set;
    original.feature_enable=package_enable;original.last_error=package_error;
    const auto* provider=player_settings_provider(&original);
    RecompLauncherCGameInfo game{};launcher_profile_apply("psx",&game);game.mods=provider;game.has_bios=1;
    game.name="MediEvil Settings";game.has_frame_interp=1;game.has_render_pipeline=1;game.aspect_mask=7;
    game.frame_generation_hidden=0;
    RecompLauncherCSettings settings_ui{};settings_ui.renderer=1;settings_ui.volume=37;
    settings_ui.supersampling=1;settings_ui.frame_interp=1;settings_ui.frame_generation=1;settings_ui.render_thread=1;
    settings_ui.present_thread=1;settings_ui.vsync=1;
    LauncherModel model{};launcher_model_init(&model,&settings_ui,&game,nullptr);
    model.rom_present=true;snprintf(model.rom_full,sizeof model.rom_full,"owned-fixture.bin");model.verify.verdict=1;
    snprintf(model.hotkeys[0],sizeof model.hotkeys[0],"F2");const auto before=model.s;
    assert(manager.set_feature_option("psx.enhancement.pgxp","pgxp","culling","true"));
    assert(manager.set_feature_option("psx.enhancement.pgxp","pgxp","cpu_mode","true"));
    assert(manager.set_feature_enabled("psx.enhancement.cd-speed","cd-speed",true));
    assert(manager.set_feature_option("fixture.custom","custom","keep","true"));
    assert(manager.save_state());
    // Compare persisted baselines: the manager materializes implicit defaults
    // on reload, independently of any preset operation.
    reopen(root/"mods",settings);
    const auto unrelated=manager.selections().at("psx.enhancement.cd-speed");
    const auto third_party=manager.selections().at("fixture.custom");
    assert(psx_preset_available(&model));
    for (bool best:{true,false,true}) {
        assert(psx_apply_preset(&model,best));assert(psx_current_preset(&model)==(best?1:0));
        auto prefs=PSXRecompV4::load_user_settings(settings);assert(!prefs.parse_error);
        for (const auto& item:kPsxPreset) if (!strcmp(item.package,"psx.runtime.settings"))
            assert(prefs.extra_player_settings.at(item.option)==(best?item.best:item.faithful));
        assert(prefs.extra_player_settings.at("audio.buffer_ms")=="123");
        assert(prefs.mouse_sensitivity==2.5 && prefs.memcard_dir=="custom/cards");
        assert(prefs.extra_player_settings.at("controller.anti_deadzone")=="789");
        assert(prefs.extra_player_settings.at("video.pgxp_tolerance")=="0.25");
        assert(prefs.extra_player_settings.at("video.pgxp_depth_threshold")=="4321");
        assert(prefs.parappa_timing_extra_early==2 && prefs.parappa_timing_extra_late==3);
        assert(model.s.volume==before.volume && model.s.render_thread==before.render_thread && !strcmp(model.hotkeys[0],"F2"));
        // Same merge the real launcher uses: fresh immediate provider fields
        // survive the launcher's older snapshot when Play/close saves the rest.
        PSXRecompV4::UserSettings snapshot;player_settings_merge(snapshot);
        snapshot.has_renderer=true;snapshot.renderer=model.s.renderer;snapshot.has_aspect_ratio=true;snapshot.aspect_num=4;snapshot.aspect_den=3;
        assert(PSXRecompV4::save_user_settings(settings,snapshot));
        reopen(root/"mods",settings);assert(psx_current_preset(&model)==(best?1:0));
        for (const auto& item:kPsxPreset) if (strcmp(item.package,"psx.runtime.settings"))
            assert(manager.feature_option_value(item.package,item.feature,item.option)==(best?item.best:item.faithful));
        assert(manager.feature_option_value("psx.enhancement.pgxp","pgxp","culling")=="true");
        assert(manager.feature_option_value("psx.enhancement.pgxp","pgxp","cpu_mode")=="true");
        assert(same_selection(manager.selections().at("psx.enhancement.cd-speed"),unrelated));
        assert(same_selection(manager.selections().at("fixture.custom"),third_party));
        bool intro=false;int audio=100;apply_extra_player_settings(PSXRecompV4::load_user_settings(settings),intro,audio);
        assert(g_video_geometry_correction==(int)best && g_video_perspective_texturing==(int)best);
        assert(g_video_post_aa==(best?2:0) && g_video_dithering==(best?2:1) && g_video_aniso==(best?16:1));
        PSXPgxpSessionInputs input{};input.env_geometry=input.env_texture=input.env_cpu_mode=input.env_culling=-1;
        input.player_geometry_set=input.player_texture_set=1;input.video_geometry=g_video_geometry_correction;
        input.video_texture=g_video_perspective_texturing;input.mod_enabled=manager.feature_enabled("psx.enhancement.pgxp","pgxp");
        input.mod_culling=1;const auto arm=psx_pgxp_session_resolve(&input);
        assert(arm.geometry==(int)best && arm.texture==(int)best && !arm.culling);
    }
    assert(package_set(nullptr,"psx.enhancement.image-filtering","image-filtering","texture_filtering","nearest"));
    assert(psx_current_preset(&model)==-1);assert(psx_apply_preset(&model,true));
    model.s.frame_interp=1;assert(psx_current_preset(&model)==-1);model.s.frame_interp=0;
    reject_edit=true;assert(!psx_apply_preset(&model,false));assert(psx_current_preset(&model)==1);reject_edit=false;
    { std::ofstream f(settings,std::ios::app);f<<"\n[broken\n"; }
    assert(!psx_apply_preset(&model,false)); // Existing provider rejects corrupt external edits.
    assert(!s_player_settings_error.empty());
    { std::ofstream f(settings);f<<"[audio]\nvolume=37\n"; }reopen(root/"mods",settings);assert(psx_apply_preset(&model,true));
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.DeltaTime=1.f/60;
    const auto theme=launcher_theme_psx();g_th=&theme;apply_theme_style(theme);
    const auto font=(std::filesystem::path(PSX_SETTINGS_UI_ROOT)/"assets/common/fonts/LatoLatin-Regular.ttf").u8string();
    io.Fonts->AddFontFromFileTTF(font.c_str(),theme.font_body);unsigned char* pixels;int w,h;io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    if (const char* folder=std::getenv("RUI_LAYOUT_CAPTURE_DIR")) render_matrix(model,theme,folder);
    ImGui::DestroyContext();
    puts("PASS: actual typed provider and manifest-backed ModPackageManager; exact Faithful/Best map persists/reloads, startup/PGXP consumers, custom/rejected writes, unrelated audio/input/cards/tuning preserved; real-schema offscreen layouts");
}
