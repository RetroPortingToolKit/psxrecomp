// [widescreen.cull] bgez_sites and clip_edge_x_load_sites: config parsing,
// overlay-cache identity, emitted native code, main-EXE opcode guards (also
// for bltz_sites and branch_keep_sites), and the shared runtime math
// (ws_cull_edge.h) the emitted helpers call.
#include "code_generator.h"
#include "config_loader.h"
#include "control_flow.h"
#include "../../runtime/include/ws_cull_edge.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <cstdlib>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kBase = 0x80010000u;
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

fs::path write_temp_config(const char* stem, const std::string& body) {
    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    (std::string(stem) + "-" + std::to_string(nonce) + ".toml");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    return path;
}

std::string base_config() {
    return R"toml([game]
name = "Cull Edge Test"
id = "TEST-00000"
exe = "TEST.EXE"
load_address = "0x80010000"
entry_pc = "0x80010000"
text_size = "0x1000"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"

[widescreen.cull]
guard_pixels = 0
slti_sites = ["0x80010020"]
bltz_sites = ["0x80010018"]
branch_keep_sites = ["0x80010040"]
)toml";
}

PSXRecompV4::GameConfig load(const std::string& body) {
    fs::path path = write_temp_config("ws-cull-edge", body);
    auto config = PSXRecompV4::load_game_config(path);
    fs::remove(path);
    return config;
}

bool load_throws_with(const std::string& body, const char* needle) {
    fs::path path = write_temp_config("ws-cull-edge-bad", body);
    bool matched = false;
    try {
        (void)PSXRecompV4::load_game_config(path);
    } catch (const std::exception& e) {
        matched = std::string(e.what()).find(needle) != std::string::npos;
    }
    fs::remove(path);
    return matched;
}

// A tiny function: <word> ; nop ; addiu v1,zero,1 ; jr ra ; nop. A branch in
// the first slot targets the jr (kBase+12).
PSXRecomp::GeneratedFunction generate_first_instruction(
    uint32_t first_word, const PSXRecomp::CodeGenConfig& config) {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    exe.header.initial_pc = kBase;
    exe.header.file_size = 20;
    append_word(exe.code_data, first_word);
    append_word(exe.code_data, 0x00000000u);
    append_word(exe.code_data, 0x24030001u);
    append_word(exe.code_data, 0x03E00008u);
    append_word(exe.code_data, 0x00000000u);

    PSXRecomp::Function function{};
    function.start_addr = kBase;
    function.end_addr = kBase + 20u;
    function.size = 20u;
    function.name = "ws_cull_edge_test";

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg);
}

// Runs `body` in a child and reports whether it exited with a failure status.
// The main-EXE guards call std::exit(1), which a plain call cannot observe.
// Windows has no fork: the Nth call re-runs this binary as `--exit-probe N`,
// which repeats the tests but runs only the Nth body (every other call returns
// false unrun) and exits 0 when that body returns.
const char* g_self = nullptr;
int g_exit_probe = -1;
int g_probe_seq = 0;

template <class F>
bool exits_with_failure(F&& body) {
#if defined(_WIN32)
    const int seq = g_probe_seq++;
    if (g_exit_probe >= 0) {
        if (seq != g_exit_probe) return false;
        body();
        std::fflush(nullptr);
        std::_Exit(0);
    }
    std::fflush(nullptr);
    const std::string quoted = std::string("\"") + g_self + "\"";
    const std::string n = std::to_string(seq);
    const intptr_t rc = _spawnl(_P_WAIT, g_self, quoted.c_str(), "--exit-probe",
                                n.c_str(), nullptr);
    return rc > 0;
#else
    (void)g_probe_seq;
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) {
        if (!std::freopen("/dev/null", "w", stderr)) _exit(3);
        body();
        _exit(0);
    }
    int status = 0;
    if (pid < 0 || waitpid(pid, &status, 0) != pid) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) != 0;
#endif
}

constexpr uint32_t kBgezV0 = 0x04410002u;    // bgez  v0, +2
constexpr uint32_t kBltzV0 = 0x04400002u;    // bltz  v0, +2
constexpr uint32_t kLhV0 = 0x84820070u;      // lh    v0, 0x70(a0)
constexpr uint32_t kLhuV0 = 0x94820070u;     // lhu   v0, 0x70(a0)
constexpr uint32_t kLwV0 = 0x8C820070u;      // lw    v0, 0x70(a0)
constexpr uint32_t kAddiuV0 = 0x24820070u;   // addiu v0, a0, 0x70
constexpr uint32_t kLhZero = 0x84800070u;    // lh    zero, 0x70(a0)
constexpr uint32_t kBltzalV0 = 0x04500002u;  // bltzal v0, +2
constexpr uint32_t kBeqV0 = 0x10400002u;     // beq   v0, zero, +2
constexpr uint32_t kJumpJr = 0x08004003u;    // j     kBase+12 (the jr)

