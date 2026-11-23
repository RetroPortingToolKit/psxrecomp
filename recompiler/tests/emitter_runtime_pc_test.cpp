// Every guest-visible PC the emitters bake goes through runtime_pc()
// (docs/SEGMENT_AWARE_CODE.md §5.2, rollout PR B).
//
// Game/overlay emitter: CodeGenerator::runtime_pc(addr) = code_seg | phys.
// The same synthetic function is compiled at KSEG0 compile addresses with the
// code segment left at its default, set to KSEG0 explicitly, to KUSEG and to
// KSEG1. The checks:
//   - the default and an explicit KSEG0 give byte-identical output, and the
//     default follows the image (a KSEG1 image keeps its own segment), so the
//     refactor changes nothing for any existing input;
//   - with KUSEG or KSEG1, every PC in guest-visible state carries that
//     segment: links (including a jalr in a branch delay slot), fetch tags,
//     interrupt resume PCs, CPS exits (split j, fallthrough past the unit
//     edge, image-edge tail transfer) and continuation keys, stale-static
//     guards (named and addressed), store-PC stamps, the reserved-instruction
//     EPC, the slice resume PC, call_by_address targets, the call-contract
//     return PC, the VSync-query hook's base PC, alias-group entry keys and
//     wrapper entries, and the unknown-dispatch PC of data stubs;
//   - identity keys (func_/block_ names, debug_server_log_call_entry, the
//     cyc_watch block observer, psx_native_bad_entry's owner) keep the
//     compile address;
//   - nothing else changes: with every 0x........ constant reduced to its
//     physical address, the KUSEG output equals the KSEG0 output;
//   - a KSEG1 code segment charges a fetch before every instruction, exactly
//     as compiling the same bytes at KSEG1 addresses does (#429's rule, now
//     keyed on the runtime PC; PR D's KSEG1 variants rely on it).
// Each is checked in CPS mode (the default), CPS overlay mode and legacy
// (PSX_CPS=0) mode.
//
// BIOS translator: StrictTranslator::translate(d, runtime_pc) stamps stores,
// the syscall EPC and the break/unaligned-access PCs (immediate and deferred
// loads) with the runtime PC while the psx_ldd_ temporaries keep the ROM
// address. The syscall form follows PSX_CPS at process start, so ctest runs
// this binary twice (emitter_runtime_pc_test and _legacy).
//
// Game dispatch rows (game_dispatch_emitter.cpp, PR C): each row's key and
// resume PC go through runtime_pc() while its func_ name keeps the compile
// identity, and the emitted lookup requires the exact PC. A real executable
// compiles in its own link segment, where runtime_pc() is the identity, so
// only this test (code segment != compile segment) sees the routes; the
// ledger and test_kuseg_dispatch_lookup.py check the emitted lookup end to end.
// Two rows on one physical word are an error within one compile; a segment
// variant's compile (PR D, §5.4) adds rows on its home rows' words, and the
// lookup then searches the word's rows for the exact PC.

#include "code_generator.h"
#include "control_flow.h"
#include "game_dispatch_emitter.h"
#include "mips_decoder.h"
#include "strict_translator.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <regex>
#include <set>
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

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", v);
    return buf;
}

// CodeGenerator reads PSX_CPS when it is constructed.
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void append_word(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word));
    bytes.push_back(static_cast<uint8_t>(word >> 8));
    bytes.push_back(static_cast<uint8_t>(word >> 16));
    bytes.push_back(static_cast<uint8_t>(word >> 24));
}

constexpr uint32_t kPhys = 0x00010000u;
constexpr uint32_t KUSEG = 0x00000000u, KSEG0 = 0x80000000u, KSEG1 = 0xA0000000u;

