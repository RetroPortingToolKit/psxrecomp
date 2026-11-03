// [[draw_distance.clamp]]: config parsing and validation, the emitted
// main-EXE clamp (and its absence everywhere else), the main-EXE guards, the
// shared declaration, and the overlay-cache identity (unchanged: overlays
// keep their own code at a listed address).
#include "code_generator.h"
#include "config_loader.h"
#include "control_flow.h"
#include "../../runtime/include/draw_distance.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
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
name = "Draw Distance Test"
id = "TEST-00000"
exe = "TEST.EXE"
load_address = "0x80010000"
entry_pc = "0x80010000"
text_size = "0x1000"
stack_base = "0x801FFFF0"

[recompiler]
seeds = "seeds.txt"
out_dir = "generated"
)toml";
}

std::string clamp(const char* address, const char* expected, int reg,
                  const char* max) {
    return std::string("\n[[draw_distance.clamp]]\naddress = \"") + address +
           "\"\nexpected = \"" + expected + "\"\nreg = " + std::to_string(reg) +
           "\nmax = " + max + "\n";
}

PSXRecompV4::GameConfig load(const std::string& body) {
    fs::path path = write_temp_config("draw-distance", body);
    auto config = PSXRecompV4::load_game_config(path);
    fs::remove(path);
    return config;
}

bool load_throws_with(const std::string& body, const char* needle) {
    fs::path path = write_temp_config("draw-distance-bad", body);
    bool matched = false;
    try {
        (void)PSXRecompV4::load_game_config(path);
    } catch (const std::exception& e) {
        matched = std::string(e.what()).find(needle) != std::string::npos;
        if (!matched) std::fprintf(stderr, "  (threw: %s)\n", e.what());
    }
    fs::remove(path);
    return matched;
}

// A function made of `words` followed by `jr ra ; nop`.
PSXRecomp::GeneratedFunction generate(std::initializer_list<uint32_t> words,
                                      const PSXRecomp::CodeGenConfig& config) {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    exe.header.initial_pc = kBase;
    for (uint32_t w : words) append_word(exe.code_data, w);
    append_word(exe.code_data, 0x03E00008u);
    append_word(exe.code_data, 0x00000000u);
    const uint32_t size = static_cast<uint32_t>(exe.code_data.size());
    exe.header.file_size = size;

    PSXRecomp::Function function{};
    function.start_addr = kBase;
    function.end_addr = kBase + size;
    function.size = size;
    function.name = "clamp_site_test";

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg);
}

// Runs `body` in a child and reports whether it exited with a failure status
// (the main-EXE guards call std::exit(1)). Windows has no fork: the Nth call
// re-runs this binary as `--exit-probe N`, which runs only the Nth body.
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

constexpr uint32_t kSltiuT2V0 = 0x2C4A01C0u;  // sltiu t2, v0, 0x1C0
constexpr uint32_t kAddiuAtV0 = 0x2441FFFFu;  // addiu at, v0, -1
constexpr uint32_t kSravV0 = 0x00561007u;     // srav  v0, s6, v0
constexpr uint32_t kLwV0 = 0x8C820060u;       // lw    v0, 0x60(a0)
constexpr uint32_t kLwV1 = 0x8C830060u;       // lw    v1, 0x60(a0)
constexpr uint32_t kBeqZero = 0x10000002u;    // b     +2 (beq zero, zero)
constexpr uint32_t kNop = 0x00000000u;

const char* kClampV0 =
    "if (g_psx_draw_distance_clamp && (int32_t)cpu->gpr[2] > 447) "
    "cpu->gpr[2] = (uint32_t)(447);";

PSXRecomp::CodeGenConfig with_site(uint32_t address, uint32_t expected,
                                   uint32_t reg, int32_t max) {
    PSXRecomp::CodeGenConfig config{};
    PSXRecompV4::DrawDistanceClampSite site;
    site.address = address;
    site.expected = expected;
    site.reg = reg;
    site.max = max;
    config.draw_distance_clamp_sites.push_back(site);
    return config;
}

