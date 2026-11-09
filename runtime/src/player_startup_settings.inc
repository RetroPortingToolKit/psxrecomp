/* Shared by ordinary startup and launcher write-back. Only validated explicit
 * preferences override the title baseline; later CLI/environment paths retain priority. */
static int g_player_geometry_set=0, g_player_texture_set=0;
static int g_player_texture_window_batching=1;
static void apply_extra_player_settings(const PSXRecompV4::UserSettings& prefs,
                                      bool& keep_intro,int& audio_buffer) {
    for (const auto& spec:PSXRecompV4::kExtraPlayerSettings) {
        const auto id=PSXRecompV4::player_setting_id(spec);
        auto found=prefs.extra_player_settings.find(id);
        if (found==prefs.extra_player_settings.end() || !PSXRecompV4::player_setting_valid(spec,found->second)) continue;
        const auto& value=found->second;
        if (id=="video.antialiasing_mode") { g_video_post_aa=value=="off" ? 0 : value=="fxaa" ? 1 : 2; }
        if (id=="video.bloom") { g_video_bloom=static_cast<decltype(g_video_bloom)>(std::strtod(value.c_str(),nullptr)); }
        if (id=="video.post_shader") { g_video_post_shader=value; }
        if (id=="video.supersample") { g_video_ss_milli=static_cast<int>(std::strtod(value.c_str(),nullptr)*1000.0+0.5); g_title_ss_milli=g_video_ss_milli; }
        if (id=="video.pgxp_tolerance") { g_video_pgxp_tolerance=static_cast<decltype(g_video_pgxp_tolerance)>(std::strtod(value.c_str(),nullptr)); g_video_pgxp_tolerance_set=1; }
        if (id=="video.pgxp_position_fallback") { g_video_pgxp_position_fallback=value=="true"; g_video_pgxp_position_fallback_set=1; }
        if (id=="video.pgxp_preserve_projection") { g_video_pgxp_preserve_projection=value=="true"; }
        if (id=="video.pgxp_depth_buffer") { g_video_pgxp_depth_buffer=value=="true"; }
        if (id=="video.fmv_chroma_smoothing") { g_video_fmv_chroma=value=="true"; }
        if (id=="video.texture_lod") { g_video_texture_lod=value=="off" ? 0 : 1; }
        if (id=="video.anisotropic_filtering") { g_video_aniso=std::stoi(value); }
        if (id=="video.accurate_blending") { g_video_accurate_blending=value=="true"; }
        if (id=="video.dithering") { g_video_dithering=value=="off" ? 0 : value=="on" ? 1 : 2; }
        if (id=="video.pgxp_color_correction") { g_video_pgxp_color_correction=value=="true"; }
        if (id=="video.pgxp_seam") { g_video_pgxp_seam=value=="off" ? 0 : value=="fine" ? 1 : 2; }
        if (id=="video.pgxp_depth_threshold") { g_video_pgxp_depth_threshold=static_cast<decltype(g_video_pgxp_depth_threshold)>(std::strtod(value.c_str(),nullptr)); }
        if (id=="video.texture_window_batching") { g_player_texture_window_batching=value=="true"; gl_renderer_set_texture_window_batching(g_player_texture_window_batching); }
        if (id=="video.frame_generation_method") { g_frame_generation_method=value=="redraw" ? 0 : 1; }
        if (id=="video.dynamic_resolution_min") { psx_ir_parse(value.c_str(),&g_video_dynres_min); }
        if (id=="video.match_display_max_lines") { g_video_match_max_lines=std::stoi(value); }
        if (id=="video.supersample_disable_at_lines") { g_video_ss_disable_at_lines=std::stoi(value); }
        if (id=="video.geometry_correction") { g_video_geometry_correction=value=="true"; g_player_geometry_set=1; }
        if (id=="video.perspective_texturing") { g_video_perspective_texturing=value=="true"; g_player_texture_set=1; }
        if (id=="runtime.bios_hle_keep_intro") { keep_intro=value=="true"; }
        if (id=="audio.buffer_ms") { audio_buffer=std::stoi(value); }
        if (id=="controller.anti_deadzone") { controller_anti_deadzone=std::stoi(value); }
        if (id=="controller.experimental_jogcon_force_feedback_invert") { g_jogcon_ffb_invert=value=="true"; }
        if (id=="controller.select_shortcuts") { g_title_select_shortcuts=g_player_title_select_allowed && (value=="true"); }
    }
}
#if defined(RECOMP_LAUNCHER)
static std::map<std::string,std::string> player_settings_baseline(bool fast_boot,bool bios_hle,
        bool keep_intro,int audio_buffer,const std::filesystem::path& memcard_dir) {
    std::map<std::string,std::string> values;
    values["video.antialiasing_mode"]=g_video_post_aa==0 ? "off" : g_video_post_aa==1 ? "fxaa" : "fxaa_hq";
    values["video.bloom"]=player_setting_text(g_video_bloom);
    values["video.post_shader"]=g_video_post_shader;
    values["video.supersample"]=player_setting_text(g_video_ss_milli/1000.0);
    values["video.pgxp_tolerance"]=player_setting_text(g_video_pgxp_tolerance);
    values["video.pgxp_position_fallback"]=player_setting_flag(g_video_pgxp_position_fallback!=0);
    values["video.pgxp_preserve_projection"]=player_setting_flag(g_video_pgxp_preserve_projection!=0);
    values["video.pgxp_depth_buffer"]=player_setting_flag(g_video_pgxp_depth_buffer!=0);
    values["video.fmv_chroma_smoothing"]=player_setting_flag(g_video_fmv_chroma!=0);
    values["video.texture_lod"]=g_video_texture_lod==0 ? "off" : "mipmap";
    values["video.anisotropic_filtering"]=player_setting_text(g_video_aniso);
    values["video.accurate_blending"]=player_setting_flag(g_video_accurate_blending!=0);
    values["video.dithering"]=g_video_dithering==0 ? "off" : g_video_dithering==1 ? "on" : "fine";
    values["video.pgxp_color_correction"]=player_setting_flag(g_video_pgxp_color_correction!=0);
    values["video.pgxp_seam"]=g_video_pgxp_seam==0 ? "off" : g_video_pgxp_seam==1 ? "fine" : "wide";
    values["video.pgxp_depth_threshold"]=player_setting_text(g_video_pgxp_depth_threshold);
    values["video.texture_window_batching"]=player_setting_flag(g_player_texture_window_batching!=0);
    values["video.frame_generation_method"]=g_frame_generation_method==0 ? "redraw" : "reprojection";
    values["video.dynamic_resolution_min"]=player_setting_text(g_video_dynres_min);
    values["video.match_display_max_lines"]=player_setting_text(g_video_match_max_lines);
    values["video.supersample_disable_at_lines"]=player_setting_text(g_video_ss_disable_at_lines);
    values["video.geometry_correction"]=player_setting_flag(g_video_geometry_correction!=0);
    values["video.perspective_texturing"]=player_setting_flag(g_video_perspective_texturing!=0);
    values["runtime.bios_hle_keep_intro"]=player_setting_flag(keep_intro!=0);
    values["audio.buffer_ms"]=player_setting_text(audio_buffer);
    values["controller.anti_deadzone"]=player_setting_text(controller_anti_deadzone);
    values["controller.experimental_jogcon_force_feedback_invert"]=player_setting_flag(g_jogcon_ffb_invert!=0);
    values["controller.select_shortcuts"]=player_setting_flag(g_title_select_shortcuts!=0);
    if (const char* id=psx_ir_id_for(g_video_dynres_min)) values["video.dynamic_resolution_min"]=id;
    values["video.fast_boot"]=player_setting_flag(fast_boot);
    values["video.bios_hle"]=player_setting_flag(bios_hle);
    values["video.fov_scale"]=player_setting_text(g_fov_scale);
    values["video.low_latency_input"]=player_setting_flag(g_low_latency_input!=0);
    values["memcard.dir"]=memcard_dir.u8string();
    values["controller.mouse_sensitivity"]=player_setting_text(g_mouse_sensitivity);
    values["controller.mouse_capture"]=player_setting_flag(g_mouse_capture_wanted);
#if defined(PSX_PGXP) && PSX_PGXP
    if (!g_video_pgxp_tolerance_set) values["video.pgxp_tolerance"]="-1";
    if (!g_video_pgxp_position_fallback_set) values["video.pgxp_position_fallback"]="false";
#endif
    return values;
}
#endif