// Functions (compile offsets from kPhys):
//   +00 main: jal/bgezal/jalr links and CPS continuations, stores for
//       store-PC stamps (the sh at +10 is a widescreen backdrop site and the
//       sb at +14 a persisted-option site, which emit their own stamps);
//       bgezal's target +28 is a mid-function block leader.
//   +40 callee, also the VSync-query HLE function.
//   +48 a reserved word (RI exception).
//   +50 j to the known function +40; +58 j into the middle of +00.
//   +60 plain sh/sb stores and no control flow: falls through into +68.
//   +68 a jalr in the delay slot of an always-taken beq.
//   +7C, +84 end in jal / jalr whose return lies in the next (known) function.
//   +8C a conditional branch to the known +40, falling into the known +94.
//   +94 a conditional branch into the middle of +00, falling into +9C.
//   +9C, +A4 end in jal / jalr whose return lies in the next function; the jal
//       targets +B4.
//   +AC a data section; +B4 undecodable words (the PSX_EMIT_PROD data stub).
//   +BC host with the alias entries +C0 and +C4; it runs off the image edge.
// +9C, +A4, +AC and +B4 are left out of the known-function set, so edges into
// them go through call_by_address.
const uint32_t kProgram[] = {
    0x27BDFFF8u,  // +00 addiu $sp, $sp, -8
    0xAFBF0004u,  // +04 sw    $ra, 4($sp)
    0x0C004010u,  // +08 jal   +40
    0x00000000u,  // +0C nop
    0xA7A80002u,  // +10 sh    $t0, 2($sp)
    0xA3A80003u,  // +14 sb    $t0, 3($sp)
    0x04110003u,  // +18 bgezal $zero, +28
    0x00000000u,  // +1C nop
    0x0100F809u,  // +20 jalr  $t0
    0x00000000u,  // +24 nop
    0xABA80004u,  // +28 swl   $t0, 4($sp)
    0xBBA80004u,  // +2C swr   $t0, 4($sp)
    0xEBA80000u,  // +30 swc2  $8, 0($sp)
    0x8FBF0004u,  // +34 lw    $ra, 4($sp)
    0x03E00008u,  // +38 jr    $ra
    0x27BD0008u,  // +3C addiu $sp, $sp, 8
    0x03E00008u,  // +40 jr    $ra         (callee)
    0x00000000u,  // +44 nop
    0x50000001u,  // +48 beql  (reserved on the R3000A: RI exception)
    0x00000000u,  // +4C nop
    0x08004010u,  // +50 j     +40         (known function)
    0x00000000u,  // +54 nop
    0x08004001u,  // +58 j     +04         (mid-function)
    0x00000000u,  // +5C nop
    0xA7A80006u,  // +60 sh    $t0, 6($sp) (plain store sites: +10/+14
    0xA3A80007u,  // +64 sb    $t0, 7($sp)  are hooked; falls into +68)
    0x10000002u,  // +68 beq   $zero, $zero, +74
    0x0100F809u,  // +6C jalr  $t0         (in the delay slot)
    0x00000000u,  // +70 nop
    0x03E00008u,  // +74 jr    $ra
    0x00000000u,  // +78 nop
    0x0C004010u,  // +7C jal   +40         (returns into +84)
    0x00000000u,  // +80 nop
    0x0100F809u,  // +84 jalr  $t0         (returns into +8C)
    0x00000000u,  // +88 nop
    0x1500FFECu,  // +8C bne   $t0, $zero, +40
    0x00000000u,  // +90 nop
    0x1500FFDBu,  // +94 bne   $t0, $zero, +04
    0x00000000u,  // +98 nop
    0x0C00402Du,  // +9C jal   +B4         (not known; returns into +A4)
    0x00000000u,  // +A0 nop
    0x0100F809u,  // +A4 jalr  $t0         (returns into +AC)
    0x00000000u,  // +A8 nop
    0xFFFFFFFFu,  // +AC data
    0xFFFFFFFFu,  // +B0 data
    0x44000000u,  // +B4 COP1: no translation (TODO words)
    0x44000000u,  // +B8 COP1
    0x25080001u,  // +BC addiu $t0, $t0, 1 (alias host)
    0xAFA80000u,  // +C0 sw    $t0, 0($sp) (alias entry)
    0x25080001u,  // +C4 addiu $t0, $t0, 1 (alias entry)
    0x0C004010u,  // +C8 jal   +40
    0x00000000u,  // +CC nop
    0x25080001u,  // +D0 addiu $t0, $t0, 1 (runs off the image edge)
};
constexpr uint32_t kCount = sizeof(kProgram) / sizeof(kProgram[0]);

enum class Mode { Cps, CpsOverlay, Legacy };

const char* mode_name(Mode m) {
    return m == Mode::Cps ? "cps" : m == Mode::CpsOverlay ? "cps-overlay" : "legacy";
}

