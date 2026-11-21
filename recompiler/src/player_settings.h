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
    /* Optional heading inside the page; null keeps the page name. Rows that
     * share a heading must be adjacent: the launcher draws them in this
     * order. */
    const char *heading;
};

inline constexpr const char kAutomaticResolution[] = "Automatic resolution";
inline constexpr const char kCrtFilter[] = "CRT filter";
inline constexpr const char kImageFiltering[] = "Image filtering";
inline constexpr const char kRewind[] = "Rewind";
/* Game default, then every fixed scale. 4x is used when no choice is saved. */
inline constexpr const char kInternalResolutionChoices[] =
    "default,1x,2x,3x,4x,5x,6x,7x,8x,9x,10x,11x,12x,13x,14x,15x,16x,"
    "17x,18x,19x,20x,21x,22x,23x,24x,25x,26x,27x,28x,29x,30x,31x,32x,"
    "native,720p,1080p,1440p,4k,5k,8k,display,auto";

/* Runtime-only player overrides. Build addresses, timing constants, feature
 * declarations and operational compiler/cache settings are not preferences. */
inline const PlayerSettingSpec kExtraPlayerSettings[] = {
    {"video","internal_resolution","Internal resolution","Display",PlayerSettingKind::Choice,0,0,kInternalResolutionChoices},
    {"video","antialiasing_mode","Post-process antialiasing","Display",PlayerSettingKind::Choice,0,0,"off,fxaa,fxaa_hq"},
    {"video","bloom","Bloom strength (0-2)","Display",PlayerSettingKind::Number,0,2,""},
    {"video","post_shader","Post-effect chain (.pfx)","Display",PlayerSettingKind::Text,0,0,""},
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
    {"video","crt_filter","Screen model","Display",PlayerSettingKind::Choice,0,0,"raw,crt,composite,trinitron",kCrtFilter},
    {"video","scanlines","Scanlines","Display",PlayerSettingKind::Boolean,0,1,"",kCrtFilter},
    {"video","scanline_strength","Scanline strength (0-1)","Display",PlayerSettingKind::Number,0,1,"",kCrtFilter},
    {"video","antialiasing","Smooth scaling","Display",PlayerSettingKind::Boolean,0,1,"",kImageFiltering},
    {"video","texture_filtering","Texture filtering","Display",PlayerSettingKind::Choice,0,0,"nearest,bilinear,stable,xbr",kImageFiltering},
    {"video","fmv_filter","Movie filtering","Display",PlayerSettingKind::Choice,0,0,"nearest,bilinear,sharp,bicubic",kImageFiltering},
    {"video","dynamic_resolution","Automatic resolution","Display",PlayerSettingKind::Boolean,0,1,"",kAutomaticResolution},
    {"video","dynamic_resolution_min","Lowest resolution (floor)","Display",PlayerSettingKind::ResolutionFloor,0,0,"",kAutomaticResolution},
    {"video","match_display_max_lines","Highest display lines (ceiling, 0=none)","Display",PlayerSettingKind::Integer,0,8640,"",kAutomaticResolution},
    {"video","supersample","Supersample multiplier (1-4)","Display",PlayerSettingKind::Number,1,4,"",kAutomaticResolution},
    {"video","supersample_disable_at_lines","Disable supersampling at display lines (0=never)","Display",PlayerSettingKind::Integer,0,8640,"",kAutomaticResolution},
    {"video","geometry_correction","PGXP vertex correction","PGXP",PlayerSettingKind::Boolean,0,1,""},
    {"video","perspective_texturing","Perspective-correct textures","PGXP",PlayerSettingKind::Boolean,0,1,""},
    {"video","pgxp_cpu_mode","PGXP CPU propagation","PGXP",PlayerSettingKind::Boolean,0,1,""},
    {"video","pgxp_culling","PGXP precise culling","PGXP",PlayerSettingKind::Boolean,0,1,""},
    {"runtime","bios_hle_keep_intro","Keep BIOS intro with kernel HLE","General",PlayerSettingKind::Boolean,0,1,""},
    {"audio","buffer_ms","Audio buffer (milliseconds)","General",PlayerSettingKind::Integer,30,500,""},
    {"controller","anti_deadzone","Analog anti-deadzone (0-32767)","General",PlayerSettingKind::Integer,0,32767,""},
    {"controller","experimental_jogcon_force_feedback_invert","Invert JogCon force feedback","General",PlayerSettingKind::Boolean,0,1,""},
    {"controller","select_shortcuts","Enable fallback Select shortcuts","General",PlayerSettingKind::Boolean,0,1,""},
    {"audio","spu_hq","Enhanced audio (high-quality mix)","General",PlayerSettingKind::Boolean,0,1,""},
    {"video","rewind","Rewind","General",PlayerSettingKind::Boolean,0,1,"",kRewind},
    {"video","rewind_depth","Rewind buffer (50, 100, 150 or 200)","General",PlayerSettingKind::Integer,50,200,"",kRewind},
    {"video","rewind_interval","Rewind capture interval (1, 4, 8, 12 or 15 frames)","General",PlayerSettingKind::Integer,1,15,"",kRewind},
};

inline const char* player_setting_heading(const PlayerSettingSpec& spec) {
    return spec.heading && *spec.heading ? spec.heading : spec.page;
}
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
    if (!std::strcmp(spec.key,"rewind_depth") && number!=50 && number!=100 && number!=150 && number!=200) return false;
    if (!std::strcmp(spec.key,"rewind_interval") && number!=1 && number!=4 && number!=8 && number!=12 && number!=15) return false;
    return true;
}
}
