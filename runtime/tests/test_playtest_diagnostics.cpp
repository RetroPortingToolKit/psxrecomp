/* Verify the VBlank recorder/replayer against a model of the engine's frame
 * loop: VBlank plugin, the guest frame (which reads the pad state latched in
 * SIO), then the boundary sample (physical pad, or a controller source).
 * Real alignment is checked separately by recording and replaying the actual
 * game. No window, disc or BIOS is needed. */
#include "host_osd.h"
#include "memcard.h"
#include "mod_netplay.h"
#include "mod_plugins.h"
#include "mod_runtime.h"
#include "savestate.h"
#include "sio.h"
#include "cdrom.h"
#include "render_pass.h"
#include "gpu.h"
#include "video_telemetry.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <cstdlib>
#include <memory>

static std::array<uint8_t,0x200000> memory;
static std::array<uint16_t,2> sio_buttons{0xFFFF,0xFFFF};
static std::array<std::array<uint8_t,4>,2> sio_sticks{};
static std::array<int,2> sio_analog{};
static std::array<int,2> sio_connected{1,0}, source_connected{1,1};
static std::array<PSXModControllerSource,2> sources{};
static std::map<std::string,unsigned> counters;
static std::string save_root,shown[2];
static std::string application_root;
static bool online;
static unsigned status_generation, status_pending;
static int status_ok=1,status_slot=-1;
static const char *status_op="save";
static std::vector<uint8_t> slot_image;
static std::string plan_text="fixture-plan";
static void (*activation)(),(*vblank)(),(*savestate)(),(*osd_activation)(),(*osd_vblank)(),(*session_reset)();
static void (*recording_activation)();
static uint64_t ram_reads;
static uint64_t ordinary_draws;
static PsxVideoConfigTelemetry video_config{PSX_VIDEO_OPENGL, 1, 4, 3, 0};
static std::vector<std::string> incident_images;

static void activate_recording_plan() {session_reset();recording_activation();}