struct FuncSpec {
    uint32_t start, end;
    uint32_t alias_host;  // nonzero: an alias entry of the host at this offset
    bool data;            // a data section
};
const FuncSpec kFuncs[] = {
    {0x00u, 0x40u, 0, false}, {0x40u, 0x48u, 0, false}, {0x48u, 0x50u, 0, false},
    {0x50u, 0x58u, 0, false}, {0x58u, 0x60u, 0, false}, {0x60u, 0x68u, 0, false},
    {0x68u, 0x7Cu, 0, false}, {0x7Cu, 0x84u, 0, false}, {0x84u, 0x8Cu, 0, false},
    {0x8Cu, 0x94u, 0, false}, {0x94u, 0x9Cu, 0, false}, {0x9Cu, 0xA4u, 0, false},
    {0xA4u, 0xACu, 0, false}, {0xACu, 0xB4u, 0, true},  {0xB4u, 0xBCu, 0, false},
    {0xBCu, 0xD4u, 0, false}, {0xC0u, 0xD4u, 0xBCu, false}, {0xC4u, 0xD4u, 0xBCu, false},
};
constexpr uint32_t kVsyncFunc = 0x40u;
const std::set<uint32_t> kNotKnown = {0x9Cu, 0xA4u, 0xACu, 0xB4u};
constexpr uint32_t kBackdropSite = 0x10u;    // sh
constexpr uint32_t kPersistSite = 0x14u;     // sb
// Words with no emitted fetch of their own: the reserved word raises RI
// before its timing (the emitter's existing order) and the word after it is
// unreachable; data and undecodable words become stubs.
const std::set<uint32_t> kNoFetch = {0x48u, 0x4Cu, 0xACu, 0xB0u, 0xB4u, 0xB8u};

struct Compiled {
    std::string code;      // every generated function
    std::string dispatch;  // the game dispatch source
};

// seg_override < 0: leave the code segment at its default.
Compiled generate_all(uint32_t image_seg, long long seg_override, Mode mode) {
    set_env("PSX_CPS", mode == Mode::Legacy ? "0" : "1");
    const uint32_t base = image_seg | kPhys;
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = base;
    exe.header.initial_pc = base;
    exe.header.file_size = kCount * 4u;
    for (uint32_t w : kProgram) append_word(exe.code_data, w);

    PSXRecomp::CodeGenConfig config{};
    config.emit_comments = true;
    config.indent = "    ";
    config.overlay_mode = (mode == Mode::CpsOverlay);
    config.vsync_query_hle_funcs[base + kVsyncFunc] = {0x80090000u, 0x80090004u,
                                                       0x80090008u, 0x8009000Cu};
    config.ws_backdrop_x_sites.insert(base + kBackdropSite);
    config.persist_init_store_sites.insert(base + kPersistSite);
    PSXRecomp::ControlFlowAnalyzer analyzer(exe);
    PSXRecomp::CodeGenerator generator(exe, config);
    if (seg_override >= 0) generator.set_code_segment(static_cast<uint32_t>(seg_override));

    std::vector<PSXRecomp::Function> funcs;
    std::set<uint32_t> known;
    for (const FuncSpec& fs : kFuncs) {
        PSXRecomp::Function f{};
        f.start_addr = base + fs.start;
        f.end_addr = base + fs.end;
        f.size = fs.end - fs.start;
        f.name = "func_" + hex(f.start_addr);
        f.is_data_section = fs.data;
        if (fs.alias_host) {
            f.alias_walk_lo = base + fs.alias_host;
            for (const FuncSpec& o : kFuncs)
                if (o.alias_host == fs.alias_host) f.alias_group_entries.push_back(base + o.start);
        }
        funcs.push_back(f);
        if (!kNotKnown.count(fs.start)) known.insert(f.start_addr);
    }
    generator.set_known_functions(known);
    std::map<uint32_t, PSXRecomp::ControlFlowGraph> cfgs;
    for (const auto& f : funcs)
        if (!f.is_data_section) cfgs[f.start_addr] = analyzer.analyze_function(f);

    Compiled out;
    std::set<uint32_t> dispatch_addrs;
    for (const auto& gf : generator.generate_all_functions(funcs, cfgs)) {
        out.code += gf.full_code;
        // main_psx.cpp's rule: dispatchable func_ entries only.
        if (gf.dispatchable && gf.function_name.rfind("func_", 0) == 0)
            dispatch_addrs.insert(static_cast<uint32_t>(
                std::stoul(gf.function_name.substr(5, 8), nullptr, 16)));
    }
    std::string error;
    if (!PSXRecomp::emit_game_dispatch(generator, exe, dispatch_addrs,
                                       generator.generate_ranges_manifest(funcs, cfgs),
                                       out.dispatch, error))
        check(false, std::string("emit_game_dispatch: ") + error);
    return out;
}

