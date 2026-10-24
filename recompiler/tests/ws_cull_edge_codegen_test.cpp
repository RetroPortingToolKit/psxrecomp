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
}

// ---- [[widescreen.cull.sxy]] ----------------------------------------------
// The Wild 9 (SLUS-00425) shapes, relocated to kBase. Tri: the fold has GTE
// reads and stores between its ANDs and one instruction before the consumer.
//   +00 and at,t0,s1   +04 mfc2 t3,$14   +08 and at,s4,at   +0C sw t0,8(a0)
//   +10 and at,t3,at   +14 sub t0,s0,t0  +18 bne at,zero,+1 ; nop ; jr ra ; nop
// Quad: four contiguous ANDs, then the consumer.
const std::vector<uint32_t> kTriWords = {
    0x01110824u, 0x480B7000u, 0x02810824u, 0xAC880008u, 0x01610824u,
    0x02084022u, 0x14200001u, 0x00000000u, 0x03E00008u, 0x00000000u};
const std::vector<uint32_t> kQuadWords = {
    0x01110824u, 0x02810824u, 0x01610824u, 0x01210824u, 0x14200001u,
    0x00000000u, 0x03E00008u, 0x00000000u};

PSXRecomp::GeneratedFunction generate_words(const std::vector<uint32_t>& words,
                                            const PSXRecomp::CodeGenConfig& config) {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    exe.header.initial_pc = kBase;
    exe.header.file_size = (uint32_t)(4 * words.size());
    for (uint32_t w : words) append_word(exe.code_data, w);
    PSXRecomp::Function function{};
    function.start_addr = kBase;
    function.end_addr = kBase + 4u * (uint32_t)words.size();
    function.size = 4u * (uint32_t)words.size();
    function.name = "ws_cull_sxy_test";
    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg);
}

struct SxyToml {
    std::string kind = "tri";
    std::string vertex_regs = "[8, 20, 11]";
    std::string fold_addresses = R"(["0x80010000", "0x80010008", "0x80010010"])";
    std::string fold_expected = R"(["0x01110824", "0x02810824", "0x01610824"])";
    std::string consumer_address = "0x80010018";
    std::string consumer_expected = "0x14200001";
    std::string str() const {
        return "\n[[widescreen.cull.sxy]]\nkind = \"" + kind + "\"\n" +
               "vertex_regs = " + vertex_regs + "\n" +
               "fold_addresses = " + fold_addresses + "\n" +
               "fold_expected = " + fold_expected + "\n" +
               "consumer_address = \"" + consumer_address + "\"\n" +
               "consumer_expected = \"" + consumer_expected + "\"\n";
    }
};

SxyToml quad_toml() {
    SxyToml q;
    q.kind = "quad";
    q.vertex_regs = "[8, 20, 11, 9]";
    q.fold_addresses = R"(["0x80010000", "0x80010004", "0x80010008", "0x8001000C"])";
    q.fold_expected = R"(["0x01110824", "0x02810824", "0x01610824", "0x01210824"])";
    q.consumer_address = "0x80010010";
    return q;
}

