/* The existing provider's typed option interface also presents host settings.
 * These two rows are not installed packages or activation plugins: they are
 * edited only on Settings and persist to settings.toml. No launcher ABI moves. */
namespace {
using PlayerPreferences = PSXRecompV4::UserSettings;
using PlayerSpec = PSXRecompV4::PlayerSettingSpec;
using PlayerKind = PSXRecompV4::PlayerSettingKind;
const char kPlayerSettingsPackage[] = "psx.runtime.settings";
const PlayerSpec kExistingPlayerSettings[] = {
    {"video","fov_scale","Projection scale (0-8)","Display",PlayerKind::Number,0.000001,8,""},
    {"video","fast_boot","Skip BIOS boot animation","General",PlayerKind::Boolean,0,1,""},
    {"video","bios_hle","Use BIOS kernel HLE","General",PlayerKind::Boolean,0,1,""},
    {"video","low_latency_input","Resample input before present","General",PlayerKind::Boolean,0,1,""},
    {"memcard","dir","Default memory-card folder","General",PlayerKind::Text,0,0,""},
    {"controller","mouse_sensitivity","PS1 Mouse sensitivity (0.05-10)","General",PlayerKind::Number,0.05,10,""},
    {"controller","mouse_capture","Capture PS1 Mouse pointer","General",PlayerKind::Boolean,0,1,""},
};
std::filesystem::path s_player_settings_path;
std::map<std::string,std::string> s_player_setting_values;
std::string s_player_settings_error;
const RecompLauncherCModProvider* s_player_settings_original=nullptr;
RecompLauncherCModProvider s_player_settings_provider{};

template<class T> std::string player_setting_text(T value) {
    std::ostringstream text;
    text << std::setprecision(17) << value;
    return text.str();
}
std::string player_setting_flag(bool value) { return value?"true":"false"; }

template<class F> void each_player_setting(F callback) {
    for (const auto& spec:kExistingPlayerSettings) callback(spec);
    for (const auto& spec:PSXRecompV4::kExtraPlayerSettings) callback(spec);
}
const PlayerSpec* player_setting_at(const char* page,int index) {
    const PlayerSpec* result=nullptr;
    each_player_setting([&](const PlayerSpec& spec) {
        if (!std::strcmp(page,spec.page) && index--==0) result=&spec;
    });
    return result;
}
const PlayerSpec* player_setting_named(const char* id) {
    const PlayerSpec* result=nullptr;
    each_player_setting([&](const PlayerSpec& spec) {
        if (PSXRecompV4::player_setting_id(spec)==id) result=&spec;
    });
    return result;
}

bool player_setting_typed_get(const PlayerPreferences& prefs,const std::string& id,std::string& value) {
#define PLAYER_BOOL(key,field) if (id==key && prefs.has_##field) { value=player_setting_flag(prefs.field); return true; }
#define PLAYER_NUMBER(key,field) if (id==key && prefs.has_##field) { value=player_setting_text(prefs.field); return true; }
    PLAYER_BOOL("video.fast_boot",fast_boot)
    PLAYER_BOOL("video.bios_hle",bios_hle)
    PLAYER_BOOL("video.low_latency_input",low_latency_input)
    PLAYER_BOOL("controller.mouse_capture",mouse_capture)
    PLAYER_NUMBER("video.fov_scale",fov_scale)
    PLAYER_NUMBER("controller.mouse_sensitivity",mouse_sensitivity)
#undef PLAYER_BOOL
#undef PLAYER_NUMBER
    if (id=="memcard.dir" && prefs.has_memcard_dir) { value=prefs.memcard_dir.u8string(); return true; }
    auto found=prefs.extra_player_settings.find(id);
    if (found==prefs.extra_player_settings.end()) return false;
    value=found->second;
    return true;
}
void player_setting_typed_set(PlayerPreferences& prefs,const std::string& id,const std::string& value) {
#define PLAYER_BOOL(key,field) if (id==key) { prefs.field=value=="true"; prefs.has_##field=true; return; }
#define PLAYER_NUMBER(key,field) if (id==key) { prefs.field=std::strtod(value.c_str(),nullptr); prefs.has_##field=true; return; }
    PLAYER_BOOL("video.fast_boot",fast_boot)
    PLAYER_BOOL("video.bios_hle",bios_hle)
    PLAYER_BOOL("video.low_latency_input",low_latency_input)
    PLAYER_BOOL("controller.mouse_capture",mouse_capture)
    PLAYER_NUMBER("video.fov_scale",fov_scale)
    PLAYER_NUMBER("controller.mouse_sensitivity",mouse_sensitivity)
#undef PLAYER_BOOL
#undef PLAYER_NUMBER
    if (id=="memcard.dir") { prefs.memcard_dir=std::filesystem::u8path(value); prefs.has_memcard_dir=true; return; }
    prefs.extra_player_settings[id]=value;
}
void player_settings_merge(PlayerPreferences& seed) {
    /* A Settings edit is saved immediately. The launcher's older snapshot must
     * not erase it when Play or Quit persists the remaining controls. */
    const auto current=PSXRecompV4::load_user_settings(s_player_settings_path);
    seed.has_adaptive_view=current.has_adaptive_view; seed.adaptive_view=current.adaptive_view;
    seed.has_parappa_timing_mode=current.has_parappa_timing_mode; seed.parappa_timing_mode=current.parappa_timing_mode;
    seed.has_parappa_timing_extra_early=current.has_parappa_timing_extra_early; seed.parappa_timing_extra_early=current.parappa_timing_extra_early;
    seed.has_parappa_timing_extra_late=current.has_parappa_timing_extra_late; seed.parappa_timing_extra_late=current.parappa_timing_extra_late;
    each_player_setting([&](const PlayerSpec& spec) {
        const std::string id=PSXRecompV4::player_setting_id(spec);
        std::string value;
        if (player_setting_typed_get(current,id,value)) player_setting_typed_set(seed,id,value);
    });
}
void player_settings_configure(const std::filesystem::path& path,
                                std::map<std::string,std::string> baseline) {
    s_player_settings_path=path;
    s_player_setting_values=std::move(baseline);
    const auto prefs=PSXRecompV4::load_user_settings(path);
    each_player_setting([&](const PlayerSpec& spec) {
        const std::string id=PSXRecompV4::player_setting_id(spec);
        std::string value;
        if (player_setting_typed_get(prefs,id,value)) s_player_setting_values[id]=value;
    });
    s_player_settings_error.clear();
}

bool player_settings_own(const char* package) { return package && !std::strcmp(package,kPlayerSettingsPackage); }
const char* player_settings_page(const char* feature) {
    if (feature && !std::strcmp(feature,"general")) return "General";
    if (feature && !std::strcmp(feature,"display")) return "Display";
    return "";
}
int player_settings_count(void* context) {
    return s_player_settings_original->feature_count(context)+2;
}
int player_settings_feature(void* context,int index,RecompLauncherCModFeature* out) {
    const int base=s_player_settings_original->feature_count(context);
    if (index<base) return s_player_settings_original->feature_get(context,index,out);
    if (!out || index<base || index>=base+2) return 0;
    *out={};
    const char* page=index==base?"General":"Display";
    std::snprintf(out->package_id,sizeof out->package_id,"%s",kPlayerSettingsPackage);
    std::snprintf(out->id,sizeof out->id,"%s",index==base?"general":"display");
    std::snprintf(out->group,sizeof out->group,"%s",page);
    std::snprintf(out->name,sizeof out->name,"%s",index==base?"Startup and input preferences":"Advanced graphics");
    std::snprintf(out->status,sizeof out->status,"Saved immediately. Applies on the next game launch; command-line and environment overrides still win.");
    out->enabled=1;
    each_player_setting([&](const PlayerSpec& spec) { out->option_count+=!std::strcmp(page,spec.page); });
    return 1;
}
int player_settings_option(void* context,const char* package,const char* feature,int index,RecompLauncherCModOption* out) {
    if (!player_settings_own(package)) return s_player_settings_original->feature_option_get(context,package,feature,index,out);
    const auto* spec=player_setting_at(player_settings_page(feature),index);
    if (!spec || !out) return 0;
    *out={};
    const auto id=PSXRecompV4::player_setting_id(*spec);
    std::snprintf(out->id,sizeof out->id,"%s",id.c_str());
    std::snprintf(out->label,sizeof out->label,"%s",spec->label);
    std::snprintf(out->group,sizeof out->group,"%s",spec->page);
    std::snprintf(out->value,sizeof out->value,"%s",s_player_setting_values[id].c_str());
    std::snprintf(out->description,sizeof out->description,"Saved to [%s] %s. Applies next launch. Explicit command-line/environment overrides win.%s",spec->section,spec->key,
                  id=="video.fast_boot"?" Skips the boot animation only; kernel initialization and executable loading still run.":
                  id=="video.bios_hle"?" Computes supported BIOS kernel services in the host; this is separate from skipping the animation.":
                  id=="memcard.dir"?" Explicit per-card paths still take precedence.":
                  spec->kind==PlayerKind::ResolutionFloor?" Use native,720p,1080p,4k,display,1x-32x or a fixed height.":"");
    if (spec->kind==PlayerKind::Boolean) out->type=RECOMP_MOD_OPTION_BOOLEAN;
    else if (spec->kind==PlayerKind::Integer) {
        out->type=RECOMP_MOD_OPTION_INTEGER;
        out->min_value=(int64_t)spec->minimum; out->max_value=(int64_t)spec->maximum; out->step=1;
    } else if (spec->kind==PlayerKind::Choice) {
        out->type=RECOMP_MOD_OPTION_CHOICE;
        out->choice_count=1;
        for (const char* p=spec->choices;*p;++p) out->choice_count+=*p==',';
    } else out->type=RECOMP_MOD_OPTION_TEXT;
    return 1;
}
int player_settings_choice(void* context,const char* package,const char* feature,const char* option,int index,RecompLauncherCModChoice* out) {
    if (!player_settings_own(package)) return s_player_settings_original->feature_choice_get
        ? s_player_settings_original->feature_choice_get(context,package,feature,option,index,out):0;
    const auto* spec=player_setting_named(option);
    if (!spec || spec->kind!=PlayerKind::Choice || index<0 || !out) return 0;
    std::istringstream list(spec->choices);
    std::string value;
    while (std::getline(list,value,',')) if (index--==0) {
        *out={}; std::snprintf(out->value,sizeof out->value,"%s",value.c_str());
        std::snprintf(out->label,sizeof out->label,"%s",value.c_str()); return 1;
    }
    return 0;
}
int player_settings_set(void* context,const char* package,const char* feature,const char* option,const char* value) {
    s_player_settings_error.clear();
    if (!player_settings_own(package)) return s_player_settings_original->feature_set_option(context,package,feature,option,value);
    const auto* spec=option?player_setting_named(option):nullptr;
    if (!spec || !value || std::strcmp(spec->page,player_settings_page(feature)) || !PSXRecompV4::player_setting_valid(*spec,value)) {
        s_player_settings_error="That setting value is not valid."; return 0;
    }
    auto prefs=PSXRecompV4::load_user_settings(s_player_settings_path);
    if (prefs.parse_error) { s_player_settings_error="Settings file changed and is invalid. Fix it before saving."; return 0; }
    player_setting_typed_set(prefs,option,value);
    if (!PSXRecompV4::save_user_settings(s_player_settings_path,prefs)) {
        s_player_settings_error="Could not save settings.toml."; return 0;
    }
    s_player_setting_values[option]=value;
    return 1;
}
int player_settings_enable(void* context,const char* package,const char* feature,int enabled) {
    if (player_settings_own(package)) return enabled!=0;
    return s_player_settings_original->feature_enable(context,package,feature,enabled);
}
const char* player_settings_error(void* context) {
    if (!s_player_settings_error.empty()) return s_player_settings_error.c_str();
    return s_player_settings_original->last_error?s_player_settings_original->last_error(context):"";
}
const RecompLauncherCModProvider* player_settings_provider(const RecompLauncherCModProvider* original) {
    if (!original || !original->feature_count || !original->feature_get ||
        !original->feature_option_get || !original->feature_set_option || !original->feature_enable) return original;
    s_player_settings_original=original;
    s_player_settings_provider=*original;
    s_player_settings_provider.feature_count=player_settings_count;
    s_player_settings_provider.feature_get=player_settings_feature;
    s_player_settings_provider.feature_option_get=player_settings_option;
    s_player_settings_provider.feature_choice_get=player_settings_choice;
    s_player_settings_provider.feature_set_option=player_settings_set;
    s_player_settings_provider.feature_enable=player_settings_enable;
    s_player_settings_provider.last_error=player_settings_error;
    return &s_player_settings_provider;
}
}
