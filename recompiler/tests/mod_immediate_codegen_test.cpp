// [[mod_immediate.sites]]: config parsing and validation, the per-selector
// switch emitted for a listed instruction (ALU immediate, load, store, branch
// delay slot, deferred load), main-EXE vs overlay guard behaviour, the shared
// declaration and the overlay-cache identity.
#include "code_generator.h"
#include "config_loader.h"
#include "control_flow.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#if defined(_WIN32)
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
    for (int i = 0; i < 4; i++) bytes.push_back(static_cast<uint8_t>(word >> (8 * i)));
}

size_t count_of(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}

fs::path write_temp_config(const char* stem, const std::string& body) {
    const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    fs::path path = fs::temp_directory_path() /
                    (std::string(stem) + "-" + std::to_string(nonce) + ".toml");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << body;
    return path;
}

std::string base_config() {
    return R"toml([game]
name = "Mod Immediate Test"
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

std::string sites_block(std::initializer_list<std::string> entries) {
    std::string out = "\n[mod_immediate]\nsites = [\n";
    for (const auto& e : entries) out += "  " + e + ",\n";
    return out + "]\n";
}

std::string site(const char* address, const char* expected, int group,
                 const char* values) {
    return std::string("{ address = \"") + address + "\", expected = \"" + expected +
           "\", group = " + std::to_string(group) + ", values = [" + values + "] }";
}

PSXRecompV4::GameConfig load(const std::string& body) {
    fs::path path = write_temp_config("mod-immediate", body);
    auto config = PSXRecompV4::load_game_config(path);
    fs::remove(path);
    return config;
}

bool load_throws_with(const std::string& body, const char* needle) {
    fs::path path = write_temp_config("mod-immediate-bad", body);
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
    function.name = "imm_site_test";
    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg);
}

// Windows has no fork: the Nth probe re-runs this binary as `--exit-probe N`.
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
    const intptr_t rc = _spawnl(_P_WAIT, g_self, quoted.c_str(), "--exit-probe", n.c_str(), nullptr);
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

constexpr uint32_t kSltiV0V0_32 = 0x28420020u;   // slti  v0, v0, 0x20
constexpr uint32_t kAddiuA0V1 = 0x246402E9u;     // addiu a0, v1, 0x2e9
constexpr uint32_t kLbuV0V1 = 0x906202E9u;       // lbu   v0, 0x2e9(v1)
constexpr uint32_t kSbA1V1 = 0xA0650308u;        // sb    a1, 0x308(v1)
constexpr uint32_t kUseV0 = 0x00423021u;         // addu  a2, v0, v0 (reads v0, writes another)
constexpr uint32_t kBeqZero = 0x10000002u;       // b +2
constexpr uint32_t kNop = 0x00000000u;

PSXRecomp::CodeGenConfig with_sites(std::vector<PSXRecompV4::ModImmediateSite> sites,
                                    bool overlay = false) {
    PSXRecomp::CodeGenConfig config{};
    config.mod_immediate_sites = std::move(sites);
    config.overlay_mode = overlay;
    return config;
}

PSXRecompV4::ModImmediateSite make(uint32_t address, uint32_t expected, uint32_t group,
                                   std::vector<uint16_t> values) {
    PSXRecompV4::ModImmediateSite s;
    s.address = address;
    s.expected = expected;
    s.group = group;
    s.values = std::move(values);
    return s;
}