void sxy_loader() {
    const auto tri = load(base_config() + SxyToml{}.str());
    check(tri.ws_cull_sxy_sites.size() == 1, "loader reads a tri sxy site");
    const auto& t = tri.ws_cull_sxy_sites.at(0);
    check(t.vertices == 3 && t.vertex_regs == std::vector<uint32_t>({8, 20, 11}) &&
              t.fold_addresses == std::vector<uint32_t>({0x80010000u, 0x80010008u, 0x80010010u}) &&
              t.fold_expected == std::vector<uint32_t>({0x01110824u, 0x02810824u, 0x01610824u}) &&
              t.consumer_address == 0x80010018u && t.consumer_expected == 0x14200001u,
          "tri sxy site fields");
    const auto quad = load(base_config() + quad_toml().str());
    check(quad.ws_cull_sxy_sites.size() == 1 && quad.ws_cull_sxy_sites[0].vertices == 4,
          "loader reads a quad sxy site");
    check(load(base_config()).ws_cull_sxy_sites.empty(), "sxy defaults to empty");

    // Operands may be written in either order; beq is a consumer as well.
    SxyToml swapped;
    swapped.fold_expected = R"(["0x02280824", "0x00340824", "0x002B0824"])";
    swapped.consumer_expected = "0x10010001";
    check(load(base_config() + swapped.str()).ws_cull_sxy_sites.size() == 1,
          "swapped AND operands and a beq zero,at consumer are accepted");

    auto bad = [&](SxyToml site, const char* needle, const char* what) {
        check(load_throws_with(base_config() + site.str(), needle), what);
    };
    SxyToml s;
    s = {}; s.kind = "pent";
    bad(s, "kind must be", "unknown kind");
    s = {}; s.vertex_regs = "[8, 20]";
    bad(s, "needs exactly 3", "tri with two vertex_regs");
    s = quad_toml(); s.fold_expected = R"(["0x01110824", "0x02810824", "0x01610824"])";
    bad(s, "needs exactly 4", "quad with three AND words");
    s = {}; s.vertex_regs = "[8, 8, 11]";
    bad(s, "must be distinct", "repeated vertex register");
    s = {}; s.vertex_regs = "[8, 20, 0]";
    bad(s, "not a GPR", "$zero vertex register");
    s = {}; s.vertex_regs = "[8, 20, 1]";
    bad(s, "is the fold register", "vertex register equal to the fold register");
    s = {}; s.vertex_regs = "[8, 20, 12]";
    bad(s, "does not read vertex register 12", "AND does not read its vertex");
    s = {}; s.vertex_regs = "[8, 20, 17]";
    s.fold_expected = R"(["0x01140824", "0x02810824", "0x02210824"])";
    bad(s, "separate mask register", "first AND combines two vertices");
    s = {}; s.fold_expected = R"(["0x01110824", "0x02811024", "0x01610824"])";
    bad(s, "same destination", "AND into another register");
    s = {}; s.fold_expected = R"(["0x01110825", "0x02810824", "0x01610824"])";
    bad(s, "must be `and r", "OR in the fold");
    s = {}; s.fold_expected = R"(["0x01110824", "0x02910824", "0x01610824"])";
    bad(s, "with the fold register", "later AND not chained through the fold register");
    s = {}; s.fold_expected = R"(["0x01100024", "0x02800024", "0x01600024"])";
    bad(s, "must not write $zero", "fold into $zero");
    s = {}; s.consumer_expected = "0x14220001";
    bad(s, "beq/bne of the fold register", "consumer compares with another register");
    s = {}; s.consumer_expected = "0x04200001";
    bad(s, "beq/bne of the fold register", "consumer is bltz");
    s = {}; s.consumer_address = "0x8001002C";
    bad(s, "within 4 instructions", "consumer too far from the fold");
    s = {}; s.fold_addresses = R"(["0x80010008", "0x80010000", "0x80010010"])";
    bad(s, "must follow", "fold addresses out of order");
    s = {}; s.consumer_address = "0x80010010";
    bad(s, "must follow", "consumer on the last AND");
    s = {}; s.fold_addresses = R"(["0x80010000", "0xA0010008", "0x80010010"])";
    bad(s, "segment of the first AND", "fold crossing segments");
    s = {}; s.fold_addresses = R"(["0x80010000", "0x8001000A", "0x80010010"])";
    bad(s, "word-aligned", "misaligned AND");
    check(load_throws_with(base_config() + SxyToml{}.str() + quad_toml().str(),
                           "already used by another sxy site"),
          "two sites sharing a physical address");
}

void sxy_hash_identity() {
    const auto plain = load(base_config());
    const auto with = load(base_config() + SxyToml{}.str());
    check(PSXRecompV4::overlay_codegen_config_hash(plain) !=
              PSXRecompV4::overlay_codegen_config_hash(with),
          "an sxy site changes the overlay-cache identity");
    auto changed = with;
    changed.ws_cull_sxy_sites[0].consumer_expected = 0x10200001u;
    check(PSXRecompV4::overlay_codegen_config_hash(changed) !=
              PSXRecompV4::overlay_codegen_config_hash(with),
          "the consumer word is part of the overlay-cache identity");
    changed = with;
    changed.ws_cull_sxy_sites[0].vertex_regs[2] = 12u;
    check(PSXRecompV4::overlay_codegen_config_hash(changed) !=
              PSXRecompV4::overlay_codegen_config_hash(with),
          "the vertex registers are part of the overlay-cache identity");
}