std::string generate(uint32_t image_seg, long long seg_override, Mode mode) {
    return generate_all(image_seg, seg_override, mode).code;
}

// Constants of one site class, in emission order.
std::vector<uint32_t> constants(const std::string& code, const std::string& pattern) {
    std::vector<uint32_t> out;
    const std::regex re(pattern);
    for (auto it = std::sregex_iterator(code.begin(), code.end(), re);
         it != std::sregex_iterator(); ++it) {
        for (size_t g = 1; g < it->size(); ++g) {
            if ((*it)[g].matched) {
                out.push_back(static_cast<uint32_t>(std::stoul((*it)[g].str(), nullptr, 16)));
                break;
            }
        }
    }
    return out;
}

// Every 0x........ constant (literal or in a comment) reduced to physical.
std::string to_phys(const std::string& code) {
    static const std::regex re("0x([0-9A-F]{8})");
    std::string out;
    auto last = code.cbegin();
    for (auto it = std::sregex_iterator(code.begin(), code.end(), re);
         it != std::sregex_iterator(); ++it) {
        out.append(last, code.cbegin() + it->position());
        const uint32_t v = static_cast<uint32_t>(std::stoul((*it)[1].str(), nullptr, 16));
        out += "0x" + hex(v & 0x1FFFFFFFu);
        last = code.cbegin() + it->position() + it->length();
    }
    out.append(last, code.cend());
    return out;
}

struct Site {
    const char* id;
    const char* pattern;
    bool routed;  // true: carries the code segment; false: compile identity
};

const Site kSites[] = {
    {"link", R"(= 0x([0-9A-F]{8})u;  /\* (?:jal link|jalr link|branch-and-link))", true},
    {"cps-jal-note", R"(/\* CPS jal -> 0x([0-9A-F]{8}) \*/)", true},
    {"delay-slot-link", R"(= 0x([0-9A-F]{8});  cpu->pc = _jt_)", true},
    {"fetch-tag", R"(psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\))", true},
    {"irq-resume", R"(PSX_BLOCK_EDGE\(cpu, 0x([0-9A-F]{8})u\))", true},
    {"exit-pc", R"(cpu->pc = 0x([0-9A-F]{8})u;)", true},
    {"continuation-key", R"(case 0x([0-9A-F]{8})u:)", true},
    {"store-pc", R"(g_debug_last_store_pc = 0x([0-9A-F]{8})u;)", true},
    {"ri-epc", R"(cpu->cop0\[14\] = 0x([0-9A-F]{8})u;)", true},
    {"slice-pc", R"(psx_slice_block\(cpu, 0x([0-9A-F]{8})u)", true},
    {"call-by-address", R"(call_by_address\(cpu, 0x([0-9A-F]{8})u\))", true},
    {"call-contract", R"(psx_call_contract\(cpu, 0x([0-9A-F]{8})u)", true},
    {"stale-guard", R"(psx_game_text_native_ok\(0x([0-9A-F]{8})u\))", true},
    {"vsync-hle", R"(psx_vsync_query_hle_enter\(cpu, 0x([0-9A-F]{8})u,)", true},
    {"alias-entry", R"(psx_alias_body_[0-9A-F]{8}\(cpu, 0x([0-9A-F]{8})u\))", true},
    {"unknown-dispatch", R"(psx_unknown_dispatch\(cpu, 0x([0-9A-F]{8})u,)", true},
    {"cosim", R"(cosim_(?:block|instr)\(0x([0-9A-F]{8})u\))", true},
    {"func-name", R"(func_([0-9A-F]{8}))", false},
    {"block-label", R"(block_([0-9A-F]{8}))", false},
    {"call-entry-hook", R"(debug_server_log_call_entry\(0x([0-9A-F]{8})u\))", false},
    {"cyc-observe", R"(PSX_DEBUG_BLOCK_OBSERVE\(0x([0-9A-F]{8})u\))", false},
    {"bad-entry-owner", R"(psx_native_bad_entry\(cpu, 0x([0-9A-F]{8})u)", false},
};

