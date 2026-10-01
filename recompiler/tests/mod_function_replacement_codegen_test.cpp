#include "code_generator.h"
#include "control_flow.h"
#include <cstdio>
#include <cstdlib>

static void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    constexpr uint32_t base = 0x80010000u;
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = exe.header.initial_pc = base;
    const uint32_t words[] = {0x24020001u, 0x10400002u, 0, 0x24030002u, 0x03E00008u, 0};
    for (uint32_t word : words)
        for (unsigned byte = 0; byte < 4; ++byte)
            exe.code_data.push_back(static_cast<uint8_t>(word >> (byte * 8)));
    exe.header.file_size = static_cast<uint32_t>(exe.code_data.size());
    PSXRecomp::Function function{};
    function.start_addr = base;
    function.size = exe.header.file_size;
    function.end_addr = base + function.size;
    function.name = "replacement_test";
    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto flow = analyzer.analyze_function(function);
    PSXRecomp::CodeGenConfig config{};
    config.indent = "    ";
    const auto generate = [&]() {
        PSXRecomp::CodeGenerator generator(exe, config);
        return generator.generate_function(function, flow).full_code;
    };
    const std::string hook = "if (psx_mod_try_function_replacement(cpu, 0x80010000u)) return;";
    check(generate().find("psx_mod_try_function_replacement") == std::string::npos,
          "default configuration emits no replacement call");
    config.mod_function_entry_funcs.insert(base + 4);
    check(generate().find("psx_mod_try_function_replacement") == std::string::npos,
          "listing a different address does not hook this function");
    config.mod_function_entry_funcs.insert(base);
    const auto code = generate();
    const auto at = code.find(hook);
    check(at != std::string::npos && code.find(hook, at + 1) == std::string::npos,
          "explicit function entry emits exactly one replacement call");
    check(code.find("psx_mod_function_entry(cpu, 0x80010000u);") < at,
          "existing preparation callback runs before replacement");
    check(at < code.find("block_80010000:"), "fallback body remains after replacement");
    const auto continuation = code.find("switch (_cont)");
    check(continuation != std::string::npos && continuation < at,
          "interior CPS resumes bypass fresh-entry replacement");
    PSXRecomp::CodeGenerator generator(exe, config);
    check(generator.build_shared_decls_header({}).find(
              "extern int psx_mod_try_function_replacement(CPUState* cpu, uint32_t address);") !=
              std::string::npos,
          "generated shards declare runtime replacement ABI");
    std::puts("PASS default-off, exact-entry, fallback and continuation-safe replacement codegen");
}