bool has_text(const std::string& code, const char* needle) {
    return code.find(needle) != std::string::npos;
}

void sxy_codegen() {
    PSXRecomp::CodeGenConfig config{};
    const auto vanilla_tri = generate_words(kTriWords, config).full_code;
    check(!has_text(vanilla_tri, "psx_ws_cull_sxy_and"), "no sxy site: vanilla ANDs");

    config.ws_cull_sxy_sites = load(base_config() + SxyToml{}.str()).ws_cull_sxy_sites;
    const auto tri = generate_words(kTriWords, config).full_code;
    check(has_text(tri, "cpu->gpr[1] = psx_ws_cull_sxy_and(cpu->gpr[17], cpu->gpr[8]);"),
          "tri AND 0 seeds from the mask with vertex t0");
    check(has_text(tri, "cpu->gpr[1] = psx_ws_cull_sxy_and(cpu->gpr[1], cpu->gpr[20]);"),
          "tri AND 1 folds vertex s4");
    check(has_text(tri, "cpu->gpr[1] = psx_ws_cull_sxy_and(cpu->gpr[1], cpu->gpr[11]);"),
          "tri AND 2 folds vertex t3");
    check(!has_text(tri, "cpu->gpr[1] = cpu->gpr[8] & cpu->gpr[17];"),
          "tri vanilla AND is replaced");
    check(!exits_with_failure([&] { generate_words(kTriWords, config); }),
          "a matching tri fold generates");

    PSXRecomp::CodeGenConfig qconfig{};
    qconfig.ws_cull_sxy_sites = load(base_config() + quad_toml().str()).ws_cull_sxy_sites;
    const auto quad = generate_words(kQuadWords, qconfig).full_code;
    check(has_text(quad, "cpu->gpr[1] = psx_ws_cull_sxy_and(cpu->gpr[17], cpu->gpr[8]);") &&
              has_text(quad, "cpu->gpr[1] = psx_ws_cull_sxy_and(cpu->gpr[1], cpu->gpr[20]);") &&
              has_text(quad, "cpu->gpr[1] = psx_ws_cull_sxy_and(cpu->gpr[1], cpu->gpr[11]);") &&
              has_text(quad, "cpu->gpr[1] = psx_ws_cull_sxy_and(cpu->gpr[1], cpu->gpr[9]);"),
          "quad folds all four vertices");

    // The guest's own consumer branch is untouched.
    check(has_text(quad, "cpu->gpr[1] != cpu->gpr[0]") || has_text(quad, "cpu->gpr[1] != 0"),
          "the consumer bne is the vanilla branch");

    // Any word of the fold or the consumer that differs: main EXE fails,
    // an overlay variant keeps every AND vanilla.
    for (size_t i : {size_t(1), size_t(2), size_t(6)}) {
        auto words = kTriWords;
        if (i == 1) words[2] = 0x02810825u;  // the second AND is an OR
        if (i == 2) words[4] = 0x01710824u;  // the last AND reads s1
        if (i == 6) words[6] = 0x10200001u;  // beq instead of bne
        check(exits_with_failure([&] { generate_words(words, config); }),
              "a mismatching tri fold fails main-EXE generation");
        auto overlay = config;
        overlay.overlay_mode = true;
        const auto code = generate_words(words, overlay).full_code;
        check(!has_text(code, "psx_ws_cull_sxy_and"),
              "an overlay variant with any mismatching word stays vanilla");
    }
    auto overlay = config;
    overlay.overlay_mode = true;
    check(has_text(generate_words(kTriWords, overlay).full_code,
                   "psx_ws_cull_sxy_and(cpu->gpr[17], cpu->gpr[8])"),
          "an overlay with the exact fold emits the helper");

    // Declared only for a config that lists sxy sites.
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    std::vector<PSXRecomp::GeneratedFunction> functions;
    check(!has_text(PSXRecomp::CodeGenerator(exe).build_shared_decls_header(functions),
                    "psx_ws_cull_sxy_and"),
          "a config without sxy sites does not declare the helper");
    check(has_text(PSXRecomp::CodeGenerator(exe, config).build_shared_decls_header(functions),
                   "extern uint32_t psx_ws_cull_sxy_and(uint32_t acc, uint32_t sxy);"),
          "a config with sxy sites declares the helper");
}