void loader_parses_new_kinds() {
    auto config = load(base_config() + R"toml(bgez_sites = ["0x80010010", "0x80010014"]
clip_edge_x_load_sites = ["0x80010030"]
clip_edge_width = 368
)toml");
    check(config.ws_cull_bgez_sites ==
              std::vector<uint32_t>({0x80010010u, 0x80010014u}),
          "loader reads bgez_sites");
    check(config.ws_cull_clip_edge_x_load_sites ==
              std::vector<uint32_t>({0x80010030u}),
          "loader reads clip_edge_x_load_sites");
    check(PSXRecompV4::ws_cull_clip_edge_width(config) == 368u,
          "explicit clip_edge_width wins");

    auto defaults = load(base_config());
    check(defaults.ws_cull_bgez_sites.empty() &&
              defaults.ws_cull_clip_edge_x_load_sites.empty(),
          "new kinds default to empty");
    check(PSXRecompV4::ws_cull_clip_edge_width(defaults) == 0x140u,
          "clip_edge_width defaults to screen_w_imms[0]");

    auto ape = load(base_config() + "screen_w_imms = [\"0x181\"]\n");
    check(PSXRecompV4::ws_cull_clip_edge_width(ape) == 0x181u,
          "clip_edge_width follows a per-game screen_w_imms");

    check(load_throws_with(base_config() + "clip_edge_width = 0\n",
                           "clip_edge_width must be 1..1024"),
          "loader rejects a zero clip_edge_width");
}

void hash_identity_only_changes_when_used() {
    const std::string body = base_config();
    const auto plain = load(body);
    const auto explicit_empty = load(body + R"toml(bgez_sites = []
clip_edge_x_load_sites = []
clip_edge_width = 320
)toml");
    const uint32_t plain_hash = PSXRecompV4::overlay_codegen_config_hash(plain);
    check(plain_hash == PSXRecompV4::overlay_codegen_config_hash(explicit_empty),
          "empty new lists (and an unused width) leave the overlay hash unchanged");

    // Pinned against the hash layout before these kinds existed, computed
    // from this exact config with the previous config_loader.cpp. A change
    // here invalidates every title's overlay cache; do it deliberately.
    check(plain_hash == 0xE3B85230u,
          "hash of a config without the new kinds matches the pre-change layout");

    const auto with_bgez = load(body + "bgez_sites = [\"0x80010010\"]\n");
    check(plain_hash != PSXRecompV4::overlay_codegen_config_hash(with_bgez),
          "bgez_sites enter the overlay hash when non-empty");
    const auto with_clip = load(body + "clip_edge_x_load_sites = [\"0x80010030\"]\n");
    const uint32_t clip_hash = PSXRecompV4::overlay_codegen_config_hash(with_clip);
    check(plain_hash != clip_hash,
          "clip_edge_x_load_sites enter the overlay hash when non-empty");
    const auto with_clip_w = load(body + "clip_edge_x_load_sites = [\"0x80010030\"]\n"
                                         "clip_edge_width = 368\n");
    check(clip_hash != PSXRecompV4::overlay_codegen_config_hash(with_clip_w),
          "clip_edge_width enters the hash with its sites");
}

void codegen_emits_bgez() {
    PSXRecomp::CodeGenConfig config{};
    config.emit_comments = true;
    const auto vanilla = generate_first_instruction(kBgezV0, config).full_code;
    check(vanilla.find("(int32_t)cpu->gpr[2] >= 0") != std::string::npos,
          "unlisted bgez stays vanilla");
    check(vanilla.find("psx_ws_cull_bgez") == std::string::npos,
          "unlisted bgez does not call the helper");

    config.ws_cull_bgez_sites.insert(kBase);
    const auto widened = generate_first_instruction(kBgezV0, config).full_code;
    check(widened.find("psx_ws_cull_bgez(cpu->gpr[2]) /* ws cull (left keep) */") !=
              std::string::npos,
          "listed bgez routes through psx_ws_cull_bgez");
}

