// [widescreen.cull] screen_x_sites on `lui` and `bltz`: a screen X kept in the
// high half of a register. Emitted native code for hand-written instruction
// words, the main-EXE guard, overlay variants, the declarations (only when the
// key is used), and the shared runtime math (ws_cull_edge.h) the emitted
// helpers call.
#include "code_generator.h"
#include "control_flow.h"
#include "../../runtime/include/ws_cull_edge.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <cstdlib>
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr uint32_t kBase = 0x80010000u;
int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

bool has(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

// A tiny function: <word> ; nop ; addiu v1,zero,1 ; jr ra ; nop. A branch in
// the first slot targets the jr (kBase+12).
std::string generate_first_instruction(uint32_t first_word,
                                       const PSXRecomp::CodeGenConfig& config) {
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
    function.name = "ws_cull_screen_x_hi_test";

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg).full_code;
}

// Runs `body` in a child and reports whether it exited with a failure status.
// The main-EXE guard calls std::exit(1), which a plain call cannot observe.
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

constexpr uint32_t kSltiuV0 = 0x2C620200u;   // sltiu v0, v1, 0x200
constexpr uint32_t kLuiS6 = 0x3C160200u;     // lui   s6, 0x200
constexpr uint32_t kLuiZero = 0x3C000200u;   // lui   zero, 0x200
constexpr uint32_t kBltzT0 = 0x05000002u;    // bltz  t0, +2
constexpr uint32_t kBgezT0 = 0x05010002u;    // bgez  t0, +2
constexpr uint32_t kBltzalT0 = 0x05100002u;  // bltzal t0, +2
constexpr uint32_t kAddiuV0 = 0x24820070u;   // addiu v0, a0, 0x70

const char* const kLuiHelper =
    "cpu->gpr[22] = psx_ws_cull_lui_hi(0x0200u); "
    "/* ws explicit screen-x edge (high half) */";
const char* const kBltzHelper =
    "psx_ws_cull_bltz_hi(cpu->gpr[8]) /* ws cull (left edge, X in the high half) */";

void unlisted_instructions_stay_vanilla() {
    PSXRecomp::CodeGenConfig config{};
    config.emit_comments = true;
    const auto lui = generate_first_instruction(kLuiS6, config);
    check(has(lui, "cpu->gpr[22] = 0x0200 << 16;"), "unlisted lui stays vanilla");
    check(!has(lui, "psx_ws_cull_lui_hi"), "unlisted lui does not call the helper");
    const auto bltz = generate_first_instruction(kBltzT0, config);
    check(has(bltz, "(int32_t)cpu->gpr[8] < 0"), "unlisted bltz stays vanilla");
    check(!has(bltz, "psx_ws_cull_bltz_hi"), "unlisted bltz does not call the helper");

    // A site at another address does not touch these instructions.
    config.ws_cull_screen_x_sites.insert(kBase + 0x100u);
    check(generate_first_instruction(kLuiS6, config) == lui,
          "a screen_x site elsewhere leaves the lui's function unchanged");
    check(generate_first_instruction(kBltzT0, config) == bltz,
          "a screen_x site elsewhere leaves the bltz's function unchanged");
}

void listed_sites_emit_the_helpers() {
    PSXRecomp::CodeGenConfig config{};
    config.emit_comments = true;
    config.ws_cull_screen_x_sites.insert(kBase);

    const auto lui = generate_first_instruction(kLuiS6, config);
    check(has(lui, kLuiHelper), "a screen_x site on lui routes through psx_ws_cull_lui_hi");
    check(!has(lui, "cpu->gpr[22] = 0x0200 << 16;"), "the vanilla lui is replaced");
    check(!exits_with_failure([&] { generate_first_instruction(kLuiS6, config); }),
          "a screen_x site on lui generates");

    const auto bltz = generate_first_instruction(kBltzT0, config);
    check(has(bltz, kBltzHelper), "a screen_x site on bltz routes through psx_ws_cull_bltz_hi");
    check(!has(bltz, "(int32_t)cpu->gpr[8] < 0"), "the vanilla bltz predicate is replaced");
    check(!exits_with_failure([&] { generate_first_instruction(kBltzT0, config); }),
          "a screen_x site on bltz generates");

    // The existing form is unchanged.
    check(has(generate_first_instruction(kSltiuV0, config),
              "cpu->gpr[2] = psx_ws_cull_sltiu(cpu->gpr[3], 512); "
              "/* ws explicit screen-x cull */"),
          "a screen_x site on sltiu still routes through psx_ws_cull_sltiu");
}