void reads_classifier() {
    using PSXRecompV4::draw_distance_clamp_reads;
    check(draw_distance_clamp_reads(kSltiuT2V0, 2), "sltiu reads rs");
    check(!draw_distance_clamp_reads(kSltiuT2V0, 10), "sltiu does not read rt");
    check(draw_distance_clamp_reads(kAddiuAtV0, 2), "addiu reads rs");
    check(draw_distance_clamp_reads(kSravV0, 2) &&
              draw_distance_clamp_reads(kSravV0, 22),
          "srav reads rs and rt");
    check(draw_distance_clamp_reads(0x00021080u, 2), "sll reads rt");  // sll v0,v0,2
    check(draw_distance_clamp_reads(0x0062102Bu, 3), "sltu reads rs");  // sltu v0,v1,v0
    check(!draw_distance_clamp_reads(0x3C02800Bu, 2), "lui reads nothing");
    check(!draw_distance_clamp_reads(kLwV0, 4), "a load is not a clamp site");
    check(!draw_distance_clamp_reads(0x10400002u, 2), "a branch is not a clamp site");
    check(!draw_distance_clamp_reads(0x00430018u, 2), "mult is not a clamp site");
    check(!draw_distance_clamp_reads(0x03E00008u, 31), "jr is not a clamp site");
    check(!draw_distance_clamp_reads(kSltiuT2V0, 0), "reg 0 is refused");
}

void loader() {
    auto none = load(base_config());
    check(none.draw_distance_clamp_sites.empty(), "no clamps by default");

    auto two = load(base_config() +
                    clamp("0x80061230", "0x2C4A01C0", 2, "0x1BF") +
                    clamp("0x80066FF8", "0x2441FFFF", 2, "447"));
    check(two.draw_distance_clamp_sites.size() == 2, "two clamps parsed");
    if (two.draw_distance_clamp_sites.size() == 2) {
        const auto& a = two.draw_distance_clamp_sites[0];
        const auto& b = two.draw_distance_clamp_sites[1];
        check(a.address == 0x80061230u && a.expected == kSltiuT2V0 &&
                  a.reg == 2u && a.max == 0x1BF,
              "first clamp fields");
        check(b.address == 0x80066FF8u && b.expected == kAddiuAtV0 &&
                  b.max == 447,
              "second clamp fields (decimal max)");
    }
    auto neg = load(base_config() + clamp("0x80061230", "0x2C4A01C0", 2, "-5"));
    check(neg.draw_distance_clamp_sites.size() == 1 &&
              neg.draw_distance_clamp_sites[0].max == -5,
          "a negative max is accepted");

    check(load_throws_with(base_config() + clamp("0x80061232", "0x2C4A01C0", 2, "1"),
                           "not instruction-aligned"),
          "unaligned address refused");
    check(load_throws_with(base_config() +
                               clamp("0x80061230", "0x2C4A01C0", 2, "1") +
                               clamp("0xA0061230", "0x2C4A01C0", 2, "1"),
                           "duplicate draw-distance clamp"),
          "duplicate physical address refused");
    check(load_throws_with(base_config() + clamp("0x80061230", "0x2C4A01C0", 0, "1"),
                           "reg must be 1..31"),
          "reg 0 refused");
    check(load_throws_with(base_config() + clamp("0x80061230", "0x2C4A01C0", 32, "1"),
                           "reg must be 1..31"),
          "reg 32 refused");
    check(load_throws_with(base_config() + clamp("0x80061230", "0x2C4A01C0", 3, "1"),
                           "must be an ALU instruction that reads reg 3"),
          "a reg the instruction does not read is refused");
    check(load_throws_with(base_config() + clamp("0x80061230", "0x8C820060", 4, "1"),
                           "must be an ALU instruction"),
          "a load site is refused");
    check(load_throws_with(base_config() +
                               clamp("0x80061230", "0x2C4A01C0", 2, "0x100000000"),
                           "not a signed 32-bit value"),
          "max outside int32 refused");
}