void codegen_emits_clip_edge_loads() {
    PSXRecomp::CodeGenConfig config{};
    config.emit_comments = true;
    config.ws_cull_clip_edge_x_load_sites.insert(kBase);

    const auto lh = generate_first_instruction(kLhV0, config).full_code;
    check(lh.find("cpu->gpr[2] = psx_ws_clip_edge_x((uint32_t)(int32_t)(int16_t)"
                  "psx_cyc_load_half(cpu, cpu->gpr[4] + 112, 2, 0x10u), 320u);") !=
              std::string::npos,
          "lh clip bound is sign-extended then widened against 320");

    const auto lhu = generate_first_instruction(kLhuV0, config).full_code;
    check(lhu.find("cpu->gpr[2] = psx_ws_clip_edge_x((uint32_t)"
                   "psx_cyc_load_half(cpu, cpu->gpr[4] + 112, 2, 0x10u), 320u);") !=
              std::string::npos,
          "lhu clip bound is zero-extended then widened");

    config.ws_cull_clip_edge_width = 368;
    const auto lw = generate_first_instruction(kLwV0, config).full_code;
    check(lw.find("cpu->gpr[2] = psx_ws_clip_edge_x("
                  "psx_cyc_load_word(cpu, cpu->gpr[4] + 112, 2, 0x10u), 368u);") !=
              std::string::npos,
          "lw clip bound uses the configured width");

    PSXRecomp::CodeGenConfig plain{};
    const auto vanilla = generate_first_instruction(kLhV0, plain).full_code;
    check(vanilla.find("psx_ws_clip_edge_x") == std::string::npos,
          "unlisted lh stays vanilla");
}

void main_exe_guards_fail_the_build() {
    PSXRecomp::CodeGenConfig bgez{};
    bgez.ws_cull_bgez_sites.insert(kBase);
    check(exits_with_failure([&] { generate_first_instruction(kBltzV0, bgez); }),
          "a bgez site holding bltz fails main-EXE generation");
    check(exits_with_failure([&] { generate_first_instruction(kAddiuV0, bgez); }),
          "a bgez site holding a non-branch fails main-EXE generation");

    PSXRecomp::CodeGenConfig clip{};
    clip.ws_cull_clip_edge_x_load_sites.insert(kBase);
    check(exits_with_failure([&] { generate_first_instruction(kAddiuV0, clip); }),
          "a clip-edge site holding a non-load fails main-EXE generation");
    check(exits_with_failure([&] { generate_first_instruction(kLhZero, clip); }),
          "a clip-edge load into $zero fails main-EXE generation");

    // Captured overlays may hold unrelated code at a listed address: keep it.
    bgez.overlay_mode = true;
    const auto overlay_bltz = generate_first_instruction(kBltzV0, bgez).full_code;
    check(overlay_bltz.find("(int32_t)cpu->gpr[2] < 0") != std::string::npos &&
              overlay_bltz.find("psx_ws_cull_bgez") == std::string::npos,
          "overlay variant at a bgez site stays vanilla");
    clip.overlay_mode = true;
    const auto overlay_addiu = generate_first_instruction(kAddiuV0, clip).full_code;
    check(overlay_addiu.find("psx_ws_clip_edge_x") == std::string::npos,
          "overlay variant at a clip-edge site stays vanilla");
}