// bltz_sites is the low-half form and keeps its meaning when an address is in
// both lists.
void bltz_sites_wins_over_screen_x_sites() {
    PSXRecomp::CodeGenConfig config{};
    config.ws_cull_screen_x_sites.insert(kBase);
    config.ws_cull_bltz_sites.insert(kBase);
    const auto both = generate_first_instruction(kBltzT0, config);
    check(has(both, "psx_ws_cull_bltz(cpu->gpr[8])") && !has(both, "psx_ws_cull_bltz_hi"),
          "an address in bltz_sites keeps the low-half helper");
}

void main_exe_guard_and_overlay_variants() {
    PSXRecomp::CodeGenConfig config{};
    config.ws_cull_screen_x_sites.insert(kBase);
    check(exits_with_failure([&] { generate_first_instruction(kAddiuV0, config); }),
          "a screen_x site holding addiu fails main-EXE generation");
    check(exits_with_failure([&] { generate_first_instruction(kLuiZero, config); }),
          "a screen_x site holding lui into $zero fails main-EXE generation");

    // Other branches at a listed address keep their vanilla predicate.
    const auto bgez = generate_first_instruction(kBgezT0, config);
    check(has(bgez, "(int32_t)cpu->gpr[8] >= 0") && !has(bgez, "psx_ws_cull_bltz_hi"),
          "a bgez at a screen_x site is not widened");
    const auto bltzal = generate_first_instruction(kBltzalT0, config);
    check(!has(bltzal, "psx_ws_cull_bltz_hi"), "a bltzal at a screen_x site is not widened");

    // Captured overlays may hold unrelated code at a listed address: keep it.
    // A matching instruction is widened like the main EXE, so the shard must
    // link both helpers (overlay_dispatch_preamble.c.inc defines them).
    config.overlay_mode = true;
    check(!exits_with_failure([&] { generate_first_instruction(kAddiuV0, config); }),
          "an overlay variant holding addiu at a screen_x site generates");
    check(!has(generate_first_instruction(kAddiuV0, config), "psx_ws_cull_"),
          "an overlay variant holding addiu stays vanilla");
    check(has(generate_first_instruction(kLuiS6, config), "psx_ws_cull_lui_hi(0x0200u)"),
          "an overlay lui at a screen_x site emits psx_ws_cull_lui_hi");
    check(has(generate_first_instruction(kBltzT0, config), "psx_ws_cull_bltz_hi(cpu->gpr[8])"),
          "an overlay bltz at a screen_x site emits psx_ws_cull_bltz_hi");
}

// The two prototypes are written only for a config that lists screen_x_sites,
// so every other title's generated declarations are byte-identical.
void declarations_only_when_the_key_is_used() {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    std::vector<PSXRecomp::GeneratedFunction> functions;

    PSXRecomp::CodeGenerator plain(exe);
    const auto without = plain.build_shared_decls_header(functions);
    check(!has(without, "psx_ws_cull_lui_hi") && !has(without, "psx_ws_cull_bltz_hi"),
          "a config without screen_x_sites declares neither helper");

    PSXRecomp::CodeGenConfig config{};
    config.ws_cull_screen_x_sites.insert(kBase);
    PSXRecomp::CodeGenerator used(exe, config);
    const auto with = used.build_shared_decls_header(functions);
    check(has(with, "extern uint32_t psx_ws_cull_lui_hi(uint32_t imm);"),
          "a config with screen_x_sites declares psx_ws_cull_lui_hi");
    check(has(with, "extern int  psx_ws_cull_bltz_hi(uint32_t v);"),
          "a config with screen_x_sites declares psx_ws_cull_bltz_hi");
}