// Emitted text that must be present, per mode, so a route cannot vanish
// unnoticed: each names one emission site.
struct Marker {
    const char* text;
    bool cps, overlay, legacy;
};
const Marker kMarkers[] = {
    {"/* CPS taken: split */", true, true, false},
    {"/* CPS not taken: split */", true, true, false},
    {"/* taken: split piece */", false, false, true},
    {"/* not taken: split piece */", false, false, true},
    {"/* taken: split (mid-func) */", false, false, true},
    {"/* not taken: split (mid-func) */", false, false, true},
    {"psx_ws_backdrop_x(", true, true, true},
    {"psx_game_option_store(", true, true, true},
    {"/* CPS j: split */", true, true, false},
    {"/* external jal */", false, false, true},
    {"/* jal cont: split piece */", false, false, true},
    {"/* jalr cont: split piece */", false, false, true},
    {"/* jal cont: split */", false, false, true},
    {"/* jalr cont: split */", false, false, true},
    {"/* j to split piece */", false, false, true},
    {"/* j to split (mid-func) */", false, false, true},
    {"/* fallthrough to split piece */", true, true, true},
    {"/* fallthrough to next function */", true, true, true},
    {"/* CPS fallthrough past unit edge */", true, true, false},
    {"/* image-edge fallthrough: tail-transfer */", true, true, true},
    {"/* alias entry into host", true, true, true},
    {"switch (entry) {", true, true, true},
    {"/* jalr */", true, true, true},
    {"/* branch-and-link before delay slot */", true, true, true},
};

void check_game_emitter(Mode mode) {
    const std::string m = mode_name(mode);
    const std::string def = generate(KSEG0, -1, mode);
    check(def == generate(KSEG0, KSEG0, mode),
          m + ": an explicit KSEG0 code segment changed the output");
    // A KSEG1 image keeps its own segment by default (the pre-B behaviour).
    const std::string k1_image = generate(KSEG1, -1, mode);
    check(k1_image == generate(KSEG1, KSEG1, mode),
          m + ": the default code segment does not follow the image");

    for (const Marker& mk : kMarkers) {
        const bool want = mode == Mode::Cps ? mk.cps
                        : mode == Mode::CpsOverlay ? mk.overlay : mk.legacy;
        if (want)
            check(def.find(mk.text) != std::string::npos,
                  m + ": the program no longer reaches \"" + mk.text + "\"");
    }

    for (uint32_t seg : {KUSEG, KSEG1}) {
        const std::string sn = seg == KUSEG ? "KUSEG" : "KSEG1";
        const std::string code = generate(KSEG0, seg, mode);
        // KSEG1 adds per-instruction fetches (checked below); KUSEG is
        // cached like KSEG0, so only constant segments may differ.
        if (seg == KUSEG)
            check(to_phys(code) == to_phys(def),
                  m + "/" + sn + ": output differs from KSEG0 beyond constant segments");
        std::set<std::string> seen;
        for (const Site& s : kSites) {
            const auto vals = constants(code, s.pattern);
            if (!vals.empty()) seen.insert(s.id);
            for (uint32_t v : vals) {
                // Continuation keys also include jump-table case values in
                // general; this program has none, so every key is a PC.
                const uint32_t want = s.routed ? seg : KSEG0;
                if (s.routed && v == 0) continue;
                check((v & 0xE0000000u) == want,
                      m + "/" + sn + ": " + s.id + " 0x" + hex(v) + " is not in " +
                          (s.routed ? sn : std::string("the compile segment")));
            }
        }
        // Every class this mode emits must actually have been seen.
        std::vector<std::string> want_seen = {
            "link", "delay-slot-link", "fetch-tag", "irq-resume", "exit-pc",
            "continuation-key", "store-pc", "ri-epc", "slice-pc", "stale-guard",
            "alias-entry", "unknown-dispatch", "func-name", "block-label",
            "call-entry-hook", "cyc-observe"};
        if (mode != Mode::Legacy) want_seen.push_back("cps-jal-note");
        if (mode == Mode::Legacy) {
            want_seen.push_back("call-by-address");
            want_seen.push_back("call-contract");
        }
        if (mode == Mode::CpsOverlay) want_seen.push_back("bad-entry-owner");
        // The VSync-query hook's hand-timed body charges cached fetches only,
        // so an uncached code segment runs the compiled instructions instead.
        if (seg == KUSEG) want_seen.push_back("vsync-hle");
        else check(seen.count("vsync-hle") == 0,
                   m + "/" + sn + ": the VSync-query hook is emitted for uncached code");
        for (const auto& id : want_seen)
            check(seen.count(id) == 1, m + "/" + sn + ": no " + id + " site emitted");
    }
    check(def.find("psx_vsync_query_hle_enter(cpu, 0x" + hex(KSEG0 | (kPhys + kVsyncFunc)) +
                   "u,") != std::string::npos,
          m + ": the KSEG0 VSync-query hook is missing or moved");

    // KSEG1 code segment at KSEG0 compile addresses charges exactly what the
    // same bytes compiled at KSEG1 addresses charge: a fetch per instruction.
    const auto k1_tags = constants(generate(KSEG0, KSEG1, mode),
                                   R"(psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\))");
    const auto k1_image_tags = constants(k1_image,
                                         R"(psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\))");
    check(k1_tags == k1_image_tags,
          m + ": a KSEG1 code segment does not charge like a KSEG1 image");
    std::set<uint32_t> charged(k1_tags.begin(), k1_tags.end());
    for (uint32_t i = 0; i < kCount; ++i) {
        if (kNoFetch.count(i * 4u)) continue;
        check(charged.count(KSEG1 | (kPhys + i * 4u)) == 1,
              m + ": KSEG1 instruction +" + hex(i * 4u) + " has no fetch of its own");
    }
    const auto k0_tags = constants(def, R"(psx_icache_fetch\(cpu, 0x([0-9A-F]{8})u\))");
    check(k0_tags.size() < k1_tags.size(),
          m + ": the cached KSEG0 body lost the line-leader rule");
}