uint32_t pack_sxy(int x, int y) {
    return ((uint32_t)(uint16_t)(int16_t)y << 16) | (uint16_t)(int16_t)x;
}

uint32_t sxy_fold(const uint32_t* v, int n, uint32_t mask, int32_t m) {
    uint32_t r = psx_ws_sxy_and_value(mask, v[0], m);
    for (int k = 1; k < n; k++) r = psx_ws_sxy_and_value(r, v[k], m);
    return r;
}

// Every tri (and a quad sweep) over the edge values: at margin 0 the fold is
// the vanilla AND word for any mask; widened, it differs from vanilla only in
// bit 15, which then means "every X < -m". Bit 31 (all Y above the screen)
// and the right/bottom side (a separate chain in the guest) are unchanged.
void sxy_runtime_math() {
    const int xs[] = {-1024, -200, -54, -53, -52, -1, 0, 1, 160, 319, 320, 1023};
    const int ys[] = {-1024, -1, 0, 1, 239, 240};
    std::vector<uint32_t> pts;
    for (int x : xs) for (int y : ys) pts.push_back(pack_sxy(x, y));
    const uint32_t masks[] = {0x80008000u, 0xFFFFFFFFu, 0x00008000u, 0x80000000u};
    int bad0 = 0, badm = 0;
    for (uint32_t mask : masks)
        for (uint32_t a : pts) for (uint32_t b : pts) for (uint32_t c : pts) {
            const uint32_t v[3] = {a, b, c};
            const uint32_t vanilla = mask & a & b & c;
            if (sxy_fold(v, 3, mask, 0) != vanilla) bad0++;
            for (int32_t m : {53, 192}) {
                bool all_left = true;
                for (uint32_t p : v) all_left &= (int16_t)(p & 0xFFFFu) < -m;
                const uint32_t want = (vanilla & ~0x8000u) |
                                      (all_left ? (vanilla & 0x8000u) : 0u);
                if (sxy_fold(v, 3, mask, m) != want) badm++;
            }
        }
    for (uint32_t a : pts) for (uint32_t d : pts) {
        const uint32_t v[4] = {a, pack_sxy(-300, 10), pack_sxy(-60, -5), d};
        const uint32_t vanilla = 0x80008000u & v[0] & v[1] & v[2] & v[3];
        if (sxy_fold(v, 4, 0x80008000u, 0) != vanilla) bad0++;
        bool all_left = true;
        for (uint32_t p : v) all_left &= (int16_t)(p & 0xFFFFu) < -53;
        const uint32_t want = (vanilla & ~0x8000u) | (all_left ? (vanilla & 0x8000u) : 0u);
        if (sxy_fold(v, 4, 0x80008000u, 53) != want) badm++;
    }
    check(bad0 == 0, "the sxy fold is the vanilla AND at margin 0");
    check(badm == 0, "widened, the sxy fold differs only in bit 15 = every X < -m");

    // Spot checks at 16:9 on 320 wide (margin 53), mask 0x80008000.
    const uint32_t mask = 0x80008000u;
    uint32_t v[3] = {pack_sxy(-60, 10), pack_sxy(-54, 20), pack_sxy(-100, 30)};
    check(sxy_fold(v, 3, mask, 53) == 0x8000u, "all vertices left of -m: rejected");
    v[1] = pack_sxy(-53, 20);
    check(sxy_fold(v, 3, mask, 53) == 0u, "one vertex in the revealed band: kept");
    check(sxy_fold(v, 3, mask, 0) == 0x8000u, "the same triangle is rejected at 4:3");
    uint32_t up[3] = {pack_sxy(-10, -1), pack_sxy(100, -5), pack_sxy(5, -9)};
    check(sxy_fold(up, 3, mask, 53) == 0x80000000u, "all above the screen: still rejected");
    check(psx_ws_sxy_and_value(0x8000u, pack_sxy(-5, 0), -7) == 0x8000u,
          "a negative margin is treated as 0");
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
    sxy_loader();
    sxy_hash_identity();
    sxy_codegen();
    sxy_runtime_math();

    if (g_exit_probe >= 0) return 3;  // no call had that probe index
    if (failures != 0) {
        std::fprintf(stderr, "ws_cull_edge_codegen_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::puts("PASS: ws cull edge kinds");
    return 0;
}
