"""Compose real provider/startup/reset/renderer glue without a window or GL context."""
from pathlib import Path
import argparse,subprocess,re
p=argparse.ArgumentParser()
p.add_argument('--compiler',required=True);p.add_argument('--ui',type=Path,required=True)
p.add_argument('--work',type=Path,required=True);p.add_argument('--source',type=Path)
p.add_argument('--compile-only',action='store_true')
p.add_argument('--expect-baseline-failure',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[2]
main=(a.source or root/'runtime/src/main.cpp').read_text(encoding='utf-8')
fixture=(root/'runtime/tests/test_player_settings_provider.cpp').read_text(encoding='utf-8')
def block(at):
 opening=main.index('{',at);end=opening+1;depth=1
 while depth:
  depth+=(main[end]=='{')-(main[end]=='}');end+=1
 return main[at:end]
def statement(prefix):
 at=main.index(prefix)
 opening=main.find('{',at);semi=main.find(';',at)
 return block(at) if opening<semi else main[at:semi+1]
reset=block(main.index('static void reset_mod_owned_presentation(void)'))
capabilities=block(main.index('static void psx_launcher_hide_mod_owned_rows('))
start=main.index('    /* Skip FMVs is mod-owned on PSX. Clamp stale generic settings')
clamps=main[start:main.index('    /* Latency knobs:',start)]
start=main.index('    if (const char *e = std::getenv("PSX_LOW_LATENCY_INPUT"))')
env_declaration=''
if 'auto apply_player_presentation_environment' in main:
 env_declaration=block(main.index('auto apply_player_presentation_environment'))+';'
 env='apply_player_presentation_environment();'
else:
 env=main[start:main.index('    /* Apply writable-state isolation',start)]
keys=['fov_scale','fast_boot','bios_hle','memcard_dir','mouse_sensitivity','mouse_capture',
      'low_latency_input','render_thread','present_thread','frame_generation','vsync',
      'frame_interpolation','frame_interpolation_fps','aspect_ratio']
apply='\n'.join(statement('if (us.has_'+key+')') for key in keys)
start=main.index('            g_frame_interpolation = ls.frame_interp ? 1 : 0;')
end=main.index('            apply_present_cadence();',start)
rematch=main[start:end]
renderer_names=('post_aa','bloom','post_shader','pgxp_depth','fmv_chroma_smoothing',
 'texture_lod','accurate_blending','dithering',
 'pgxp_color_perspective','pgxp_seam','pgxp_depth_threshold','frame_generation_method')
bindings=[]
for name in renderer_names:
 pattern=r'(?:\(void\))?gl_renderer_set_'+name+r'\(g_[^;]+;'
 found=re.search(pattern,main)
 assert found, 'Actual renderer binding missing: '+name
 bindings.append(found.group(0))
stubs=r'''
static bool g_offer_frame_interpolation=true;
#include "mod_session_baseline.h"
#include "pgxp_session.h"
#include "projection_scale.hpp"
static PSXModSessionBaseline g_mod_owned_baseline;
static int g_frame_interpolation=0,g_frame_interpolation_fps=0,g_video_vsync=1,g_auto_skip_fmv=0;
static int g_render_thread=1,g_present_thread=1,g_frame_generation=0,g_video_aspect_num=4,g_video_aspect_den=3;
static double g_guest_frame_period_ms=20,g_frame_period_ms=20;
static bool g_mod_native_vblank_rate=false,g_ws_adaptive_view=false,g_turbo_loads_enabled=false;
static int g_mod_native_vblank_fps,g_mod_texfilter,g_video_texfilter,g_ws_adaptive_max_num=16,g_ws_adaptive_max_den=9,g_ws_auto_ui_proportional_cfg;
static std::string g_bezel_path;
static float g_frame_interpolation_blend,g_frame_interpolation_blend_default;
static int g_frame_interpolation_source;
static constexpr int PSX_MOD_FRAME_SOURCE_VBLANK=0;
static constexpr bool skip_fmv_offered=false,frame_interpolation_offered=false,ws_offered=false,ws_ultrawide_offered=false;
static bool settings_chose_fast_boot=false,settings_chose_bios_hle=false,g_mouse_sensitivity_set=false,g_mouse_capture_set=false;
static int launcher_vsync_preference(){return g_mod_owned_baseline.captured?g_mod_owned_baseline.values.video_vsync:g_video_vsync;}
static int psx_vsync_from_launcher(int v,int){return v;}
static void psx_smooth_60fps_set(int){}
extern "C" void psx_pgxp_session_reset(void){}
static std::filesystem::path anchor_on_exe_dir(const char*,const std::filesystem::path& p){return p;}
static void set_environment(const char* key,const char* value){
#ifdef _WIN32
 _putenv_s(key,value);
#else
 if(*value)setenv(key,value,1);else unsetenv(key);
#endif
}
static std::map<std::string,std::string> renderer_arguments;
'''
for name in renderer_names:
 if name=='texture_lod':
  stubs+='static int gl_renderer_set_texture_lod(int value,int aniso){renderer_arguments["texture_lod"]=std::to_string(value);renderer_arguments["anisotropic_filtering"]=std::to_string(aniso);return 1;}\n'
 else:
  stubs+='template<class T> static int gl_renderer_set_'+name+'(T value){std::ostringstream text;text<<value;renderer_arguments["'+name+'"]=text.str();return 1;}\n'
for name in ('psx_mod_internal_resolution_reset','memcard_set_extension_codec','psx_local_mouse_clear',
 'psx_keybinds_clear_session_players','gr_set_texture_filter','psx_ram_reset_size_request',
 'debug_server_request','psx_mod_set_texture_bank_resolver','psx_mod_set_texture_bank_batching','gpu_ws_set_auto_ui_proportional',
 'psx_mod_set_world_scene_predicate','gpu_ws_set_native_scene_predicate','psx_mod_set_native_wide_projection_correction',
 'psx_mod_set_retained_scene_predicate','psx_mod_set_adaptive_backdrop_preload','psx_mod_set_draw_distance_clamp',
 'psx_mod_immediate_reset','psx_mod_set_guest_cycle_scale_gate','psx_host_enh_reset','render_pass_reset_session'):
 stubs+='template<class... T> static int '+name+'(T...){return 0;}\n'
stubs+=reset+'\n'+capabilities+'\n'
fixture=fixture.replace('int main() {',stubs+'\nint main() {')
start=fixture.index('    assert(intro && audio==64')
end=fixture.index('    PSXRecompV4::UserSettings old_snapshot;',start)
checks=fixture[start:end]
fixture=fixture[:start]+'    auto check_35=[&](){\n'+checks+'    }; check_35();\n'+fixture[end:]
insert=r'''
    assert(!g_frame_interpolation && !g_frame_interpolation_fps && g_video_aspect_num==4 && g_video_aspect_den==3);
    // The same persisted user settings now feed the actual main.cpp blocks.
    prefs.has_render_thread=prefs.has_present_thread=prefs.has_frame_generation=true;
    prefs.render_thread=false;prefs.present_thread=false;prefs.frame_generation=true;
    prefs.has_frame_interpolation=prefs.has_frame_interpolation_fps=true;
    prefs.frame_interpolation=true;prefs.frame_interpolation_fps=144;
    prefs.has_aspect_ratio=true;prefs.aspect_num=21;prefs.aspect_den=9;
    assert(PSXRecompV4::save_user_settings(path,prefs));
    auto us=PSXRecompV4::load_user_settings(path);
    bool fast_boot=false,bios_hle=false;std::filesystem::path memcard_dir;
    const char* argv[]={"probe.exe"};
'''+env_declaration+'\n'+apply+r'''
    assert(fast_boot && bios_hle);
    assert(g_fov_scale==1.5 && !g_low_latency_input && g_mouse_sensitivity==2.5 && g_mouse_capture_wanted);
    assert(memcard_dir=="cards/test");
'''+clamps+r'''
    assert(!g_render_thread && !g_present_thread && g_frame_generation);
    if (!g_frame_interpolation || g_frame_interpolation_fps!=144) {
        puts("FAIL: saved interpolation discarded before session start");return 1;
    }
    assert(g_video_aspect_num==21 && g_video_aspect_den==9);
    RecompLauncherCGameInfo offered{};
    psx_launcher_hide_mod_owned_rows(&offered);
    assert(offered.has_frame_interp && !offered.frame_generation_hidden);
    set_environment("PSX_FRAME_INTERPOLATION","0");
    set_environment("PSX_FRAME_INTERPOLATION_FPS","120");
    set_environment("PSX_LOW_LATENCY_INPUT","1");
'''+env+r'''
    assert(!g_frame_interpolation && g_frame_interpolation_fps==120 && g_low_latency_input);
    // Session capture, a temporary trusted-plugin choice, and rematch restore.
    reset_mod_owned_presentation();check_35();
    g_frame_interpolation=1;g_frame_interpolation_fps=240;g_video_vsync=0;
    reset_mod_owned_presentation();check_35();
    assert(!g_frame_interpolation && g_frame_interpolation_fps==120 && g_video_vsync==1);
    struct {int frame_interp=1,frame_interp_fps=165,vsync=1;} ls;
'''+rematch+r'''
    g_frame_interpolation=0;g_frame_interpolation_fps=0;
    reset_mod_owned_presentation();check_35();
    assert(g_mod_owned_baseline.values.frame_interpolation && g_mod_owned_baseline.values.frame_interpolation_fps==165);
    assert(!g_frame_interpolation && g_frame_interpolation_fps==120);
    set_environment("PSX_FRAME_INTERPOLATION","");set_environment("PSX_FRAME_INTERPOLATION_FPS","");set_environment("PSX_LOW_LATENCY_INPUT","");
    reset_mod_owned_presentation();check_35();
    assert(g_frame_interpolation && g_frame_interpolation_fps==165);
    assert(!g_render_thread && !g_present_thread && g_frame_generation && g_video_aspect_num==21 && g_video_aspect_den==9);
    PSXPgxpSessionInputs axes{};axes.env_geometry=axes.env_texture=axes.env_cpu_mode=axes.env_culling=-1;
    axes.player_geometry_set=g_player_geometry_set;axes.video_geometry=g_video_geometry_correction;
    axes.player_texture_set=g_player_texture_set;axes.video_texture=g_video_perspective_texturing;
    axes.mod_enabled=axes.mod_culling=1;
    auto armed=psx_pgxp_session_resolve(&axes);
    assert(armed.geometry && !armed.texture && armed.culling);
    axes.netplay=1;armed=psx_pgxp_session_resolve(&axes);
    assert(armed.geometry && !armed.texture && !armed.culling);
    axes.env_geometry=0;axes.env_texture=1;armed=psx_pgxp_session_resolve(&axes);
    assert(!armed.geometry && armed.texture && !armed.culling);
'''+ '\n'.join(bindings)+r'''
    assert(renderer_arguments.at("post_aa")=="2" && renderer_arguments.at("bloom")=="0.75");
    assert(renderer_arguments.at("post_shader")=="filters/test.pfx" && renderer_arguments.at("pgxp_depth")=="1");
    assert(renderer_arguments.at("fmv_chroma_smoothing")=="1" && renderer_arguments.at("texture_lod")=="1");
    assert(renderer_arguments.at("anisotropic_filtering")=="8" && renderer_arguments.at("accurate_blending")=="1");
    assert(renderer_arguments.at("dithering")=="2" && renderer_arguments.at("pgxp_color_perspective")=="1");
    assert(renderer_arguments.at("pgxp_seam")=="2" && renderer_arguments.at("pgxp_depth_threshold")=="2048.5");
    assert(renderer_arguments.at("frame_generation_method")=="1");
    puts("PASS:41 functional controls composed through real startup/clamps/env/session reset/rematch;PGXP plugin/netplay/env precedence;no renderer-output claim");
'''
if env_declaration:
 insert=insert.replace('reset_mod_owned_presentation();check_35();','reset_mod_owned_presentation();apply_player_presentation_environment();check_35();')
fixture=fixture.replace('    puts("PASS:35 actual provider edits',insert+'\n    puts("PASS:35 actual provider edits')
fixture=fixture.replace('#include "../src/', '#include "'+(root/'runtime/src').as_posix()+'/')
a.work.mkdir(parents=True,exist_ok=True);cpp=a.work/'composition.cpp';cpp.write_text(fixture,encoding='utf-8');exe=a.work/'composition.exe'
command=[a.compiler,'-std=c++17','-O0','-UNDEBUG','-DRECOMP_LAUNCHER','-DFMT_HEADER_ONLY=1']
command+=['-I'+str(root/part) for part in ('runtime/include','recompiler/src','recompiler/include','recompiler/lib/fmt/include','recompiler/lib/toml11')]
command+=['-I'+str(a.ui/'src'),str(cpp),str(root/'recompiler/src/config_loader.cpp'),str(root/'recompiler/src/ps1_exe_parser.cpp'),'-o',str(exe)]
result=subprocess.run(command,text=True,encoding='utf-8',errors='replace')
if result.returncode:raise SystemExit(result.returncode)
if a.compile_only:raise SystemExit(0)
result=subprocess.run([str(exe)],capture_output=True,text=True,encoding='utf-8',errors='replace')
print(result.stdout,end='');print(result.stderr,end='')
if a.expect_baseline_failure:
 assert result.returncode==1 and 'FAIL: saved interpolation discarded before session start' in result.stdout
 print('Measured expected baseline failure: saved interpolation clamp')
else:
 raise SystemExit(result.returncode)