void loader() {
    check(load(base_config()).mod_immediate_sites.empty(), "no sites by default");

    const auto cfg = load(base_config() + sites_block({
        site("0x80023500", "0x28820020", 1, "64, 128"),
        site("0x8002336C", "0xA0650308", 1, "0x328, 0x368"),
        site("0x80023500", "0x28820021", 2, "-1"),   // same address, other overlay word
    }));
    check(cfg.mod_immediate_sites.size() == 3, "three sites parsed");
    if (cfg.mod_immediate_sites.size() == 3) {
        const auto& a = cfg.mod_immediate_sites[0];
        check(a.address == 0x80023500u && a.expected == 0x28820020u && a.group == 1u &&
                  a.values.size() == 2 && a.values[0] == 64 && a.values[1] == 128,
              "first site fields");
        check(cfg.mod_immediate_sites[1].values[1] == 0x368, "hex values");
        check(cfg.mod_immediate_sites[2].values[0] == 0xFFFF, "negative value wraps to imm16");
    }

    check(load_throws_with(base_config() + sites_block({site("0x80023502", "0x28820020", 1, "64")}),
                           "not instruction-aligned"), "unaligned address refused");
    check(load_throws_with(base_config() + sites_block({site("0x80023500", "0x10400002", 1, "64")}),
                           "not an I-type"), "a branch is refused");
    check(load_throws_with(base_config() + sites_block({site("0x80023500", "0x0043102B", 1, "64")}),
                           "not an I-type"), "an R-type is refused");
    check(load_throws_with(base_config() + sites_block({site("0x80023500", "0x28820020", 64, "64")}),
                           "group must be 0..63"), "group 64 refused");
    check(load_throws_with(base_config() + sites_block({site("0x80023500", "0x28820020", 1, "")}),
                           "at least one"), "empty values refused");
    check(load_throws_with(base_config() + sites_block({site("0x80023500", "0x28820020", 1, "70000")}),
                           "does not fit 16 bits"), "value out of range refused");
    check(load_throws_with(base_config() + sites_block({
                               site("0x80023500", "0x28820020", 1, "64"),
                               site("0xA0023500", "0x28820020", 1, "128")}),
                           "duplicate mod_immediate"), "duplicate (address, word) refused");
    check(load_throws_with(base_config() + sites_block({site("0x80800000", "0x28820020", 1, "64")}),
                           "not main RAM"), "address past main RAM refused");
}

void emission() {
    // slti: one case per value plus the stock default, each with its own immediate.
    const auto out = generate({kNop, kSltiV0V0_32, kNop},
        with_sites({make(kBase + 4u, kSltiV0V0_32, 1, {64, 128})})).full_code;
    check(out.find("switch (psx_mod_immediate_select(1u))") != std::string::npos,
          "switch on the group selector");
    check(count_of(out, "case 1u:") == 1 && count_of(out, "case 2u:") == 1 &&
              count_of(out, "default:") >= 1,
          "one case per value plus default");
    check(out.find("< 64") != std::string::npos && out.find("< 128") != std::string::npos &&
              out.find("< 32") != std::string::npos,
          "each variant compares against its own immediate");

    // lbu: offsets differ per variant; deferred loads rewrite every copy.
    const auto load_out = generate({kNop, kLbuV0V1, kUseV0, kNop},
        with_sites({make(kBase + 4u, kLbuV0V1, 3, {0x2E9 + 0x100, 0x2E9 - 0x100})})).full_code;
    check(count_of(load_out, "switch (psx_mod_immediate_select(3u))") == 1, "load site switched");
    check(load_out.find("1001") != std::string::npos,
          "relocated load offset emitted (0x2E9+0x100 = 1001)");
    check(count_of(load_out, "psx_ldd_80010004") >= 4,
          "a deferred load writes the temporary in every selector copy");
    check(load_out.find("cpu->gpr[2] = psx_ldd_80010004;") != std::string::npos,
          "the deferred load is still written back");

    // sb store + addiu pointer formation.
    const auto st = generate({kSbA1V1, kAddiuA0V1, kNop},
        with_sites({make(kBase, kSbA1V1, 4, {0x328}),
                    make(kBase + 4u, kAddiuA0V1, 4, {0x400})})).full_code;
    check(count_of(st, "switch (psx_mod_immediate_select(4u))") == 2, "store and addiu switched");

    // A site in a branch delay slot.
    const auto ds = generate({kBeqZero, kSltiV0V0_32, kNop, kNop},
        with_sites({make(kBase + 4u, kSltiV0V0_32, 1, {64})})).full_code;
    check(ds.find("switch (psx_mod_immediate_select(1u))") != std::string::npos,
          "delay-slot site is switched");

    // No sites: vanilla.
    PSXRecomp::CodeGenConfig plain{};
    const auto vanilla = generate({kNop, kSltiV0V0_32, kNop}, plain).full_code;
    check(vanilla.find("psx_mod_immediate_select") == std::string::npos, "no sites: vanilla code");

    // A site elsewhere leaves this instruction alone.
    const auto other = generate({kNop, kSltiV0V0_32, kNop},
        with_sites({make(kBase + 0x100u, kSltiV0V0_32, 1, {64})})).full_code;
    check(other.find("psx_mod_immediate_select") == std::string::npos, "unlisted address: vanilla");

    // Overlay mode: a different word at a listed address stays vanilla.
    const auto ov = generate({kNop, kLbuV0V1, kNop},
        with_sites({make(kBase + 4u, kSltiV0V0_32, 1, {64})}, true)).full_code;
    check(ov.find("psx_mod_immediate_select") == std::string::npos,
          "overlay word mismatch stays vanilla");
    const auto ov_match = generate({kNop, kSltiV0V0_32, kNop},
        with_sites({make(kBase + 4u, kSltiV0V0_32, 1, {64})}, true)).full_code;
    check(ov_match.find("psx_mod_immediate_select(1u)") != std::string::npos,
          "overlay word match is switched");
}