void runtime_math() {
    // Identity at 4:3 (margin 0): the lui's own value, the plain sign test.
    for (uint32_t imm : {0x0000u, 0x0001u, 0x0140u, 0x0200u, 0x0280u, 0x7FFFu,
                         0x8000u, 0xFF60u, 0xFFFFu}) {
        check(psx_ws_cull_lui_hi_value(imm, 0) == (imm << 16),
              "lui edge is the vanilla value at margin 0");
    }
    for (int32_t x : {-1000, -193, -192, -1, 0, 1, 511, 512, 1000}) {
        const uint32_t v = (uint32_t)x << 16;
        check(psx_ws_cull_bltz_hi_value(v, 0) == (x < 0 ? 1 : 0),
              "bltz is vanilla at margin 0");
        check(psx_ws_cull_bltz_hi_value(v | 0xFFFFu, 0) == (x < 0 ? 1 : 0),
              "bltz is vanilla at margin 0 with a fraction in the low half");
    }

    // 21:9 on a 512-wide display: margin 192.
    check(psx_ws_cull_lui_hi_value(0x0200u, 192) == (704u << 16),
          "right edge 512 moves to 704");
    check(psx_ws_cull_lui_hi_value(0xFF60u, 192) == ((uint32_t)-352 << 16),
          "left edge -160 moves to -352");
    check(psx_ws_cull_lui_hi_value(0x0000u, 192) == 0u, "a zero edge is unchanged");
    check(psx_ws_cull_bltz_hi_value((uint32_t)-192 << 16, 192) == 0, "bltz keeps x = -m");
    check(psx_ws_cull_bltz_hi_value((uint32_t)-193 << 16, 192) == 1, "bltz rejects x < -m");
    check(psx_ws_cull_bltz_hi_value(((uint32_t)-193 << 16) | 0xFFFFu, 192) == 1,
          "bltz rejects x < -m with a fraction in the low half");
    check(psx_ws_cull_bltz_hi_value((uint32_t)-5 << 16, 192) == 0,
          "bltz keeps the revealed band");
    check(psx_ws_cull_bltz_hi_value(5u << 16, 192) == 0, "bltz keeps an on-screen x");

    // 16:9 on a 320-wide display: margin 53.
    check(psx_ws_cull_lui_hi_value(0x0140u, 53) == (373u << 16),
          "right edge 320 moves to 373");
    check(psx_ws_cull_bltz_hi_value((uint32_t)-53 << 16, 53) == 0 &&
              psx_ws_cull_bltz_hi_value((uint32_t)-54 << 16, 53) == 1,
          "bltz boundary at margin 53");

    // The form bltz_sites cannot widen: a whole-register compare with -m.
    check(((int32_t)((uint32_t)-5 << 16) < -192) &&
              psx_ws_cull_bltz_hi_value((uint32_t)-5 << 16, 192) == 0,
          "a whole-register compare rejects x = -5; the high-half one keeps it");

    check(psx_ws_cull_lui_hi_value(0x0200u, -7) == (0x0200u << 16) &&
              psx_ws_cull_bltz_hi_value((uint32_t)-1 << 16, -7) == 1,
          "a negative margin is treated as 0");
}

}  // namespace

static void packed_x_forms() {
    PSXRecomp::CodeGenConfig config;
    config.ws_cull_packed_x_sites.push_back({kBase, 0x0079C02Bu});
    const auto emitted = generate_first_instruction(0x0079C02Bu, config);
    check(has(emitted, "psx_ws_cull_packed_x_value"), "packed X emits the shared predicate");
    config.overlay_mode = true;
    const auto alias = generate_first_instruction(0x0079C02Au, config);
    check(!has(alias, "psx_ws_cull_packed_x_value"), "different overlay opcode stays vanilla");
    for (int sx = -32768; sx <= 32767; ++sx) {
        const uint32_t x = (uint32_t)sx << 16;
        const uint32_t w = 320u << 16;
        check(psx_ws_cull_packed_x_value(x, w, 0) == (x < w), "packed X 4:3 identity");
        check(psx_ws_cull_packed_x_value(x, w, 120) == (sx >= -120 && sx < 440),
              "packed X covers exactly the expanded horizontal range");
    }
    check(psx_ws_cull_packed_x_value(0xFFFF0001u, 320u << 16, 120) == 0,
          "nonzero packed low half stays vanilla");
    check(psx_ws_cull_packed_x_value(0xFFFF0000u, 2048u << 16, 120) == 0,
          "non-screen width stays vanilla");
}

int main(int argc, char** argv) {
    g_self = argv[0];
    if (argc == 3 && std::string(argv[1]) == "--exit-probe") {
        g_exit_probe = std::atoi(argv[2]);
        if (!std::freopen("NUL", "w", stderr)) return 3;
    }
    unlisted_instructions_stay_vanilla();
    listed_sites_emit_the_helpers();
    bltz_sites_wins_over_screen_x_sites();
    main_exe_guard_and_overlay_variants();
    declarations_only_when_the_key_is_used();
    runtime_math();
    packed_x_forms();

    if (g_exit_probe >= 0) return 3;  // no call had that probe index
    if (failures != 0) {
        std::fprintf(stderr, "ws_cull_screen_x_hi_codegen_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::puts("PASS: ws cull screen_x high-half forms");
    return 0;
}