// Dispatch rows: {key, resume, range index, range count, func_ owner}.
struct Row {
    uint32_t addr, resume, owner;
};
std::vector<Row> dispatch_rows(const std::string& src) {
    std::vector<Row> rows;
    static const std::regex re(
        R"(\{0x([0-9A-F]{8})u, 0x([0-9A-F]{8})u, \d+u, \d+u, func_([0-9A-F]{8})\})");
    for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
         it != std::sregex_iterator(); ++it)
        rows.push_back({static_cast<uint32_t>(std::stoul((*it)[1].str(), nullptr, 16)),
                        static_cast<uint32_t>(std::stoul((*it)[2].str(), nullptr, 16)),
                        static_cast<uint32_t>(std::stoul((*it)[3].str(), nullptr, 16))});
    return rows;
}

void check_dispatch_rows(Mode mode) {
    const std::string m = mode_name(mode) + std::string("/dispatch");
    const std::string def = generate_all(KSEG0, -1, mode).dispatch;
    check(def == generate_all(KSEG0, KSEG0, mode).dispatch,
          m + ": an explicit KSEG0 code segment changed the dispatch source");
    const auto def_rows = dispatch_rows(def);
    check(def_rows.size() >= 10, m + ": too few dispatch rows parsed");
    // The lookup indexes the physical word, then requires the exact PC.
    check(def.find("if (!index || k_psx_game_dispatch[index - 1u].addr != addr) return 0;") !=
              std::string::npos,
          m + ": the lookup does not require the exact PC");
    for (uint32_t seg : {KUSEG, KSEG1}) {
        const std::string sn = seg == KUSEG ? "KUSEG" : "KSEG1";
        const std::string src = generate_all(KSEG0, seg, mode).dispatch;
        const auto rows = dispatch_rows(src);
        check(rows.size() == def_rows.size(), m + "/" + sn + ": row count changed");
        bool continuation = false;
        for (size_t i = 0; i < rows.size() && i < def_rows.size(); ++i) {
            const Row& r = rows[i];
            check((r.addr & 0xE0000000u) == seg,
                  m + "/" + sn + ": row key 0x" + hex(r.addr) + " is not in " + sn);
            check(r.resume == 0 || (r.resume & 0xE0000000u) == seg,
                  m + "/" + sn + ": resume PC 0x" + hex(r.resume) + " is not in " + sn);
            check((r.owner & 0xE0000000u) == KSEG0 && r.owner == def_rows[i].owner,
                  m + "/" + sn + ": func_" + hex(r.owner) + " is not the compile identity");
            check((r.addr & 0x1FFFFFFFu) == (def_rows[i].addr & 0x1FFFFFFFu),
                  m + "/" + sn + ": row 0x" + hex(r.addr) + " moved");
            continuation |= r.resume != 0;
        }
        if (mode != Mode::Legacy)
            check(continuation, m + "/" + sn + ": no continuation row (resume PC) emitted");
        if (seg == KUSEG)
            check(to_phys(src) == to_phys(def),
                  m + "/" + sn + ": dispatch differs from KSEG0 beyond constant segments");
    }
}