uint32_t psx_mod_read_word(uint32_t a) {
    ++ram_reads;
    uint32_t v;memcpy(&v,&memory[a&0x1FFFFC],4);return v;
}
int psx_mod_register_activation_plugin(const char *id,PSXModActivationCallback cb) {
    if(!strcmp(id,"psxrecomp.diagnostics")) {recording_activation=cb;activation=activate_recording_plan;}
    else if(!strcmp(id,"psxrecomp.diagnostics.osd"))osd_activation=cb;
    return 1;
}
int psx_mod_register_session_reset_plugin(const char*,PSXModActivationCallback cb) {session_reset=cb;return 1;}
int psx_mod_register_vblank_plugin(const char *id,PSXModVBlankCallback cb) {
    if(!strcmp(id,"psxrecomp.diagnostics"))vblank=cb;
    else if(!strcmp(id,"psxrecomp.diagnostics.osd"))osd_vblank=cb;
    return 1;
}
int psx_mod_register_savestate_plugin(const char*,PSXModActivationCallback cb) {savestate=cb;return 1;}
int psx_mod_netplay_is_active(void) {return online;}
int psx_mod_set_controller_source(uint32_t seat,PSXModControllerSource source) {sources.at(seat)=source;source_connected.at(seat)=1;return 1;}
int psx_mod_set_controller_source_connected(uint32_t seat,int connected) {source_connected.at(seat)=connected;return 1;}
void psx_mod_counter_add(const char *name,uint32_t n) {counters[name]+=n;}
uint16_t sio_get_pad_buttons_slot(int slot) {return sio_buttons.at(slot);}
int sio_get_pad_analog(int slot) {return sio_analog.at(slot);}
int sio_get_pad_connected(int slot) {return sio_connected.at(slot);}
void sio_set_pad_connected(int slot,int connected) {sio_connected.at(slot)=connected;}
void sio_get_pad_sticks(int slot,uint8_t out[4]) {memcpy(out,sio_sticks.at(slot).data(),4);}
void sio_set_pad_state_slot(int slot,uint16_t buttons) {sio_buttons.at(slot)=buttons;}
void sio_set_pad_sticks(int slot,uint8_t lx,uint8_t ly,uint8_t rx,uint8_t ry) {sio_sticks.at(slot)={lx,ly,rx,ry};}
int memcard_is_present(int slot) {return slot==0;}
int memcard_export_raw(int,uint8_t *out) {memset(out,0xAB,MEMCARD_SIZE);return 0;}
void host_osd_set_diagnostics(const char *a,const char *b) {shown[0]=a?a:"";shown[1]=b?b:"";}
void host_osd_push(const char*,int) {}
void cdrom_debug_snapshot(CDROMDebugState *out) {*out={};}
int cdrom_data_read_active(void) {return 0;}
int cdrom_xa_stream_active(void) {return 0;}
void render_pass_get_stats(RenderPassStats *out) {*out={};}
int render_pass_verification_enabled(void) {return 0;}
void gpu_get_gp0_stats(uint64_t *nop, uint64_t *fill, uint64_t *draw,
                       uint64_t *environment, uint64_t *copy) {
    *nop = *fill = *environment = *copy = 0; *draw = ordinary_draws;
}
void psx_video_config_telemetry(PsxVideoConfigTelemetry *out) {*out=video_config;}
int psx_video_telemetry_capture(const char *path) {incident_images.emplace_back(path);return 1;}
void savestate_status_json(char *buf,size_t cap) {
    snprintf(buf,cap,"\"generation\":%u,\"pending\":%u,\"last_ok\":%d,\"last_op\":\"%s\",\"last_slot\":%d",
             status_generation,status_pending,status_ok,status_op,status_slot);
}
int savestate_read_slot(int,uint8_t **data,size_t *size) {
    if(slot_image.empty())return 0;
    *data=(uint8_t*)malloc(slot_image.size());memcpy(*data,slot_image.data(),slot_image.size());
    *size=slot_image.size();return 1;
}
namespace PSXRecompV4 {
std::string mod_runtime_plan_fingerprint_portable() {return plan_text;}
const std::string& mod_runtime_fingerprint() {static const std::string fp="fixture-plan";return fp;}
const std::filesystem::path& mod_runtime_root() {
    static std::filesystem::path root;
    root=std::filesystem::path(application_root)/"mods";return root;
}
}
namespace {std::filesystem::path application_directory() {return std::filesystem::u8path(application_root);}}
#define PSX_DIAGNOSTIC_TEST
#include "../src/playtest_diagnostics.cpp"

#define CHECK(x) do {if(!(x)) {fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}} while(0)
static void env(const char *name,const char *v) {
#ifdef _WIN32
    _putenv_s(name,v);
#else
    if(*v)setenv(name,v,1);else unsetenv(name);
#endif
}

