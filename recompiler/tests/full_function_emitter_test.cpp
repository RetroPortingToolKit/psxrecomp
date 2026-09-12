#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include "../src/bios_address_model.h"
#include "../src/config_loader.h"
#include "../src/full_function_emitter.h"
#include "../src/function_discovery.h"

using PSXRecompV4::BiosAddressModel;
using PSXRecompV4::BiosAddrCopy;
using PSXRecompV4::BiosConfig;
using PSXRecompV4::DiscoveredFunction;
using PSXRecompV4::DiscoveryResult;
using PSXRecompV4::EmitStats;
using PSXRecompV4::FullFunctionEmitter;
using PSXRecompV4::FunctionDiscovery;

namespace {

constexpr uint32_t kBase = 0xBFC00000u;
int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void append_word(std::vector<uint8_t>& rom, uint32_t word) {
    rom.push_back(static_cast<uint8_t>(word));
    rom.push_back(static_cast<uint8_t>(word >> 8));
    rom.push_back(static_cast<uint8_t>(word >> 16));
    rom.push_back(static_cast<uint8_t>(word >> 24));
}

DiscoveredFunction function_at(uint32_t entry, uint32_t end,
                               std::initializer_list<uint32_t> leaders) {
    DiscoveredFunction fn{};
    fn.entry_addr = entry;
    fn.normalized_addr = entry & 0x1FFFFFFFu;
    fn.end_addr = end;
    fn.instruction_count = (end - entry) / 4u + 1u;
    fn.termination_reason = "jr_ra";
    fn.discovered_by = "synthetic test";
    fn.block_leaders.assign(leaders.begin(), leaders.end());
    return fn;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

struct RunResult {
    EmitStats stats;
    std::string dispatch;
    std::string full;
};

RunResult run_case(const char* name, const std::vector<uint32_t>& words,
                   std::vector<DiscoveredFunction> functions) {
    std::vector<uint8_t> rom;
    for (uint32_t word : words) append_word(rom, word);

    DiscoveryResult discovery{};
    discovery.ok = true;
    discovery.functions = std::move(functions);

    const auto nonce = std::chrono::high_resolution_clock::now()
                           .time_since_epoch().count();
    const auto out_dir = std::filesystem::temp_directory_path() /
                         ("psxrecomp-full-emitter-" + std::string(name) + "-" +
                          std::to_string(nonce));
    std::filesystem::create_directories(out_dir);

    const std::string stem = "Test";
    RunResult result;
    result.stats = FullFunctionEmitter::emit(
        rom, kBase, kBase + static_cast<uint32_t>(rom.size()) - 1u,
        discovery, "synthetic", out_dir.string(), stem);
    result.dispatch = read_file(out_dir / (stem + "_dispatch.c"));
    result.full = read_file(out_dir / (stem + "_full.c"));
    std::filesystem::remove_all(out_dir);
    return result;
}

void expect_entry_absent(const RunResult& result, uint32_t normalized,
                         const char* message) {
    char needle[96];
    std::snprintf(needle, sizeof(needle),
                  "{ 0x%08Xu, Test_func_%08X }", normalized, normalized);
    expect(result.dispatch.find(needle) == std::string::npos, message);
}

void expect_dispatch_key_absent(const RunResult& result, uint32_t normalized,
                                const char* message) {
    char needle[32];
    std::snprintf(needle, sizeof(needle), "{ 0x%08Xu,", normalized);
    const auto begin = result.dispatch.find("static const DispatchEntry dispatch_table[");
    const auto end = result.dispatch.find("};", begin);
    expect(begin != std::string::npos &&
           result.dispatch.substr(begin, end - begin).find(needle) == std::string::npos, message);
}

void delay_slot_load_falls_back() {
    const auto result = run_case(
        "delay-slot",
        {
            0x10000001u,  // beq zero,zero,+1
            0x8D280000u,  // lw t0,0(t1) -- branch delay slot
            0x01001021u,  // addu v0,t0,zero -- dependent successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 16u, {kBase, kBase + 8u})});
    expect(result.stats.functions_interpreted == 1,
           "delay-slot load marks the function for interpretation");
    expect(result.stats.functions_emitted == 0,
           "delay-slot load function is not emitted");
    expect_entry_absent(result, 0x00000500u,
                        "delay-slot load function is absent from dispatch");
    expect_dispatch_key_absent(result, 0x00000508u,
                               "delay-slot successor continuation is absent from dispatch");
}

void label_split_load_falls_back() {
    const auto result = run_case(
        "label-split",
        {
            0x8D280000u,  // lw t0,0(t1)
            0x01001021u,  // addu v0,t0,zero -- dependent labeled successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase, kBase + 4u})});
    expect(result.stats.functions_interpreted == 1,
           "label-split load marks the function for interpretation");
    expect_entry_absent(result, 0x00000500u,
                        "label-split function is absent from dispatch");
    expect_dispatch_key_absent(result, 0x00000504u,
                               "label-split continuation is absent from dispatch");
}

void fragment_split_load_falls_back() {
    const auto result = run_case(
        "fragment-split",
        {
            0x8D280000u,  // fragment 1: lw t0,0(t1)
            0x01001021u,  // fragment 2: dependent successor
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {
            function_at(kBase, kBase, {kBase}),
            function_at(kBase + 4u, kBase + 12u, {kBase + 4u}),
        });
    expect(result.stats.functions_interpreted == 1,
           "fragment-split load marks only its owning function for interpretation");
    expect(result.stats.functions_emitted == 1,
           "unaffected neighboring fragment remains native");
    expect_entry_absent(result, 0x00000500u,
                        "fragment-split load owner is absent from dispatch");
}

void noncomplementary_lwl_falls_back() {
    const auto result = run_case(
        "lwl-dependent",
        {
            0x89280000u,  // lwl t0,0(t1)
            0x01001021u,  // addu v0,t0,zero -- dependent, not lwr
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase})});
    expect(result.stats.functions_interpreted == 1,
           "non-complementary LWL dependency falls back");
    expect_entry_absent(result, 0x00000500u,
                        "non-complementary LWL function is absent from dispatch");
}

void complementary_lwl_lwr_stays_native() {
    const auto result = run_case(
        "lwl-lwr",
        {
            0x89280000u,  // lwl t0,0(t1)
            0x99280003u,  // lwr t0,3(t1) -- architectural forwarding pair
            0x03E00008u,  // jr ra
            0x00000000u,  // nop
        },
        {function_at(kBase, kBase + 12u, {kBase})});
    expect(result.stats.functions_interpreted == 0,
           "complementary LWL/LWR does not fall back");
    expect(result.stats.functions_emitted == 1,
           "complementary LWL/LWR remains native");
}

}  // namespace

void patch_range_guards(BiosConfig config) {
    PSXRecompV4::BiosInstallSlot slot;
    slot.ram_addr = 0x508u;
    slot.len = 8u;
    slot.resume = PSXRecompV4::BiosInstallSlot::Resume::Fallthrough;
    config.install_slots.push_back(slot);
    config.address_copies[0].kernel_bless = true;
    const auto model = BiosAddressModel::from_config(config);
    FullFunctionEmitter::set_address_model(&model);
    const auto result = run_case("patch-terminator", {
        0x10000002u, // branch directly into the range interior at +12
        0x00000000u,
        0x03E00008u, // jr ra at the range START must also be guarded
        0x00000000u,
        0x03E00008u,
        0x00000000u,
    }, {function_at(kBase, kBase + 20u, {kBase, kBase + 8u, kBase + 12u})});
    const auto start = result.full.find("label_BFC00008:");
    const auto interior = result.full.find("label_BFC0000C:");
    expect(start != std::string::npos &&
           result.full.find("kernel patch-range hook", start) < interior,
           "a terminator at range start is guarded");
    expect(interior != std::string::npos &&
           result.full.find("kernel patch-range hook", interior) != std::string::npos,
           "a branch into a range interior cannot bypass the guard");
    const auto guard = result.full.find("kernel patch-range hook", start);
    expect(guard < result.full.find("psx_cyc_step", start),
           "patched instructions are not charged before interpreter handoff");
    expect_dispatch_key_absent(result, 0x508u, "range start not externally dispatched");
    expect_dispatch_key_absent(result, 0x50Cu, "range interior not externally dispatched");
}

void patch_range_delay_boundaries(BiosConfig config) {
    PSXRecompV4::BiosInstallSlot slot;
    slot.ram_addr = 0x504u;
    slot.len = 4u;
    slot.resume = PSXRecompV4::BiosInstallSlot::Resume::Fallthrough;
    config.install_slots.push_back(slot);
    config.address_copies[0].kernel_bless = true;
    const auto model = BiosAddressModel::from_config(config);
    FullFunctionEmitter::set_address_model(&model);
    for (uint32_t predecessor : {0x10000001u, 0x8D280000u}) {
        const auto result = run_case("patch-delay-boundary", {
            predecessor, // branch or CPU load immediately before the range
            0x00000000u,
            0x03E00008u,
            0x00000000u,
        }, {function_at(kBase, kBase + 12u, {kBase})});
        expect(result.stats.functions_interpreted == 1,
               "a patch guard cannot discard a pending branch/load delay");
        expect(result.stats.functions_emitted == 0,
               "an unsafe patch boundary is fail-closed");
        expect_entry_absent(result, 0x500u, "unsafe patch owner not dispatched");
    }
}

int main() {
    BiosConfig config{};
    config.config_path = "test://full-function-emitter";
    config.load_address = kBase;
    config.text_size = 0x1000u;
    BiosAddrCopy copy;
    copy.name = "synthetic RAM-backed BIOS code";
    copy.rom_lo = 0x1FC00000u;
    copy.rom_hi = 0x1FC01000u;
    copy.ram_lo = 0x00000500u;
    copy.runtime_base = 0x00000500u;
    copy.key_is_ram = true;
    config.address_copies.push_back(copy);
    BiosAddressModel model = BiosAddressModel::from_config(config);
    FunctionDiscovery::set_address_model(&model);
    FullFunctionEmitter::set_address_model(&model);

    delay_slot_load_falls_back();
    label_split_load_falls_back();
    fragment_split_load_falls_back();
    noncomplementary_lwl_falls_back();
    complementary_lwl_lwr_stays_native();
    patch_range_guards(config);
    patch_range_delay_boundaries(config);

    if (failures != 0) {
        std::fprintf(stderr, "%d full-function emitter test(s) failed\n", failures);
        return 1;
    }
    std::puts("Full-function emitter fail-closed tests passed");
    return 0;
}
