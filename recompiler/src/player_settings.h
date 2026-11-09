#pragma once
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include "../../runtime/include/internal_resolution.h"

namespace PSXRecompV4 {
enum class PlayerSettingKind { Boolean, Integer, Number, Text, Choice, ResolutionFloor };
struct PlayerSettingSpec {
    const char *section, *key, *label, *page;
    PlayerSettingKind kind;
    double minimum, maximum;
    const char *choices;
};

/* Runtime-only player overrides. Build addresses, timing constants, feature
 * declarations and operational compiler/cache settings are not preferences. */
inline const PlayerSettingSpec kExtraPlayerSettings[] = {
    {"video","antialiasing_mode","Post-process antialiasing","Display",PlayerSettingKind::Choice,0,0,"off,fxaa,fxaa_hq"},
    {"video","bloom","Bloom strength (0-2)","Display",PlayerSettingKind::Number,0,2,""},
    {"video","post_shader","Post-effect chain (.pfx)","Display",PlayerSettingKind::Text,0,0,""},
    {"video","supersample","Supersample multiplier (1-4)","Display",PlayerSettingKind::Number,1,4,""},
    {"video","pgxp_tolerance","PGXP tolerance (-1 disables clamp)","Display",PlayerSettingKind::Number,-1,1e12,""},
    {"video","pgxp_position_fallback","PGXP position fallback","Display",PlayerSettingKind::Boolean,0,1,""},
    {"video","pgxp_preserve_projection","Preserve PGXP projection","Display",PlayerSettingKind::Boolean,0,1,""},
    {"video","pgxp_depth_buffer","PGXP depth buffer","Display",PlayerSettingKind::Boolean,0,1,""},
    {"video","fmv_chroma_smoothing","FMV chroma smoothing","Display",PlayerSettingKind::Boolean,0,1,""},
    {"video","texture_lod","Texture mipmaps","Display",PlayerSettingKind::Choice,0,0,"off,mipmap"},
    {"video","anisotropic_filtering","Anisotropic filtering (1-16)","Display",PlayerSettingKind::Integer,1,16,""},
    {"video","accurate_blending","Accurate blending","Display",PlayerSettingKind::Boolean,0,1,""},
    {"video","dithering","Dithering","Display",PlayerSettingKind::Choice,0,0,"off,on,fine"},
    {"video","pgxp_color_correction","PGXP color correction","Display",PlayerSettingKind::Boolean,0,1,""},
    {"video","pgxp_seam","PGXP seam correction","Display",PlayerSettingKind::Choice,0,0,"off,fine,wide"},
    {"video","pgxp_depth_threshold","PGXP depth threshold","Display",PlayerSettingKind::Number,0,1e12,""},
    {"video","texture_window_batching","Texture-window batching","Display",PlayerSettingKind::Boolean,0,1,""},
    {"video","frame_generation_method","Smooth-motion method","Display",PlayerSettingKind::Choice,0,0,"redraw,reprojection"},
    {"video","dynamic_resolution_min","Automatic-resolution floor","Display",PlayerSettingKind::ResolutionFloor,0,0,""},
    {"video","match_display_max_lines","Display-match ceiling (0=none)","Display",PlayerSettingKind::Integer,0,8640,""},
    {"video","supersample_disable_at_lines","Disable supersampling at height (0=never)","Display",PlayerSettingKind::Integer,0,8640,""},
    {"video","geometry_correction","PGXP vertex correction","Display",PlayerSettingKind::Boolean,0,1,""},
    {"video","perspective_texturing","Perspective-correct textures","Display",PlayerSettingKind::Boolean,0,1,""},
    {"runtime","bios_hle_keep_intro","Keep BIOS intro with kernel HLE","General",PlayerSettingKind::Boolean,0,1,""},
    {"audio","buffer_ms","Audio buffer (milliseconds)","General",PlayerSettingKind::Integer,30,500,""},
    {"controller","anti_deadzone","Analog anti-deadzone (0-32767)","General",PlayerSettingKind::Integer,0,32767,""},
    {"controller","experimental_jogcon_force_feedback_invert","Invert JogCon force feedback","General",PlayerSettingKind::Boolean,0,1,""},
    {"controller","select_shortcuts","Enable fallback Select shortcuts","General",PlayerSettingKind::Boolean,0,1,""},
};

inline std::string player_setting_id(const PlayerSettingSpec& spec) {
    return std::string(spec.section)+"."+spec.key;
}
inline bool player_setting_valid(const PlayerSettingSpec& spec, const std::string& value) {
    if (value.size()>=256 || value.find_first_of("\r\n")!=std::string::npos) return false;
    if (spec.kind==PlayerSettingKind::Boolean) return value=="true" || value=="false";
    if (spec.kind==PlayerSettingKind::Text) return true;
    if (spec.kind==PlayerSettingKind::Choice) {
        const std::string list=std::string(",")+spec.choices+",";
        return !value.empty() && list.find(","+value+",")!=std::string::npos;
    }
    if (spec.kind==PlayerSettingKind::ResolutionFloor) {
        int parsed=0;
        return psx_ir_parse(value.c_str(),&parsed) && !psx_ir_is_auto(parsed);
    }
    errno=0; char* end=nullptr;
    const double number=std::strtod(value.c_str(),&end);
    if (errno || end==value.c_str() || *end || !std::isfinite(number) ||
        number<spec.minimum || number>spec.maximum) return false;
    if (spec.kind==PlayerSettingKind::Integer && std::floor(number)!=number) return false;
    if ((!std::strcmp(spec.key,"match_display_max_lines") || !std::strcmp(spec.key,"supersample_disable_at_lines")) &&
        number!=0 && number<240) return false;
    return true;
}
}