// bltz_sites and branch_keep_sites get the same main-EXE guard as bgez_sites
// (docs/WIDESCREEN.md "Explicit screen-X cull sites"); jumps, which never
// reach generate_branch_condition, are checked at the block exit.
void branch_site_guards() {
    PSXRecomp::CodeGenConfig bltz{};
    bltz.ws_cull_bltz_sites.insert(kBase);
    check(generate_first_instruction(kBltzV0, bltz).full_code.find(
              "psx_ws_cull_bltz(cpu->gpr[2])") != std::string::npos,
          "a bltz site on bltz emits the helper");
    check(!exits_with_failure([&] { generate_first_instruction(kBltzV0, bltz); }),
          "a bltz site on bltz generates");
    check(exits_with_failure([&] { generate_first_instruction(kBgezV0, bltz); }),
          "a bltz site holding bgez fails main-EXE generation");
    check(exits_with_failure([&] { generate_first_instruction(kBltzalV0, bltz); }),
          "a bltz site holding bltzal fails main-EXE generation");
    check(exits_with_failure([&] { generate_first_instruction(kAddiuV0, bltz); }),
          "a bltz site holding a non-branch fails main-EXE generation");
    check(exits_with_failure([&] { generate_first_instruction(kJumpJr, bltz); }),
          "a bltz site holding a jump fails main-EXE generation");

    PSXRecomp::CodeGenConfig bgez{};
    bgez.ws_cull_bgez_sites.insert(kBase);
    check(exits_with_failure([&] { generate_first_instruction(kJumpJr, bgez); }),
          "a bgez site holding a jump fails main-EXE generation");

    PSXRecomp::CodeGenConfig keep{};
    keep.ws_cull_branch_keep_sites.insert(kBase);
    check(generate_first_instruction(kBeqV0, keep).full_code.find(
              "ws branch keep") != std::string::npos,
          "a branch-keep site on beq keeps the branch while wide");
    check(!exits_with_failure([&] { generate_first_instruction(kBeqV0, keep); }) &&
              !exits_with_failure([&] { generate_first_instruction(kBgezV0, keep); }) &&
              !exits_with_failure([&] { generate_first_instruction(kBltzalV0, keep); }),
          "a branch-keep site on a conditional branch generates");
    check(exits_with_failure([&] { generate_first_instruction(kAddiuV0, keep); }),
          "a branch-keep site holding a non-branch fails main-EXE generation");
    check(exits_with_failure([&] { generate_first_instruction(kJumpJr, keep); }),
          "a branch-keep site holding a jump fails main-EXE generation");

    // Captured overlays may hold unrelated code at a listed address: keep it.
    bltz.overlay_mode = true;
    keep.overlay_mode = true;
    check(!exits_with_failure([&] { generate_first_instruction(kAddiuV0, bltz); }) &&
              !exits_with_failure([&] { generate_first_instruction(kJumpJr, bltz); }) &&
              !exits_with_failure([&] { generate_first_instruction(kAddiuV0, keep); }),
          "overlay variants at bltz/branch-keep sites generate vanilla code");
    check(generate_first_instruction(kBgezV0, bltz).full_code.find(
              "psx_ws_cull_bltz") == std::string::npos,
          "overlay variant at a bltz site stays vanilla");
}

// In overlay code a listed address whose instruction matches is widened like
// the main EXE (the dirty-RAM interpreter does the same), so the shard must
// link both helpers: overlay_dispatch_preamble.c.inc defines them
// (overlay_shim_compile_contract, overlay_widescreen_callbacks).
void overlay_matching_sites_emit_helpers() {
    PSXRecomp::CodeGenConfig bgez{};
    bgez.ws_cull_bgez_sites.insert(kBase);
    bgez.overlay_mode = true;
    check(generate_first_instruction(kBgezV0, bgez).full_code.find(
              "psx_ws_cull_bgez(cpu->gpr[2])") != std::string::npos,
          "overlay bgez at a bgez site emits psx_ws_cull_bgez");
    PSXRecomp::CodeGenConfig clip{};
    clip.ws_cull_clip_edge_x_load_sites.insert(kBase);
    clip.overlay_mode = true;
    check(generate_first_instruction(kLhV0, clip).full_code.find(
              "psx_ws_clip_edge_x(") != std::string::npos,
          "overlay lh at a clip-edge site emits psx_ws_clip_edge_x");
}

void shared_decls_include_new_helpers() {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    PSXRecomp::CodeGenerator generator(exe);
    std::vector<PSXRecomp::GeneratedFunction> functions;
    const auto decls = generator.build_shared_decls_header(functions);
    check(decls.find("psx_ws_cull_bgez(uint32_t v)") != std::string::npos,
          "shared declarations include psx_ws_cull_bgez");
    check(decls.find("psx_ws_clip_edge_x(uint32_t v, uint32_t w)") != std::string::npos,
          "shared declarations include psx_ws_clip_edge_x");
}

void masked_reject_sites() {
    const std::string plain = base_config();
    const std::string body = plain + R"toml(
[[widescreen.cull.masked_reject]]
address = "0x80010000"
expected = "0x14800002"
reject_mask = "0xFFFF0000"
)toml";
    const auto loaded = load(body);
    check(loaded.ws_cull_masked_reject_sites.size() == 1, "load guarded masked reject");
    check(PSXRecompV4::overlay_codegen_config_hash(load(plain)) !=
          PSXRecompV4::overlay_codegen_config_hash(loaded), "masked reject affects cache identity");
    auto changed = loaded;
    changed.ws_cull_masked_reject_sites[0].reject_mask = 0xFF000000u;
    check(PSXRecompV4::overlay_codegen_config_hash(changed) !=
          PSXRecompV4::overlay_codegen_config_hash(loaded), "reject mask affects cache identity");
    PSXRecomp::CodeGenConfig config{};
    config.ws_cull_masked_reject_sites = loaded.ws_cull_masked_reject_sites;
    const auto code = generate_first_instruction(0x14800002u, config).full_code;
    check(code.find("psx_ws_masked_reject(cpu->gpr[4], 0xFFFF0000u)") != std::string::npos,
          "native branch emits guarded masked predicate");
    config.overlay_mode = true;
    const auto other = generate_first_instruction(0x14800003u, config).full_code;
    check(other.find("psx_ws_masked_reject(") == std::string::npos,
          "overlay with another instruction stays vanilla");

    const std::string flag_body = plain + R"toml(
