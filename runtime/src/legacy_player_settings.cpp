#include "legacy_player_settings.h"
#include "mod_packages.h"
#include "player_settings.h"
#include "toml.hpp"
#include <chrono>
#include <fstream>
#include <stdexcept>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace PSXRecompV4 {
bool preserve_legacy_player_settings(const std::filesystem::path& settings,
                                     std::string* error) {
    namespace fs = std::filesystem;
    fs::path temp;
    try {
        toml::value doc(toml::table{});
        if (fs::exists(settings)) doc=toml::parse(settings.string());
        if (doc.contains("legacy_builtin_settings_migrated") &&
            doc.at("legacy_builtin_settings_migrated").is_boolean() &&
            toml::find<bool>(doc,"legacy_builtin_settings_migrated")) return true;
        ModPackageManager saved(settings.parent_path()/"mods");
        if (!saved.load_state(error)) return false;
        if (saved.selections().empty()) return true;
        bool changed=false,seen_legacy=false;
        auto put=[&](const char* id,const std::string& value) {
            for (const auto& spec:kExtraPlayerSettings) {
                if (player_setting_id(spec)!=id || !player_setting_valid(spec,value)) continue;
                if (doc.contains(spec.section) && !doc.at(spec.section).is_table()) return;
                if (!doc.contains(spec.section)) doc[spec.section]=toml::table{};
                auto& table=doc[spec.section];
                if (table.contains(spec.key)) return; // Even a hand-written new key owns its choice.
                if (spec.kind==PlayerSettingKind::Boolean) table[spec.key]=value=="true";
                else if (spec.kind==PlayerSettingKind::Integer) table[spec.key]=std::stoll(value);
                else if (spec.kind==PlayerSettingKind::Number) table[spec.key]=std::stod(value);
                else table[spec.key]=value;
                changed=true;
                return;
            }
        };
        std::map<std::string,ModFeatureSelection> package_choices;
        auto feature=[&](const char* package,const char* id)->const ModFeatureSelection* {
            const auto p=saved.selections().find(package);
            if (p==saved.selections().end()) return nullptr;
            const auto f=p->second.features.find(id);
            if (f!=p->second.features.end()) { seen_legacy=true;return &f->second; }
            // Pre-feature state stored the active choice on the package.
            // A version-only modern package record is not an explicit choice.
            if (p->second.features.empty() && (p->second.enabled || !p->second.values.empty())) {
                auto& old=package_choices[package];
                old.enabled=p->second.enabled;old.has_enabled=true;old.values=p->second.values;
                seen_legacy=true;
                return &old;
            }
            return nullptr;
        };
        auto value=[](const ModFeatureSelection& f,const char* id,const char* fallback="default") {
            const auto v=f.values.find(id);
            return v==f.values.end() ? std::string(fallback) : v->second;
        };
        auto active=[](const ModFeatureSelection* f) { return f && f->has_enabled && f->enabled; };
        auto choice=[&](const ModFeatureSelection& f,const char* old,const char* key) {
            put(key,value(f,old)); // "default" is intentionally absent for settings without it.
        };
        auto toggle=[&](const ModFeatureSelection& f,const char* old,const char* key) {
            const auto v=value(f,old);
            if (v=="on" || v=="off") put(key,v=="on" ? "true" : "false");
        };
        const auto* resolution=feature("psx.enhancement.internal-resolution","internal-resolution");
        if (resolution && resolution->has_enabled) {
            // The old resolver made Off native, dynamic off, supersample 1.
            // Only an enabled option at "default" followed the title baseline.
            put("video.internal_resolution",resolution->enabled ? value(*resolution,"resolution","4x") : "1x");
            if (resolution->enabled) {
                choice(*resolution,"minimum","video.dynamic_resolution_min");
                choice(*resolution,"supersample","video.supersample");
                toggle(*resolution,"dynamic_resolution","video.dynamic_resolution");
            } else {
                put("video.dynamic_resolution","false");
                put("video.supersample","1");
            }
        }
        const auto* crt=feature("psx.enhancement.crt-filter","crt-filter");
        if (active(crt)) {
            choice(*crt,"screen_model","video.crt_filter");
            toggle(*crt,"scanlines","video.scanlines");
            const auto strength=value(*crt,"scanline_strength","50");
            char* end=nullptr;const long n=std::strtol(strength.c_str(),&end,10);
            if (end!=strength.c_str() && !*end && n>=1 && n<=100)
                put("video.scanline_strength",std::to_string(n/100.0));
        }
        const auto* image=feature("psx.enhancement.image-filtering","image-filtering");
        if (active(image)) {
            const auto smoothing=value(*image,"smoothing");
            if (smoothing=="smooth" || smoothing=="sharp") put("video.antialiasing",smoothing=="smooth" ? "true" : "false");
            choice(*image,"texture_filtering","video.texture_filtering");
            choice(*image,"fmv_filtering","video.fmv_filter");
            const auto dither=value(*image,"dithering");
            put("video.dithering",dither=="native" ? "on" : dither);
        }
        if (active(feature("psx.enhancement.enhanced-audio","enhanced-audio"))) put("audio.spu_hq","true");
        const auto* pgxp=feature("psx.enhancement.pgxp","pgxp");
        if (active(pgxp)) {
            put("video.geometry_correction",value(*pgxp,"geometry","true"));
            put("video.perspective_texturing",value(*pgxp,"perspective","true"));
            put("video.pgxp_cpu_mode",value(*pgxp,"cpu_mode","false"));
            put("video.pgxp_culling",value(*pgxp,"culling","false"));
        }
        const auto* rewind=feature("psx.feature.rewind","rewind");
        if (rewind && rewind->has_enabled) {
            put("video.rewind",rewind->enabled ? "true" : "false");
            if (rewind->enabled) {
                put("video.rewind_depth",value(*rewind,"buffer","50"));
                put("video.rewind_interval",value(*rewind,"interval","15"));
            }
        }
        const auto* post=feature("psx.enhancement.post-processing","post-processing");
        if (active(post)) {
            choice(*post,"antialiasing","video.antialiasing_mode");
            const auto bloom=value(*post,"bloom");
            if (bloom=="off") put("video.bloom","0");
            if (bloom=="on") {
                const auto strength=value(*post,"bloom_strength","25");
                char* end=nullptr;const long n=std::strtol(strength.c_str(),&end,10);
                if (end!=strength.c_str() && !*end && n>=1 && n<=200)
                    put("video.bloom",std::to_string(n/100.0));
            }
            const auto shader=value(*post,"shader");
            if (shader=="off") put("video.post_shader","");
            if (shader=="on") {
                const auto resource=post->resources.find("shader");
                if (resource!=post->resources.end()) {
                    // The old resource resolver returned the saved path verbatim.
                    put("video.post_shader",resource->second);
                }
            }
        }
        if (!changed && !seen_legacy) return true;
        doc["legacy_builtin_settings_migrated"]=true;
        temp=settings;
        temp+=".legacy-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".tmp";
        std::ofstream out(temp,std::ios::binary);
        out<<doc;out.close();
        if (!out) throw std::runtime_error("cannot write preserved player settings");
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(),settings.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot publish preserved player settings (Windows error "+std::to_string(GetLastError())+")");
#else
        fs::rename(temp,settings);
#endif
        return true;
    } catch (const std::exception& e) {
        if (!temp.empty()) { std::error_code ignored;fs::remove(temp,ignored); }
        if (error) *error=e.what();
        return false;
    }
}
}
