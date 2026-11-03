// Game/overlay emitter: uncached (KSEG1) code charges an I-cache fetch before
// every instruction; cached code keeps the line-leader rule unchanged.
//
// Beetle's ReadInstruction (the model psx_icache.c transcribes) never fills a
// line for a fetch at 0xA0000000 or above, so every such fetch costs +4 and
// clears the load give-back. The dirty-RAM interpreter reaches the same charge
// by fetching at every PC. The emitter used to emit the fetch only at block
// leaders and 16-byte line starts, which is exact only for cached code.
//
// The same synthetic function is generated at KSEG0 and at KSEG1. Its entry
// starts mid-line and it has a loop (a mid-line branch target), a load, a
// branch with a delay slot and a jr/delay-slot return. The emitted fetch tags,
// in emission order, must be:
//   KSEG1: every instruction PC, once each, in execution-layout order;
//   KSEG0: exactly the leader-rule set (entry, line starts, block leaders).
// The fetch sequences' cycle cost is checked against psx_icache.c, the
// interpreter's fetch path and a Beetle transcription by
// test_uncached_fetch_charge.py (ctest uncached_fetch_charge).

#include "code_generator.h"
#include "control_flow.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++failures;
    }
}

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

// Offsets from the entry, which sits at +8 inside a 16-byte line.
const uint32_t kProgram[] = {
    0x24080003u,  // +00 addiu $t0, $zero, 3
    0x8C890000u,  // +04 lw    $t1, 0($a0)
    0x01285021u,  // +08 addu  $t2, $t1, $t0        (line start)
    0x2508FFFFu,  // +0C addiu $t0, $t0, -1         (loop top: mid-line leader)
    0x1500FFFEu,  // +10 bne   $t0, $zero, +0C
    0x01495021u,  // +14 addu  $t2, $t2, $t1        (delay slot)
    0x00000000u,  // +18 nop                        (line start, leader after the loop)
    0x03E00008u,  // +1C jr    $ra
    0xAC8A0004u,  // +20 sw    $t2, 4($a0)          (delay slot)
};
constexpr uint32_t kCount = sizeof(kProgram) / sizeof(kProgram[0]);
constexpr uint32_t kEntryPhys = 0x00010008u;

std::string generate_at(uint32_t segment) {
    const uint32_t base = segment | kEntryPhys;
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = base;
    exe.header.initial_pc = base;
    exe.header.file_size = kCount * 4u;
    for (uint32_t w : kProgram) append_word(exe.code_data, w);

    PSXRecomp::Function function{};
    function.start_addr = base;
    function.end_addr = base + kCount * 4u;
    function.size = kCount * 4u;
    function.name = "uncached_fetch_probe";

    PSXRecomp::CodeGenConfig config{};
    config.emit_comments = true;
    config.indent = "    ";
    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    const auto cfg = analyzer.analyze_function(function);
    PSXRecomp::CodeGenerator generator(exe, config);
    return generator.generate_function(function, cfg).full_code;
}

std::vector<uint32_t> fetch_tags(const std::string& code) {
    static const std::string needle = "psx_icache_fetch(cpu, 0x";
    std::vector<uint32_t> tags;
    for (size_t pos = code.find(needle); pos != std::string::npos;
         pos = code.find(needle, pos + 1)) {
        tags.push_back(static_cast<uint32_t>(
            std::strtoul(code.c_str() + pos + needle.size(), nullptr, 16)));
    }
    return tags;
}

std::string hex_list(const std::vector<uint32_t>& v) {
    std::string s;
    char buf[16];
    for (uint32_t x : v) {
        std::snprintf(buf, sizeof buf, "%s%08X", s.empty() ? "" : " ", x);
        s += buf;
    }
    return s;
}

void check_segment(const char* name, uint32_t segment,
                   const std::vector<uint32_t>& want_offsets) {
    const std::string code = generate_at(segment);
    const uint32_t base = segment | kEntryPhys;
    std::vector<uint32_t> want;
    for (uint32_t off : want_offsets) want.push_back(base + off);
    const std::vector<uint32_t> got = fetch_tags(code);
    const char* native_charge = segment == 0xA0000000u
        ? "psx_native_span_charge(17u)" : "psx_native_span_charge(5u)";
    check(code.find(native_charge) != std::string::npos,
          std::string(name) + " native entry span includes its load and address policy");
    check(got == want, std::string(name) + " fetch tags: got [" + hex_list(got) +
                           "], want [" + hex_list(want) + "]");
}

}  // namespace

int main() {
    // PSX_CODEGEN_CYCLE_PER_INSN=0 (block-up-front mode) emits no fetches at
    // all; this test is about the default per-instruction mode.
    const char* mode = std::getenv("PSX_CODEGEN_CYCLE_PER_INSN");
    if (mode != nullptr && mode[0] == '0') {
        std::puts("SKIP: PSX_CODEGEN_CYCLE_PER_INSN=0 emits no fetch charges");
        return 0;
    }

    std::vector<uint32_t> every;
    for (uint32_t i = 0; i < kCount; ++i) every.push_back(i * 4u);
    check_segment("KSEG1", 0xA0000000u, every);
    // Entry (+00), line start (+08), loop leader (+0C), line start and
    // post-loop leader (+18). Intra-line followers are guaranteed hits.
    check_segment("KSEG0", 0x80000000u, {0x00u, 0x08u, 0x0Cu, 0x18u});

    if (failures != 0) {
        std::fprintf(stderr, "uncached_fetch_codegen_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("PASS: uncached fetch codegen (KSEG1 every instruction, KSEG0 leader rule)");
    return 0;
}