[[widescreen.cull.masked_reject]]
address = "0x80010000"
expected = "0x04400002"
reject_mask = "0x7F87A000"
)toml";
    const auto flag_config = load(flag_body);
    config.ws_cull_masked_reject_sites = flag_config.ws_cull_masked_reject_sites;
    config.overlay_mode = false;
    const auto flags = generate_first_instruction(kBltzV0, config).full_code;
    check(flags.find("psx_ws_x_margin() > 0 ? psx_ws_masked_reject(cpu->gpr[2], 0x7F87A000u) : ((int32_t)cpu->gpr[2] < 0)") != std::string::npos,
          "GTE summary branch preserves signed predicate at 4:3");
    check(load_throws_with(plain + R"toml(
[[widescreen.cull.masked_reject]]
address = "0x80010000"
expected = "0x04500002"
reject_mask = "0x7F87A000"
)toml", "BLTZ reg"), "masked reject excludes link branch");
    config.overlay_mode = true;
    const auto mismatched_flags = generate_first_instruction(kBgezV0, config).full_code;
    check(mismatched_flags.find("psx_ws_masked_reject(") == std::string::npos,
          "GTE overlay opcode mismatch keeps signed vanilla branch");
}

void runtime_math() {
    // Identity at 4:3 (margin 0) for every input.
    for (int32_t v : {-1000, -54, -53, -1, 0, 1, 98, 222, 319, 320, 321, 1000}) {
        check(psx_ws_cull_bgez_value(v, 0) == (v >= 0 ? 1 : 0),
              "bgez is vanilla at margin 0");
        check(psx_ws_clip_edge_x_value((uint32_t)v, 320u, 0) == (uint32_t)v,
              "clip edge is the identity at margin 0");
    }
    // 16:9 on a 320-wide display: margin 53.
    check(psx_ws_cull_bgez_value(-53, 53) == 1, "bgez keeps x = -m");
    check(psx_ws_cull_bgez_value(-54, 53) == 0, "bgez rejects x < -m");
    check(psx_ws_cull_bgez_value(-5, 53) == 1, "bgez keeps the revealed band");
    check(psx_ws_clip_edge_x_value(0u, 320u, 53) == (uint32_t)-53,
          "left screen edge moves to -m");
    check(psx_ws_clip_edge_x_value(320u, 320u, 53) == 373u,
          "right screen edge moves to W+m");
    check(psx_ws_clip_edge_x_value(98u, 320u, 53) == 98u &&
              psx_ws_clip_edge_x_value(222u, 320u, 53) == 222u,
          "interior viewport edges (mirror) are unchanged");
    check(psx_ws_clip_edge_x_value(160u, 320u, 53) == 160u,
          "split-screen centre edge is unchanged");
    check(psx_ws_clip_edge_x_value(0xFFFFFFFFu, 320u, 53) == 0xFFFFFFFFu,
          "a negative bound other than 0 is unchanged");
    check(psx_ws_cull_bgez_value(-1, -7) == 0,
          "a negative margin is treated as 0");
}

}  // namespace

int main(int argc, char** argv) {
    g_self = argv[0];
    if (argc == 3 && std::string(argv[1]) == "--exit-probe") {
        g_exit_probe = std::atoi(argv[2]);
        if (!std::freopen("NUL", "w", stderr)) return 3;
    }
    loader_parses_new_kinds();
    hash_identity_only_changes_when_used();
    codegen_emits_bgez();
    codegen_emits_clip_edge_loads();
    main_exe_guards_fail_the_build();
    branch_site_guards();
    overlay_matching_sites_emit_helpers();
    shared_decls_include_new_helpers();
    runtime_math();
    masked_reject_sites();

    if (g_exit_probe >= 0) return 3;  // no call had that probe index
    if (failures != 0) {
        std::fprintf(stderr, "ws_cull_edge_codegen_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::puts("PASS: ws cull edge kinds");
    return 0;
}