void emission() {
    // The clamp runs immediately before the guard it protects.
    const auto cfg = with_site(kBase + 4u, kSltiuT2V0, 2, 447);
    const auto out = generate({kSravV0, kSltiuT2V0, kNop}, cfg).full_code;
    const size_t at = out.find(kClampV0);
    const size_t guard = out.find("0x80010004:");
    check(at != std::string::npos, "main-EXE site emits the clamp");
    check(at != std::string::npos && guard != std::string::npos && at < guard,
          "the clamp precedes the guard instruction");
    check(at != std::string::npos &&
              out.find(kClampV0, at + 1) == std::string::npos,
          "exactly one clamp emitted");
    check(at != std::string::npos && at >= 5 &&
              out.compare(at - 5, 5, "\n    ") == 0 &&
              (out.find("\n    cpu->gpr[10] =", at) != std::string::npos ||
               /* PGXP-aliasing form: the source is captured before the write. */
               out.find("\n    { uint32_t _pgx1 = cpu->gpr[2]; cpu->gpr[10] =", at) !=
                   std::string::npos),
          "the clamp and the guard share the statement indent");

    // The addiu form clamps the source the slot index is computed from.
    const auto addiu = generate({kSravV0, kAddiuAtV0, kNop},
                                with_site(kBase + 4u, kAddiuAtV0, 2, 447)).full_code;
    check(addiu.find(kClampV0) != std::string::npos,
          "addiu site emits the clamp on its source");

    // A site in a branch delay slot is clamped inside the slot.
    const auto ds = generate({kBeqZero, kSltiuT2V0, kNop, kNop},
                             with_site(kBase + 4u, kSltiuT2V0, 2, 447)).full_code;
    check(ds.find(kClampV0) != std::string::npos, "delay-slot site is clamped");

    // No sites: nothing about the clamp anywhere.
    PSXRecomp::CodeGenConfig plain{};
    const auto vanilla = generate({kSravV0, kSltiuT2V0, kNop}, plain).full_code;
    check(vanilla.find("draw_distance") == std::string::npos &&
              vanilla.find("draw-distance") == std::string::npos,
          "no sites: vanilla code");

    // A site elsewhere leaves this instruction alone.
    const auto other = generate({kSravV0, kSltiuT2V0, kNop},
                                with_site(kBase + 0x100u, kSltiuT2V0, 2, 447)).full_code;
    check(other.find("draw-distance") == std::string::npos,
          "unlisted address: vanilla code");

    // Captured overlays keep their own code at a listed address, matching
    // word or not.
    auto overlay = with_site(kBase + 4u, kSltiuT2V0, 2, 447);
    overlay.overlay_mode = true;
    check(generate({kSravV0, kSltiuT2V0, kNop}, overlay).full_code.find(
              "draw-distance") == std::string::npos,
          "overlay variant with the listed word stays vanilla");
    check(!exits_with_failure([&] { generate({kSravV0, kAddiuAtV0, kNop}, overlay); }),
          "overlay variant with another word generates");
}

void guards() {
    const auto cfg = with_site(kBase + 4u, kSltiuT2V0, 2, 447);
    check(!exits_with_failure([&] { generate({kSravV0, kSltiuT2V0, kNop}, cfg); }),
          "matching word generates");
    check(exits_with_failure([&] { generate({kSravV0, kAddiuAtV0, kNop}, cfg); }),
          "a main-EXE word mismatch fails generation");
    check(exits_with_failure([&] { generate({kLwV0, kSltiuT2V0, kNop}, cfg); }),
          "a site in the load-delay slot of its register fails generation");
    check(!exits_with_failure([&] { generate({kLwV1, kSltiuT2V0, kNop}, cfg); }),
          "a load into another register before the site is fine");
}

