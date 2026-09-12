#include "code_generator.h"
#include "config_loader.h"
#include "control_flow.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
name = "Signed Bound Test"
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

bool load_throws_with(const std::string& body, const char* needle) {
    fs::path path = write_temp_config("signed-x-bound", body);
    bool matched = false;
    try {
        (void)PSXRecompV4::load_game_config(path);
    } catch (const std::exception& e) {
        matched = std::string(e.what()).find(needle) != std::string::npos;
    }
    fs::remove(path);
    return matched;
}

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
    function.name = "signed_bound_test";

    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg);
}

void loader_accepts_lui_and_addiu() {
    fs::path path = write_temp_config("signed-x-bound-valid", base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x3C020001"

[[widescreen.signed_x_bound]]
address = "0x80010004"
expected = "0x2402FF00"
)toml");
    auto config = PSXRecompV4::load_game_config(path);
    check(config.ws_signed_x_bound_sites.size() == 2,
          "loader accepts LUI plus ADDIU signed_x_bound entries");
    fs::remove(path);
}

void loader_rejects_bad_addiu_shapes() {
    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x30020100"
)toml", "expected must be LUI or ADDIU"),
          "loader rejects unsupported signed_x_bound opcode");

    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x20020100"
)toml", "expected must be LUI or ADDIU"),
          "loader rejects ADDI without a matching codegen/interpreter route");

    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x2482FF00"
)toml", "must use rs=$zero"),
          "loader rejects ADDIU signed_x_bound with nonzero rs");

    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x2400FF00"
)toml", "must not write $zero"),
          "loader rejects ADDIU signed_x_bound writing zero");

    check(load_throws_with(base_config() + R"toml(
[[widescreen.signed_x_bound]]
address = "0x80010000"
expected = "0x24020000"
)toml", "non-zero signed screen edge"),
          "loader rejects ADDIU signed_x_bound with zero immediate");
}

void codegen_routes_bound_kinds() {
    PSXRecomp::CodeGenConfig config{};
    config.emit_comments = true;
    config.ws_signed_x_bound_sites.push_back(
        PSXRecompV4::WidescreenSignedBoundSite{kBase, 0x2402FF00u});
    auto addiu_neg = generate_first_instruction(0x2402FF00u, config).full_code;
    check(addiu_neg.find(
              "cpu->gpr[2] = (uint32_t)psx_ws_screen_x_bound(-256);") !=
              std::string::npos,
          "codegen routes ADDIU negative screen bound to screen helper");

    config.ws_signed_x_bound_sites[0] =
        PSXRecompV4::WidescreenSignedBoundSite{kBase, 0x24020100u};
    auto addiu_pos = generate_first_instruction(0x24020100u, config).full_code;
    check(addiu_pos.find(
              "cpu->gpr[2] = (uint32_t)psx_ws_screen_x_bound(256);") !=
              std::string::npos,
          "codegen routes ADDIU positive screen bound to screen helper");

    config.ws_signed_x_bound_sites[0] =
        PSXRecompV4::WidescreenSignedBoundSite{kBase, 0x3402FF00u};
    auto ori = generate_first_instruction(0x3402FF00u, config).full_code;
    check(ori.find("psx_ws_screen_x_bound(65280)") != std::string::npos,
          "ORI bound zero-extends, unlike the ADDIU negative bound");

    config.ws_signed_x_bound_sites[0] =
        PSXRecompV4::WidescreenSignedBoundSite{kBase, 0x3C020001u};
    auto lui = generate_first_instruction(0x3C020001u, config).full_code;
    check(lui.find(
              "cpu->gpr[2] = (uint32_t)psx_ws_player_x_bound((int32_t)0x00010000);") !=
              std::string::npos,
          "codegen preserves LUI signed-Q16 gameplay helper");
}

void shared_decls_include_screen_helper() {
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = kBase;
    PSXRecomp::CodeGenerator generator(exe);
    std::vector<PSXRecomp::GeneratedFunction> functions;
    const auto decls = generator.build_shared_decls_header(functions);
    check(decls.find("psx_ws_screen_x_bound") != std::string::npos,
          "shared declarations include psx_ws_screen_x_bound");
}

}  // namespace

int main() {
    loader_accepts_lui_and_addiu();
    loader_rejects_bad_addiu_shapes();
    codegen_routes_bound_kinds();
    shared_decls_include_screen_helper();

    if (failures != 0) {
        std::fprintf(stderr, "signed_x_bound_codegen_test: %d failure(s)\n",
                     failures);
        return 1;
    }
    std::puts("PASS: signed_x_bound codegen");
    return 0;
}