// ---- Engine model -----------------------------------------------------------
struct Physical {uint16_t buttons[2];uint8_t sticks[2][4];int analog[2];int connected[2]={1,0};};
static Physical script(unsigned t) {
    Physical p{{0xFFFF,0xFFFF},{{128,128,128,128},{128,128,128,128}},{0,0}};
    if(t>=10 && t<40)p.buttons[0]=0xBFFF;                 // Cross held
    if(t%7==3)p.buttons[0]&=(uint16_t)~0x0010;            // Up taps
    if(t>=45 && t<50)p.buttons[0]&=(uint16_t)~0x0006;     // one held incident chord
    if(t>=60 && t<90) {p.analog[0]=1;p.sticks[0][0]=uint8_t(t*3);p.sticks[0][1]=uint8_t(255-t);}
    if(t>=100 && t<120)p.buttons[1]=0x7FFF;               // P2 Square
    p.connected[0] = !((t>=70 && t<80) || (t>=130 && t<145)); // analog + digital unplug/replug
    p.connected[1] = t>=95 && t<125;                     // P2 plug/unplug
    return p;
}
// Boundary sample: exactly the engine's rule. A source ANDs with the physical
// pad and supplies sticks/type; with no source the physical pad is used.
static void boundary_sample(const Physical& phys) {
    for(unsigned seat=0;seat<2;++seat) {
        uint16_t buttons=phys.buttons[seat];int analog=phys.analog[seat];
        int connected=phys.connected[seat];
        std::array<uint8_t,4> sticks;memcpy(sticks.data(),phys.sticks[seat],4);
        if(sources[seat]) {
            PSXModControllerState state{sizeof state,0xFFFF,128,128,128,128,1};
            if(!sources[seat](&state)) state={sizeof state,0xFFFF,128,128,128,128,1};
            buttons=(uint16_t)(state.buttons&buttons);analog=(int)state.analog;
            connected=source_connected[seat];
            sticks={uint8_t(state.lx),uint8_t(state.ly),uint8_t(state.rx),uint8_t(state.ry)};
            if(!analog)sticks={128,128,128,128};
        }
        sio_buttons[seat]=buttons;sio_analog[seat]=analog;sio_sticks[seat]=sticks;
        sio_connected[seat]=connected;
    }
}
// The guest frame reads the pad state SIO held at the VBlank and mutates RAM.
static void guest_frame(unsigned t) {
    uint32_t value;memcpy(&value,&memory[0x4000],4);
    value=value*1664525u+1013904223u+sio_buttons[0]+(uint32_t(sio_buttons[1])<<7)+
          sio_sticks[0][0]*31u+sio_sticks[0][1]*17u+t+
          sio_connected[0]*71u+sio_connected[1]*97u;
    memcpy(&memory[0x4000],&value,4);
    memcpy(&memory[0x100000+((t*64u)&0xFFFFF)],&value,4); // a spread-out write
}
// One engine frame: VBlank plugin, guest, boundary. `tamper` fires after the
// VBlank of that frame to model a diverging guest.
static void run_frames(unsigned from,unsigned to,int tamper=-1) {
    for(unsigned t=from;t<to;++t) {
        ordinary_draws += 17;
        if (t == 50) video_config = {PSX_VIDEO_OPENGL, 1080, 16, 9, 1};
        psx_video_telemetry_compose(PSX_VIDEO_OPENGL, t < 50 ? 1 : 5,
                                   t < 50 ? 320 : 426, 240,
                                   t < 50 ? 640 : 1920, t < 50 ? 480 : 1080,
                                   t < 50 ? 640 : 1920, t < 50 ? 480 : 1080);
        psx_video_telemetry_present(1);
        vblank();
        if(osd_enabled)osd_vblank();
        if((int)t==tamper)memory[0x80000]^=1;
        guest_frame(t);boundary_sample(script(t));
    }
}
static void reset_machine() {
    memory.fill(0);sio_buttons={0xFFFF,0xFFFF};sio_sticks={};sio_analog={};
    sio_connected={1,0};source_connected={1,1};
    for(auto& s:sio_sticks)s.fill(128);
}
static std::filesystem::path newest_session() {
    std::filesystem::path file;
    for(const auto& entry:std::filesystem::directory_iterator(std::filesystem::path(application_root)/"saves/diagnostics"))
        if(std::filesystem::exists(entry.path()/"inputs.csv") && (file.empty() || entry.path()>file))file=entry.path();
    return file;
}
static std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p,std::ios::binary);return std::string((std::istreambuf_iterator<char>(in)),{});
}