void guards() {
    const auto cfg = with_sites({make(kBase + 4u, kSltiV0V0_32, 1, {64})});
    check(!exits_with_failure([&] { generate({kNop, kSltiV0V0_32, kNop}, cfg); }),
          "matching word generates");
    check(exits_with_failure([&] { generate({kNop, kLbuV0V1, kNop}, cfg); }),
          "a main-EXE word mismatch fails generation");
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
            check(code.find("psx_mod_memory_address(cpu, 0x80010000u,") != std::string::npos,
                  "configured load/store computes one guarded address");
            check(count_of(code, "psx_mod_memory_address(cpu,") == 1,
                  "address callback is evaluated once, including PGXP");
            check(code.find("_pgxa = _mod_ea") != std::string::npos,
                  "PGXP and guest access share the redirected address");
            check(code.find("cpu->gpr[5] =") == std::string::npos,
                  "address adaptation never overwrites the base register");
        }
        config.mod_instruction_sites.clear(); config.mod_instruction_sites.insert(kBase + 4);
        const auto delay = generate({0x10000001u, 0x8ca30014u, 0u}, config).full_code;
        check(delay.find("psx_mod_memory_address(cpu, 0x80010004u,") != std::string::npos,
              "branch delay-slot memory access is adapted");
        const auto zero = generate({0u, 0x88a00000u, 0u}, config).full_code;
        check(zero.find("(void)psx_lwl(cpu, _mod_ea") != std::string::npos,
              "unaligned load to zero still performs the access");
    }
    PSXRecomp::CodeGenConfig plain{};
    check(generate({0x8ca30014u, 0u}, plain).full_code.find("psx_mod_memory_address(cpu,") == std::string::npos,
          "unconfigured load has no address adapter cost");
    auto both = with_sites({make(kBase, 0x8ca30014u, 1, {0x18})});
    both.mod_instruction_sites.insert(kBase);
    const auto code = generate({0x8ca30014u, 0u}, both).full_code;
    check(count_of(code, "0x8CA30014u, (uint32_t)(cpu->gpr[5] +") == 2,
          "immediate variants retain the original instruction guard");
}

void shared_decls() {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    std::vector<PSXRecomp::GeneratedFunction> functions;
    {
        PSXRecomp::CodeGenerator plain(exe);
        check(plain.build_shared_decls_header(functions).find("psx_mod_immediate_select") ==
                  std::string::npos, "no sites: the declarations do not change");
    }
    {
        const auto cfg = with_sites({make(kBase + 4u, kSltiV0V0_32, 1, {64})});
        PSXRecomp::CodeGenerator gen(exe, cfg);
        check(gen.build_shared_decls_header(functions).find(
                  "extern uint32_t psx_mod_immediate_select(uint32_t group);") != std::string::npos,
              "sites: the selector is declared");
    }
}

void overlay_identity() {
    auto none = load(base_config());
    auto some = load(base_config() + sites_block({site("0x80023500", "0x28820020", 1, "64")}));
    auto other = load(base_config() + sites_block({site("0x80023500", "0x28820020", 1, "65")}));
    auto ordered = load(base_config() + sites_block({
        site("0x80023500", "0x28820020", 1, "64"), site("0x80023504", "0x28820020", 1, "64")}));
    auto reversed = load(base_config() + sites_block({
        site("0x80023504", "0x28820020", 1, "64"), site("0x80023500", "0x28820020", 1, "64")}));
    using PSXRecompV4::overlay_codegen_config_hash;
    check(overlay_codegen_config_hash(none) != overlay_codegen_config_hash(some),
          "sites enter the overlay-cache identity");
    check(overlay_codegen_config_hash(some) != overlay_codegen_config_hash(other),
          "a changed value changes the identity");
    check(overlay_codegen_config_hash(ordered) == overlay_codegen_config_hash(reversed),
          "site order does not");
}

}  // namespace

int main(int argc, char** argv) {
    g_self = argv[0];
    if (argc >= 3 && std::string(argv[1]) == "--exit-probe") g_exit_probe = std::atoi(argv[2]);
    loader();
    emission();
    memory_addresses();
    guards();
    shared_decls();
    overlay_identity();
    if (failures) {
        std::fprintf(stderr, "mod_immediate_codegen_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("mod_immediate_codegen_test: OK\n");
    return 0;
}