// One compile has one code segment, so two rows on one physical word within
// it are the same PC: a build error, never a row that silently shadows the
// other. Segment variants (§5.4) are separate compiles, and their rows share
// the home rows' words: adjacent, in PC order, looked up by exact PC. Two
// compiles in one segment would name the same PCs, which is an error again.
void check_dispatch_duplicate_word() {
    const uint32_t base = KSEG0 | kPhys;
    PSXRecomp::PS1Executable exe{};
    exe.header.load_address = base;
    exe.header.initial_pc = base;
    exe.header.file_size = kCount * 4u;
    for (uint32_t w : kProgram) append_word(exe.code_data, w);
    PSXRecomp::CodeGenConfig config{};
    PSXRecomp::CodeGenerator generator(exe, config);
    std::string out, error;
    check(PSXRecomp::emit_game_dispatch(generator, exe, {base, base + 0x20u}, "", out, error) &&
              error.empty() && !out.empty(),
          "dispatch: rows on distinct words must emit");
    for (uint32_t alias : {KUSEG | kPhys, KSEG1 | (kPhys + 0x20u)}) {
        out.clear();
        error.clear();
        check(!PSXRecomp::emit_game_dispatch(generator, exe, {base, base + 0x20u, alias}, "",
                                             out, error),
              "dispatch: two rows on one physical word (0x" + hex(alias) + ") must be an error");
        check(error.find("share a physical word") != std::string::npos,
              "dispatch: the duplicate-word error must say why, got: " + error);
    }

    PSXRecomp::PS1Executable kseg1 = exe;
    kseg1.header.load_address = KSEG1 | kPhys;
    kseg1.header.initial_pc = KSEG1 | kPhys;
    PSXRecomp::CodeGenerator variant(kseg1, config);
    const std::vector<PSXRecomp::GameDispatchUnit> units{
        {&generator, {base, base + 0x20u}}, {&variant, {KSEG1 | kPhys}}};
    out.clear();
    error.clear();
    check(PSXRecomp::emit_game_dispatch(units, exe, "", out, error) && error.empty(),
          "dispatch: a variant compile's row may share its home row's word: " + error);
    const auto home_row = out.find("{0x" + hex(base) + "u, 0x00000000u");
    const auto variant_row = out.find("{0x" + hex(KSEG1 | kPhys) + "u, 0x00000000u");
    const auto variant_line =
        variant_row == std::string::npos
            ? std::string()
            : out.substr(variant_row, out.find('\n', variant_row) - variant_row);
    check(home_row != std::string::npos && variant_row != std::string::npos &&
              home_row < variant_row &&
              variant_line.find("func_" + hex(KSEG1 | kPhys) + "}") != std::string::npos,
          "dispatch: the variant row follows the home row on their word and names its "
          "body: " + variant_line);
    check(out.find("k_psx_game_dispatch[row].addr == addr") != std::string::npos,
          "dispatch: a shared word is searched for the exact PC");
    const std::vector<PSXRecomp::GameDispatchUnit> twice{
        {&generator, {base}}, {&variant, {KSEG1 | kPhys}}, {&variant, {KSEG1 | kPhys}}};
    out.clear();
    error.clear();
    check(!PSXRecomp::emit_game_dispatch(twice, exe, "", out, error) &&
              error.find("are the same PC 0x" + hex(KSEG1 | kPhys)) != std::string::npos,
          "dispatch: two compiles in one segment name the same PC, an error: " + error);
}