void shared_decls() {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    std::vector<PSXRecomp::GeneratedFunction> functions;
    {
        PSXRecomp::CodeGenerator plain(exe);
        check(plain.build_shared_decls_header(functions).find(
                  "g_psx_draw_distance_clamp") == std::string::npos,
              "no sites: the declarations do not change");
    }
    {
        const auto cfg = with_site(kBase + 4u, kSltiuT2V0, 2, 447);
        PSXRecomp::CodeGenerator gen(exe, cfg);
        check(gen.build_shared_decls_header(functions).find(
                  "extern uint32_t g_psx_draw_distance_clamp;") != std::string::npos,
              "sites: the switch is declared");
    }
}

void overlay_identity_unchanged() {
    auto none = load(base_config());
    auto some = load(base_config() + clamp("0x80061230", "0x2C4A01C0", 2, "0x1BF"));
    check(PSXRecompV4::overlay_codegen_config_hash(none) ==
              PSXRecompV4::overlay_codegen_config_hash(some),
          "clamps do not enter the overlay-cache identity");
}

void memory_addresses() {
    const uint32_t ops[] = {0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x28,0x29,0x2a,0x2b,0x2e,0x32,0x3a};
    for (bool overlay : {false, true}) {
        PSXRecomp::CodeGenConfig config{};
        config.overlay_mode = overlay;
        config.mod_instruction_sites.insert(kBase);
        for (uint32_t op : ops) {
            const uint32_t word = (op << 26) | (5u << 21) | (3u << 16) | 0xfffcu;
            const auto code = generate({word, 0u}, config).full_code;
            const auto callback = code.find("psx_mod_memory_address(cpu, 0x80010000u,");
            check(callback != std::string::npos, "configured memory opcode has an address adapter");
            check(code.find("psx_mod_memory_address(cpu,", callback + 1) == std::string::npos,
                  "guest access and PGXP evaluate the adapter only once");
            check(code.find("_pgxa = _mod_ea") != std::string::npos,
                  "PGXP and guest access share the redirected address");
            check(code.find("cpu->gpr[5] =") == std::string::npos,
                  "adaptation preserves the base register");
        }
        config.mod_instruction_sites.clear();
        config.mod_instruction_sites.insert(kBase + 4);
        check(generate({0x10000001u, 0x8ca30014u, 0u}, config).full_code.find(
                  "psx_mod_memory_address(cpu, 0x80010004u,") != std::string::npos,
              "branch delay-slot memory access is adapted");
        check(generate({0u, 0x88a00000u, 0u}, config).full_code.find(
                  "(void)psx_lwl(cpu, _mod_ea") != std::string::npos,
              "unaligned load to zero still performs its access");
    }
    PSXRecomp::CodeGenConfig plain{};
    check(generate({0x8ca30014u, 0u}, plain).full_code.find("psx_mod_memory_address(cpu,") == std::string::npos,
          "unconfigured native memory access has no adapter call");
}

void runtime_value() {
    check(psx_draw_distance_clamp_value(0x1C0u, 0x1BF) == 0x1BFu, "clamps past max");
    check(psx_draw_distance_clamp_value(0x7FFu, 0x1BF) == 0x1BFu, "clamps far past max");
    check(psx_draw_distance_clamp_value(0x1BFu, 0x1BF) == 0x1BFu, "max is kept");
    check(psx_draw_distance_clamp_value(5u, 0x1BF) == 5u, "near values unchanged");
    check(psx_draw_distance_clamp_value(0u, 0x1BF) == 0u, "zero unchanged");
    check(psx_draw_distance_clamp_value(0xFFFFFFFFu, 0x1BE) == 0xFFFFFFFFu,
          "a negative value is left for the original predicate");
}

}  // namespace

int main(int argc, char** argv) {
    g_self = argv[0];
    if (argc >= 3 && std::string(argv[1]) == "--exit-probe")
        g_exit_probe = std::atoi(argv[2]);
    reads_classifier();
    loader();
    emission();
    memory_addresses();
    guards();
    shared_decls();
    overlay_identity_unchanged();
    runtime_value();
    if (failures) {
        std::fprintf(stderr, "draw_distance_codegen_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("draw_distance_codegen_test: OK\n");
    return 0;
}