int main(int argc,char **argv) {
    CHECK(psx_playtest_diagnostic_application_directory()==
          std::filesystem::canonical(std::filesystem::u8path(argv[0])).parent_path());
    save_root=argc>1?argv[1]:"diagnostic-test-output";
    save_root+="/"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    application_root=save_root;
    std::filesystem::create_directories(PSXRecompV4::mod_runtime_root());
    {std::ofstream state(PSXRecompV4::mod_runtime_root()/"state.toml");state<<"format_version = 2\n";}
    {std::ofstream settings(std::filesystem::path(application_root)/"settings.toml");settings<<"[video]\n";}
    CHECK(activation && vblank && savestate && osd_activation && osd_vblank && session_reset);

    // Display-only plans invoke their own callback: accepted input changes
    // are visible without creating files, hashing RAM or taking pad ownership.
    reset_machine();session_reset();osd_activation();
    const auto display_reads=ram_reads;
    sio_buttons[0]=0xBFFF;osd_vblank();
    CHECK(shown[0].find(" X ")!=std::string::npos && shown[0].find("F0")!=std::string::npos);
    CHECK(shown[0].find("REC")==std::string::npos && !recording.is_open() && !telemetry.is_open());
    sio_buttons[0]=0xFFFF;sio_buttons[1]=0x7FFF;osd_vblank();
    CHECK(shown[0].find(" X ")==std::string::npos && shown[1].find(" [] ")!=std::string::npos);
    CHECK(shown[1].find("F1")!=std::string::npos && ram_reads==display_reads);
    CHECK(!sources[0] && !sources[1] && sequence==0);
    CHECK(!std::filesystem::exists(std::filesystem::path(application_root)/"saves/diagnostics"));
    // Reactivation starts at F0; online suspension clears stale text and
    // offline resumption does not count the suspended network frames.
    osd_activation();CHECK(shown[0].empty());osd_vblank();
    CHECK(shown[0].find("F0")!=std::string::npos);
    online=true;osd_vblank();CHECK(shown[0].empty() && shown[1].empty());
    online=false;osd_vblank();CHECK(shown[0].find("F1")!=std::string::npos);
    session_reset();CHECK(shown[0].empty() && shown[1].empty() && !osd_enabled);

    // ---- Record 200 frames of scripted two-seat input --------------------------
    env("PSX_DIAGNOSTIC_REPLAY","");env("PSX_DIAGNOSTIC_BASELINE","");
    // The two features may activate in either order after the engine reset.
    reset_machine();session_reset();osd_activation();recording_activation();
    CHECK(osd_enabled);
    CHECK(!sources[0] && !sources[1]);
    const auto recording_reads=ram_reads;
    run_frames(0,200);
    CHECK(ram_reads-recording_reads==200*DigestWords); // display never adds RAM sampling
    CHECK(shown[0].find("REC")!=std::string::npos && shown[0].find("F199")!=std::string::npos);
    recording.flush();
    telemetry.flush();
    auto final_memory=std::make_unique<decltype(memory)>(memory);
    recording.close();
    telemetry.close();
    auto session=newest_session();
    CHECK(!session.empty());
    auto text=slurp(session/"inputs.csv");
    CHECK(text.rfind("# psxrecomp-input-v2\n# execution ",0)==0 && text.find("\n# plan fixture-plan\n# build ")!=std::string::npos);
    size_t rows=0;for(size_t at=0;(at=text.find("\nN ",at))!=std::string::npos;++at)++rows;
    CHECK(rows==200);
    auto timings=slurp(session/"telemetry.csv");
    CHECK(timings.rfind("sequence,host_elapsed_us,host_interval_us,",0)==0);
    CHECK(std::count(timings.begin(),timings.end(),'\n')==201);
    CHECK(timings.find("gpu_draw_commands,host_presents,host_present_errors")!=std::string::npos);
    CHECK(timings.find("visual_verify_available,visual_verify_checks")!=std::string::npos);
    // Ordinary frames are observable with no optional render passes/checks.
    // Runtime changes must be sampled, rather than copied from saved settings.
    {std::istringstream csv(timings);std::string line;std::getline(csv,line);unsigned capture_requests=0;
     for(unsigned n=0;n<200;++n) {
         CHECK(bool(std::getline(csv,line)));std::replace(line.begin(),line.end(),',',' ');
         std::istringstream values(line);psx_playtest_diagnostic::TelemetryRow row{};
         for(auto& value:row)CHECK(bool(values>>value));
         std::string extra;CHECK(!(values>>extra));
         CHECK(row[9]==0 && row[11]==0 && row[12]==0);
         CHECK(row[13]>0 && row[14]>0 && row[16]==1);
         CHECK(row[18]==(n<50?1:1080) && row[19]==(n<50?4:16));
         CHECK(row[23]==(n<50?1:5) && row[26]==(n<50?640:1920));
         CHECK(row[30]==0 && row[31]==0 && row[32]==0 && row[33]==0);
         CHECK(row[34]==-1 || row[34]==1);capture_requests+=row[34]==1;
         CHECK(row[35]==1);
     }
     CHECK(capture_requests==1 && incident_images.size()==1);
     CHECK(std::filesystem::u8path(incident_images[0])==(session/"incident-1.png"));}
    CHECK(incident==1); // holding the chord across five frames marks once
    // Row 0 is the initial SIO state; row 11 shows Cross, which the model's
    // script first sampled at the boundary after frame 10... recorded one
    // VBlank later, exactly as the game sees it.
    CHECK(text.find("\nN 11,"+std::to_string(0xBFFF&0xFFEF)+",")!=std::string::npos ||
          text.find("\nN 11,"+std::to_string(0xBFFF)+",")!=std::string::npos);
    CHECK(std::filesystem::file_size(session/"card1.mcd")==MEMCARD_SIZE);
    CHECK(std::filesystem::exists(session/"card2.absent"));
    CHECK(std::filesystem::exists(session/"mods-state.toml") && std::filesystem::exists(session/"settings.toml"));
    CHECK(slurp(session/"session.json").find("\"netplay\": false")!=std::string::npos);
    CHECK(slurp(session/"session.json").find("psxrecomp-telemetry-v2")!=std::string::npos);
    CHECK(slurp(session/"session.json").find("baseline")==std::string::npos);
    {auto location=slurp(std::filesystem::path(save_root)/"diagnostics-location.txt");
     CHECK(location.find(psx_playtest_diagnostic_utf8(std::filesystem::absolute(session).make_preferred()))!=std::string::npos &&
           location.find("zip this entire session folder")!=std::string::npos);}

    // ---- Replay with idle physical pads: zero divergence ----------------------
    env("PSX_DIAGNOSTIC_REPLAY",(session/"inputs.csv").string().c_str());
    reset_machine();activation();
    CHECK(sources[0] && sources[1] && replaying && !replay_failed);
    // Physical input is idle during replay; the recorded script must come from
    // the sources alone.
    for(unsigned t=0;t<200;++t) {
        vblank();guest_frame(t);
        boundary_sample(Physical{{0xFFFF,0xFFFF},{{128,128,128,128},{128,128,128,128}},{0,0}});
        CHECK(!replay_failed);
    }
    CHECK(!replaying && !replay_failed && counters["psxrecomp.diagnostics.replay-complete"]==1);
    CHECK(counters["psxrecomp.diagnostics.replayed-inputs"]==200);
    CHECK(memory==*final_memory);
    CHECK(!sources[0] && !sources[1]);

    // ---- A diverging guest is reported at its first sequence ------------------
    reset_machine();activation();
    for(unsigned t=0;t<150 && !replay_failed;++t) {
        vblank();if(t==70)memory[0x4000]^=1;guest_frame(t);
        boundary_sample(Physical{{0xFFFF,0xFFFF},{{128,128,128,128},{128,128,128,128}},{0,0}});
    }
    // The digest samples a rotating 1/128th of RAM, so a corrupted word is
    // caught within one full rotation of the divergence.
    CHECK(replay_failed && !replaying && !sources[0] && sequence>70 && sequence<=71+StrideWords);

    // ---- Identity mismatches, torn tails and netplay --------------------------
    plan_text="different-plan";reset_machine();activation();
    CHECK(replay_failed && !sources[0]);
    plan_text="fixture-plan";
    online=true;reset_machine();activation();CHECK(replay_failed);online=false;
    {   // A hard-killed recording leaves a torn final row: replay verifies the rest.
        auto torn=std::filesystem::path(save_root)/"torn.csv";
        auto whole=slurp(session/"inputs.csv");
        std::ofstream out(torn,std::ios::binary);out<<whole.substr(0,whole.size()-9);out.close();
        env("PSX_DIAGNOSTIC_REPLAY",torn.string().c_str());
        reset_machine();activation();
        for(unsigned t=0;t<200 && replaying;++t) {
            vblank();guest_frame(t);
            boundary_sample(Physical{{0xFFFF,0xFFFF},{{128,128,128,128},{128,128,128,128}},{0,0}});
        }
        CHECK(!replaying && !replay_failed && counters["psxrecomp.diagnostics.replay-complete"]==2);
    }
    env("PSX_DIAGNOSTIC_REPLAY","");

    // A developer may apply old inputs across a fix, but this must never
    // increment the verified-replay counter or guess missing v1 presence.
#ifdef PSX_NO_DEBUG_TOOLS
    env("PSX_DIAGNOSTIC_INPUT_ONLY","1");
    reset_machine();activation();CHECK(replay_failed);
    env("PSX_DIAGNOSTIC_INPUT_ONLY","");
#else
    {
        auto legacy=std::filesystem::path(save_root)/"legacy-route.csv";
        std::istringstream in(slurp(session/"inputs.csv"));std::ofstream out(legacy);
        std::string line;unsigned row=0;
        while(std::getline(in,line)) {
            if(row++==0)line="# psxrecomp-input-v1";
            else if(line.rfind("N ",0)==0) {
                line.resize(line.rfind(','));line.resize(line.rfind(','));
            }
            out<<line<<'\n';
        }
        out.close();
        env("PSX_DIAGNOSTIC_REPLAY",legacy.string().c_str());
        reset_machine();activation();CHECK(replay_failed); // strict stays strict
        env("PSX_DIAGNOSTIC_INPUT_ONLY","1");
        reset_machine();activation();CHECK(replay_failed); // presence required
        env("PSX_DIAGNOSTIC_LEGACY_CONNECTED","10");
        const auto verified=counters["psxrecomp.diagnostics.replay-complete"];
        reset_machine();activation();CHECK(replaying && input_only && legacy_route);
        for(unsigned t=0;t<200;++t) {
            vblank();guest_frame(t);memory[0x4000]^=1; // deliberately different guest state
            boundary_sample(Physical{{0xFFFF,0xFFFF},{{128,128,128,128},{128,128,128,128}},{0,0}});
            CHECK(!replay_failed);
        }
        CHECK(!replaying && counters["psxrecomp.diagnostics.route-complete"]==1);
        CHECK(counters["psxrecomp.diagnostics.replay-complete"]==verified);
        env("PSX_DIAGNOSTIC_INPUT_ONLY","");
        env("PSX_DIAGNOSTIC_LEGACY_CONNECTED","");
        env("PSX_DIAGNOSTIC_REPLAY","");
    }
#endif

    // ---- Netplay: no rows, no overlay, no session ------------------------------
    {auto before=std::distance(std::filesystem::directory_iterator(std::filesystem::path(application_root)/"saves/diagnostics"),
                               std::filesystem::directory_iterator{});
     online=true;reset_machine();activation();osd_activation();
     for(unsigned t=0;t<5;++t) {vblank();osd_vblank();}
     CHECK(!recording.is_open() && shown[0].empty() && sequence==0);
     CHECK(std::distance(std::filesystem::directory_iterator(std::filesystem::path(application_root)/"saves/diagnostics"),
                         std::filesystem::directory_iterator{})==before);
     online=false;}

    // ---- Slot load starts a new baseline segment --------------------------------
    slot_image={0x50,0x53,0x54,0x0A,0xFE,0x01};
    reset_machine();activation();osd_activation();run_frames(0,30);
    status_generation=5;status_pending=0;status_ok=1;status_op="load";status_slot=3;
    auto first_segment=newest_session();
    memory[0x8000]=0x77;savestate();            // restore callback after the load
    CHECK(sequence==0 && !started && !recording.is_open());
    run_frames(30,80);recording.flush();
    auto segment=newest_session();
    CHECK(segment!=first_segment);
    CHECK(slurp(segment/"baseline.pst")==std::string((const char*)slot_image.data(),slot_image.size()));
    CHECK(slurp(segment/"session.json").find("\"baseline\": \"baseline.pst\"")!=std::string::npos);
    CHECK(counters["psxrecomp.diagnostics.state-segments"]==1);
    // A rewind restore (same status generation, no new slot load) cannot copy a slot.
    savestate();recording.flush();
    CHECK(counters["psxrecomp.diagnostics.state-segments"]==2);
    run_frames(80,85);recording.flush();
    auto rewound=newest_session();
    CHECK(rewound!=segment && !std::filesystem::exists(rewound/"baseline.pst"));
    CHECK(slurp(rewound/"session.json").find("\"baseline_unavailable\": true")!=std::string::npos);
    recording.close();

    // ---- Replay of the baseline segment -----------------------------------------
    {auto baseline_file=segment/"baseline.pst";
     env("PSX_DIAGNOSTIC_REPLAY",(segment/"inputs.csv").string().c_str());
     env("PSX_DIAGNOSTIC_BASELINE",baseline_file.string().c_str());
     reset_machine();activation();
     // Rows of the normal boot must not be consumed before the baseline loads.
     run_frames(0,5);
     CHECK(!started && !replaying && !replay_failed && sequence==0);
     status_generation=9;status_op="load";status_slot=0;memory[0x8000]=0x77;
     // The model's guest state at restore is whatever it was at recording; the
     // plugin only checks the loaded slot image and then verifies digests.
     // The recorded segment's first row was sampled mid-frame (P1 had Cross
     // held); a restored replay has no source sample before the first VBlank,
     // so the plugin must seed SIO with that row.
     sio_buttons={0xFFFF,0xFFFF};
     savestate();
     CHECK(baseline_loaded && replaying && !replay_failed);
     CHECK(sio_buttons[0]==pending[ColButtons] && sio_buttons[1]==pending[ColButtons+1]);
     CHECK(pending[ColButtons]==0xBFFF);
     // A mismatching restored image is refused.
     slot_image.back()^=1;reset_machine();activation();status_generation=11;savestate();
     CHECK(replay_failed && !replaying);
     slot_image.back()^=1;
     env("PSX_DIAGNOSTIC_BASELINE","");env("PSX_DIAGNOSTIC_REPLAY","");}

    // ---- An unwritable application directory uses a separate writable root ---
    online=false;reset_machine();activation();osd_activation();
    application_root+="/blocked-app";std::filesystem::create_directories(application_root);
    {std::ofstream blocked(std::filesystem::path(application_root)/"saves");blocked<<"file, not a directory";}
    vblank();recording.flush();
    CHECK(!recording_failed && recording.is_open());
    auto location=slurp(std::filesystem::path(application_root)/"diagnostics-location.txt");
    CHECK(location.find("Recording: ")!=std::string::npos);
    CHECK(location.find("psxrecomp-diagnostics")!=std::string::npos);
    // Play Again with Recording disabled must release the previous capture
    // and clear its REC label before a new display-only plan starts.
    session_reset();
    CHECK(!recording.is_open() && !telemetry.is_open() && shown[0].empty());
    const auto restarted_reads=ram_reads;
    osd_activation();osd_vblank();
    CHECK(shown[0].find("F0")!=std::string::npos && shown[0].find("REC")==std::string::npos);
    CHECK(ram_reads==restarted_reads && !recording.is_open() && !sources[0]);
    session_reset();
    printf("diagnostic recording/replay boundary checks passed\n");return 0;
}