void check_strict_translator(bool strict_cps) {
    // SCPH-1001 Kernel Part 2 runs at 0x500 from ROM 0xBFC10000: the store at
    // ROM 0xBFC10A00 executes at 0x00000F00 (memory.c's RAM-0 filter key).
    const uint32_t rom = 0xBFC10A00u, rt_pc = 0x00000F00u;
    struct Case {
        uint32_t raw;
        const char* want;  // substring that must carry the runtime PC
    };
    // The syscall form is latched from PSX_CPS when the process starts.
    const char* syscall_want = strict_cps
        ? "cpu->pc = 0x00000F00u; if (psx_syscall(cpu, cpu->gpr[2])) return;"
        : "cpu->pc = 0x00000F00u; psx_syscall(cpu, cpu->gpr[2]); return;";
    const Case cases[] = {
        {0xAC880000u, "g_debug_last_store_pc = 0x00000F00u"},              // sw
        {0xA4880000u, "psx_unaligned_access(cpu, psx_addr, 0x00000F00u)"}, // sh
        {0xA0880000u, "g_debug_last_store_pc = 0x00000F00u"},              // sb
        {0xA8880000u, "g_debug_last_store_pc = 0x00000F00u"},              // swl
        {0xB8880000u, "g_debug_last_store_pc = 0x00000F00u"},              // swr
        {0xE8880000u, "g_debug_last_store_pc = 0x00000F00u"},              // swc2
        {0x8C880000u, "psx_unaligned_access(cpu, psx_addr, 0x00000F00u)"}, // lw
        {0x84880000u, "psx_unaligned_access(cpu, psx_addr, 0x00000F00u)"}, // lh
        {0x94880000u, "psx_unaligned_access(cpu, psx_addr, 0x00000F00u)"}, // lhu
        {0x0000000Cu, syscall_want},                                        // syscall
        {0x0000000Du, "0x00000F00u); return;"},                             // break
    };
    for (const Case& c : cases) {
        const auto d = PSXRecomp::MipsDecoder::decode(c.raw, rom);
        const auto tr = PSXRecompV4::StrictTranslator::translate(d, rt_pc);
        check(tr.supported, "translator: 0x" + hex(c.raw) + " unsupported");
        check(tr.c_code.find(c.want) != std::string::npos,
              "translator: 0x" + hex(c.raw) + " lacks \"" + c.want + "\": " + tr.c_code);
        check(tr.c_code.find("0xBFC10A00u") == std::string::npos,
              "translator: 0x" + hex(c.raw) + " still bakes the ROM address: " + tr.c_code);
        // The one-argument form is the in-place case (runtime == ROM).
        const auto same = PSXRecompV4::StrictTranslator::translate(d);
        check(same.c_code == PSXRecompV4::StrictTranslator::translate(d, rom).c_code,
              "translator: translate(d) != translate(d, d.address) for 0x" + hex(c.raw));
        // A deferred load's temporary is an identity key: it keeps the ROM
        // address, while its unaligned-access PC is the runtime PC.
        if (!tr.c_code_deferred.empty())
            check(tr.c_code_deferred.find("psx_ldd_BFC10A00") != std::string::npos &&
                      tr.c_code_deferred.find("psx_unaligned_access(cpu, psx_addr, 0x00000F00u)") !=
                          std::string::npos &&
                      tr.c_code_deferred.find("0xBFC10A00u") == std::string::npos,
                  "translator: deferred 0x" + hex(c.raw) +
                      " must name psx_ldd_<ROM> and fault at the runtime PC: " +
                      tr.c_code_deferred);
    }
    int deferred = 0;
    for (uint32_t raw : {0x8C880000u, 0x84880000u, 0x94880000u})
        deferred += !PSXRecompV4::StrictTranslator::translate(
                         PSXRecomp::MipsDecoder::decode(raw, rom), rt_pc)
                         .c_code_deferred.empty();
    check(deferred == 3, "translator: lw/lh/lhu no longer emit a deferred form");
}

}  // namespace

int main() {
    // strict_translator.cpp latches PSX_CPS at static initialisation, before
    // check_game_emitter() starts setting it per mode.
    const char* cps_env = std::getenv("PSX_CPS");
    const bool strict_cps = cps_env == nullptr || cps_env[0] != '0';
    const char* mode = std::getenv("PSX_CODEGEN_CYCLE_PER_INSN");
    if (mode != nullptr && mode[0] == '0') {
        std::puts("SKIP: PSX_CODEGEN_CYCLE_PER_INSN=0 emits no fetch charges");
        return 0;
    }
    check_game_emitter(Mode::Cps);
    check_game_emitter(Mode::CpsOverlay);
    check_game_emitter(Mode::Legacy);
    check_dispatch_rows(Mode::Cps);
    check_dispatch_rows(Mode::Legacy);
    check_dispatch_duplicate_word();
    check_strict_translator(strict_cps);
    if (failures != 0) {
        std::fprintf(stderr, "emitter_runtime_pc_test: %d failure(s)\n", failures);
        return 1;
    }
    std::puts("PASS: every baked guest PC follows the code segment; identity keys and "
              "KSEG0 output unchanged");
    return 0;
}
