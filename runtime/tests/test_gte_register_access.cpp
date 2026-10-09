#include "cpu_state.h"
#include "gte.h"
#include "hle_gte.h"
#include "execution_profile.h"
#include "pgxp.h"
#include "projection_scale.hpp"
#include "gte_view.h"
#include "gte_nclip_stats.h"
#include "render_pass_projection.h"
#include <limits>
extern "C" void gte_set_fov_scale(int, int);

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

using PSXRecomp::GTE::GTEState;
using PSXRecomp::GTE::gte_cfc2;
using PSXRecomp::GTE::gte_ctc2;
using PSXRecomp::GTE::gte_mfc2;
using PSXRecomp::GTE::gte_mtc2;

extern "C" void gte_canonicalize_cpu_state(CPUState *cpu);
extern "C" void gte_test_set_precise_valid_mask(uint32_t mask);
extern "C" uint32_t gte_test_get_precise_valid_mask(void);
extern "C" void gte_test_set_timeline_generations(uint32_t precision,
                                                   uint32_t geometry);
extern "C" uint32_t gte_test_get_precision_generation(void);
extern "C" uint32_t gte_test_get_geometry_generation(void);
extern "C" void gte_precision_tracking_set(int enabled);
extern "C" void gte_precision_invalidate_word(uint32_t addr);
extern "C" int gte_precision_load_word(uint32_t addr, uint32_t packed,
                                        int32_t *x16, int32_t *y16,
                                        uint16_t *z);
extern "C" void gte_geometry_correction_set(int enabled);
extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                                int32_t *x16, int32_t *y16);
extern "C" void gte_test_seed_precise_projection(uint32_t index,
                                                   uint32_t packed,
                                                   int32_t x16,
                                                   int32_t y16,
                                                   uint16_t z);
extern "C" void gte_test_get_precise_projection(uint32_t index,
                                                  uint32_t *packed,
                                                  int32_t *x16,
                                                  int32_t *y16,
                                                  uint16_t *z,
                                                  uint8_t *valid);
extern "C" void gte_test_seed_geometry(uint32_t packed, int32_t x16,
                                        int32_t y16);
extern "C" void gte_test_execute_reference(CPUState *cpu, uint32_t cmd);

/* gte.cpp runtime dependencies that are irrelevant to register-transfer tests. */
extern "C" int gpu_ws_present_native_43(void) { return 0; }
static int g_test_precise_nclip_enabled;
extern "C" int gpu_ws_precise_nclip_enabled(void) {
    return g_test_precise_nclip_enabled;
}
extern "C" void gpu_pgxp_rederive_enable(void) {}
/* This fixture has ordinary RAM only; no optional GPU DMA arena is allocated. */
extern "C" int psx_mod_gpu_dma_memory_contains(uint32_t, uint32_t) { return 0; }
static int g_test_shadow_diff = 0;
extern "C" int psx_overlay_shadow_diff_active(void) { return g_test_shadow_diff; }
extern "C" void psx_ws_note_gte_project(int) {}
static int g_test_netplay_active = 0;
extern "C" int psx_netplay_active(void) { return g_test_netplay_active; }
extern "C" {
uint64_t s_frame_count = 0;
uint32_t g_debug_last_store_pc = 0;
}

uint64_t g_test_cycle = 0;
uint32_t g_test_gte_set_calls = 0;
uint32_t g_test_gte_last_latency = 0;

extern "C" uint32_t psx_gte_cmd_latency(uint32_t cmd) {
    return 7u + (cmd & 0x3Fu);
}

extern "C" void psx_gte_set(CPUState *cpu, uint32_t latency) {
    ++g_test_gte_set_calls;
    g_test_gte_last_latency = latency;
    if (cpu->gte_ts_done > g_test_cycle) g_test_cycle = cpu->gte_ts_done;
    cpu->gte_ts_done = g_test_cycle + latency;
}

namespace {

struct PreciseState {
    uint32_t packed;
    int32_t x16;
    int32_t y16;
    uint16_t z;
    uint8_t valid;
};

std::array<PreciseState, 4> precise_snapshot() {
    std::array<PreciseState, 4> result{};
    for (uint32_t i = 0; i < result.size(); ++i) {
        auto &p = result[i];
        gte_test_get_precise_projection(i, &p.packed, &p.x16, &p.y16,
                                        &p.z, &p.valid);
    }
    return result;
}

void seed_precise_snapshot() {
    for (uint32_t i = 0; i < 4; ++i)
        gte_test_seed_precise_projection(i, 0x01010001u * (i + 1u),
                                         0x10000 + static_cast<int32_t>(i * 31),
                                         -0x20000 - static_cast<int32_t>(i * 47),
                                         static_cast<uint16_t>(0x100u + i));
}

bool same_precise(const std::array<PreciseState, 4> &lhs,
                  const std::array<PreciseState, 4> &rhs) {
    for (uint32_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].packed != rhs[i].packed || lhs[i].x16 != rhs[i].x16 ||
            lhs[i].y16 != rhs[i].y16 || lhs[i].z != rhs[i].z ||
            lhs[i].valid != rhs[i].valid)
            return false;
    }
    return true;
}

constexpr std::array<uint32_t, 10> kEdgeValues = {
    0x00000000u, 0x00000001u, 0x00007FFFu, 0x00008000u, 0x0000FFFFu,
    0x00010000u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFEu, 0xFFFFFFFFu,
};

constexpr std::array<uint8_t, 6> kInvalidRegs = {32u, 33u, 63u, 64u, 127u, 255u};

uint64_t g_rng = 0xD1B54A32D192ED03ull;

uint32_t random_u32() {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return static_cast<uint32_t>((g_rng * 0x2545F4914F6CDD1Dull) >> 16);
}

void randomize_gte(CPUState &cpu) {
    std::memset(&cpu, 0, sizeof(cpu));
    for (uint32_t &value : cpu.gte_data) value = random_u32();
    for (uint32_t &value : cpu.gte_ctrl) value = random_u32();
}

void oracle_load(GTEState &gte, const CPUState &cpu) {
    for (uint8_t reg = 0; reg < 32; ++reg) {
        if (reg == 15 || reg == 28) continue;
        gte_mtc2(&gte, reg, cpu.gte_data[reg]);
    }
    for (uint8_t reg = 0; reg < 32; ++reg)
        gte_ctc2(&gte, reg, cpu.gte_ctrl[reg]);
    /* Importing emulator state is not a guest CTC2 write. Preserve a computed
     * FLAG error bit and any existing backing bits exactly like gte_load_data. */
    gte.FLAG = cpu.gte_ctrl[31];
}

void oracle_export(CPUState &cpu, GTEState &gte) {
    for (uint8_t reg = 0; reg < 32; ++reg)
        cpu.gte_data[reg] = gte_mfc2(&gte, reg);
    for (uint8_t reg = 0; reg < 32; ++reg)
        cpu.gte_ctrl[reg] = gte_cfc2(&gte, reg);
}

void oracle_canonicalize(CPUState &cpu) {
    GTEState gte;
    oracle_load(gte, cpu);
    oracle_export(cpu, gte);
}

uint32_t oracle_read_data(const CPUState &cpu, uint8_t reg) {
    GTEState gte;
    oracle_load(gte, cpu);
    return gte_mfc2(&gte, reg);
}

uint32_t oracle_read_ctrl(const CPUState &cpu, uint8_t reg) {
    GTEState gte;
    oracle_load(gte, cpu);
    return gte_cfc2(&gte, reg);
}

void oracle_write_data(CPUState &cpu, uint8_t reg, uint32_t value) {
    GTEState gte;
    oracle_load(gte, cpu);
    gte_mtc2(&gte, reg, value);
    oracle_export(cpu, gte);
}

void oracle_write_ctrl(CPUState &cpu, uint8_t reg, uint32_t value) {
    GTEState gte;
    oracle_load(gte, cpu);
    gte_ctc2(&gte, reg, value);
    oracle_export(cpu, gte);
}

bool same_gte(const CPUState &lhs, const CPUState &rhs) {
    return std::memcmp(lhs.gte_data, rhs.gte_data, sizeof(lhs.gte_data)) == 0 &&
           std::memcmp(lhs.gte_ctrl, rhs.gte_ctrl, sizeof(lhs.gte_ctrl)) == 0;
}

int fail_value(const char *phase, unsigned iteration, unsigned reg,
               uint32_t value, uint32_t expected, uint32_t actual) {
    std::fprintf(stderr,
                 "FAIL: %s iter=%u reg=%u value=%08X expected=%08X actual=%08X\n",
                 phase, iteration, reg, value, expected, actual);
    return 1;
}

int fail_state(const char *phase, unsigned iteration, unsigned reg, uint32_t value,
               const CPUState &expected, const CPUState &actual) {
    for (unsigned i = 0; i < 32; ++i) {
        if (expected.gte_data[i] != actual.gte_data[i]) {
            std::fprintf(stderr,
                         "FAIL: %s iter=%u source_reg=%u value=%08X data=%u "
                         "expected=%08X actual=%08X\n",
                         phase, iteration, reg, value, i, expected.gte_data[i],
                         actual.gte_data[i]);
            return 1;
        }
    }
    for (unsigned i = 0; i < 32; ++i) {
        if (expected.gte_ctrl[i] != actual.gte_ctrl[i]) {
            std::fprintf(stderr,
                         "FAIL: %s iter=%u source_reg=%u value=%08X ctrl=%u "
                         "expected=%08X actual=%08X\n",
                         phase, iteration, reg, value, i, expected.gte_ctrl[i],
                         actual.gte_ctrl[i]);
            return 1;
        }
    }
    std::fprintf(stderr, "FAIL: %s reported unequal states without a differing word\n",
                 phase);
    return 1;
}

int check_all_reads(const CPUState &expected, CPUState &actual,
                    const char *phase, unsigned iteration) {
    for (uint8_t reg = 0; reg < 32; ++reg) {
        const uint32_t expected_data = oracle_read_data(expected, reg);
        const uint32_t actual_data = gte_read_data(&actual, reg);
        if (actual_data != expected_data)
            return fail_value(phase, iteration, reg, 0, expected_data, actual_data);

        const uint32_t expected_ctrl = oracle_read_ctrl(expected, reg);
        const uint32_t actual_ctrl = gte_read_ctrl(&actual, reg);
        if (actual_ctrl != expected_ctrl)
            return fail_value(phase, iteration, reg, 1, expected_ctrl, actual_ctrl);
    }
    return 0;
}

int test_canonicalizer() {
    for (unsigned iteration = 0; iteration < 512; ++iteration) {
        CPUState expected;
        randomize_gte(expected);
        CPUState actual = expected;
        oracle_canonicalize(expected);
        gte_canonicalize_cpu_state(&actual);
        if (!same_gte(expected, actual))
            return fail_state("canonicalize", iteration, 0, 0, expected, actual);
    }
    return 0;
}

int test_reads() {
    for (unsigned iteration = 0; iteration < 256; ++iteration) {
        CPUState state;
        randomize_gte(state);
        /* Deliberately use raw randomized backing here. Read helpers must match
         * the architectural value reconstructed by the old bridge even before
         * an explicit canonicalization boundary. */
        CPUState before = state;
        if (int rc = check_all_reads(state, state, "raw read", iteration)) return rc;
        if (!same_gte(before, state))
            return fail_state("read mutated state", iteration, 0, 0, before, state);

        for (uint8_t reg : kInvalidRegs) {
            const uint32_t actual_data = gte_read_data(&state, reg);
            const uint32_t actual_ctrl = gte_read_ctrl(&state, reg);
            if (actual_data != 0u)
                return fail_value("invalid data read", iteration, reg, 0, 0, actual_data);
            if (actual_ctrl != 0u)
                return fail_value("invalid ctrl read", iteration, reg, 0, 0, actual_ctrl);
        }
    }
    return 0;
}

int test_hardware_register_semantics() {
    CPUState cpu{};
    gte_write_data(&cpu, 1, 0x00008001u);
    if (gte_read_data(&cpu, 1) != 0xFFFF8001u)
        return fail_value("VZ sign extension", 0, 1, 0x00008001u,
                          0xFFFF8001u, gte_read_data(&cpu, 1));

    gte_write_data(&cpu, 23, 0xDEADBEEFu);
    if (gte_read_data(&cpu, 23) != 0xDEADBEEFu)
        return fail_value("RES1 round trip", 0, 23, 0xDEADBEEFu,
                          0xDEADBEEFu, gte_read_data(&cpu, 23));

    gte_write_ctrl(&cpu, 4, 0x00008002u);
    if (gte_read_ctrl(&cpu, 4) != 0xFFFF8002u)
        return fail_value("matrix tail sign extension", 0, 4, 0x00008002u,
                          0xFFFF8002u, gte_read_ctrl(&cpu, 4));

    gte_write_ctrl(&cpu, 26, 0x0000E810u);
    if (gte_read_ctrl(&cpu, 26) != 0xFFFFE810u)
        return fail_value("H read sign extension", 0, 26, 0x0000E810u,
                          0xFFFFE810u, gte_read_ctrl(&cpu, 26));

    gte_write_ctrl(&cpu, 31, 0x00800000u);
    if (gte_read_ctrl(&cpu, 31) != 0x80800000u)
        return fail_value("FLAG error summary", 0, 31, 0x00800000u,
                          0x80800000u, gte_read_ctrl(&cpu, 31));
    return 0;
}

int test_writes() {
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        CPUState seed;
        randomize_gte(seed);

        /* Valid helper writes must also normalize unrelated backing words.
         * This preserves the old bridge's behavior when falling back from
         * committed AOT C that predates helper routing for masked registers. */
        for (uint8_t reg = 0; reg < 32; ++reg) {
            for (uint32_t edge : kEdgeValues) {
                const uint32_t value = edge ^ (iteration ? random_u32() : 0u);

                CPUState expected = seed;
                CPUState actual = seed;
                oracle_write_data(expected, reg, value);
                gte_write_data(&actual, reg, value);
                if (!same_gte(expected, actual))
                    return fail_state("data write", iteration, reg, value,
                                      expected, actual);

                expected = seed;
                actual = seed;
                oracle_write_ctrl(expected, reg, value);
                gte_write_ctrl(&actual, reg, value);
                if (!same_gte(expected, actual))
                    return fail_state("ctrl write", iteration, reg, value,
                                      expected, actual);
            }
        }

        CPUState canonical_seed = seed;
        oracle_canonicalize(canonical_seed);
        for (uint8_t reg : kInvalidRegs) {
            CPUState expected = canonical_seed;
            CPUState actual = canonical_seed;
            const uint32_t value = random_u32();
            oracle_write_data(expected, reg, value);
            gte_write_data(&actual, reg, value);
            if (!same_gte(expected, actual))
                return fail_state("invalid data write", iteration, reg, value,
                                  expected, actual);

            expected = canonical_seed;
            actual = canonical_seed;
            oracle_write_ctrl(expected, reg, value);
            gte_write_ctrl(&actual, reg, value);
            if (!same_gte(expected, actual))
                return fail_state("invalid ctrl write", iteration, reg, value,
                                  expected, actual);
        }
    }

    return 0;
}

int test_sequence_fuzz() {
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        CPUState expected;
        randomize_gte(expected);
        oracle_canonicalize(expected);
        CPUState actual = expected;

        for (unsigned step = 0; step < 512; ++step) {
            const uint32_t selector = random_u32();
            const uint8_t reg = (selector & 7u) == 0u
                                    ? kInvalidRegs[selector % kInvalidRegs.size()]
                                    : static_cast<uint8_t>((selector >> 8) & 31u);
            const uint32_t value = random_u32();
            switch ((selector >> 16) & 3u) {
            case 0: {
                const uint32_t want = oracle_read_data(expected, reg);
                const uint32_t got = gte_read_data(&actual, reg);
                if (want != got)
                    return fail_value("sequence data read", iteration, reg, value,
                                      want, got);
                break;
            }
            case 1: {
                const uint32_t want = oracle_read_ctrl(expected, reg);
                const uint32_t got = gte_read_ctrl(&actual, reg);
                if (want != got)
                    return fail_value("sequence ctrl read", iteration, reg, value,
                                      want, got);
                break;
            }
            case 2:
                oracle_write_data(expected, reg, value);
                gte_write_data(&actual, reg, value);
                break;
            case 3:
                oracle_write_ctrl(expected, reg, value);
                gte_write_ctrl(&actual, reg, value);
                break;
            }
            if (!same_gte(expected, actual))
                return fail_state("sequence state", iteration, reg, value,
                                  expected, actual);
        }
    }
    return 0;
}

static unsigned projection_callback_calls;
static int substitute_projection(CPUState *cpu, uint32_t) {
    ++projection_callback_calls;
    cpu->gte_ctrl[0] = 0x1000u;
    cpu->gte_ctrl[1] = 0u;
    cpu->gte_ctrl[2] = 0x1000u;
    cpu->gte_ctrl[3] = 0u;
    cpu->gte_ctrl[4] = 0x1000u;
    cpu->gte_ctrl[5] = 32u;
    cpu->gte_ctrl[6] = 16u;
    cpu->gte_ctrl[7] = 1024u;
    return 1;
}

int test_projection_override_restores_transform() {
    for (uint32_t command : {0x80001u, 0x80030u}) {
        CPUState actual{};
        actual.gte_ctrl[0] = 0x800u;
        actual.gte_ctrl[2] = 0x800u;
        actual.gte_ctrl[4] = 0x800u;
        actual.gte_ctrl[5] = 80u;
        actual.gte_ctrl[6] = 40u;
        actual.gte_ctrl[7] = 2048u;
        actual.gte_ctrl[26] = 256u;
        actual.gte_data[0] = 0x00100020u;
        actual.gte_data[1] = 64u;
        actual.gte_data[2] = 0x00300040u;
        actual.gte_data[3] = 96u;
        actual.gte_data[4] = 0x00500060u;
        actual.gte_data[5] = 128u;
        CPUState original = actual, expected = actual;
        substitute_projection(&expected, command);
        gte_test_execute_reference(&expected, command);
        projection_callback_calls = 0;
        g_psx_projection_command = substitute_projection;
        gte_execute(&actual, command);
        g_psx_projection_command = nullptr;
        if (projection_callback_calls != 1 ||
            std::memcmp(actual.gte_ctrl, original.gte_ctrl, 8 * sizeof(uint32_t)) ||
            std::memcmp(actual.gte_data, expected.gte_data, sizeof(actual.gte_data))) {
            std::fprintf(stderr, "FAIL: projection override leaked transform or changed output (%x)\n", command);
            return 1;
        }
        g_psx_projection_command = substitute_projection;
        gte_execute(&actual, 0x06u);
        g_psx_projection_command = nullptr;
        if (projection_callback_calls != 1) return 1;
    }
    return 0;
}

int test_hle_command_contract() {
    constexpr uint8_t commands[] = {1,6,12,16,17,18,19,20,22,27,28,30,32,40,41,42,45,46,48,61,62,63};
    for (uint8_t function : commands) {
        for (unsigned i = 0; i < 96; ++i) {
            CPUState original;
            randomize_gte(original);
            original.gte_ts_done = 1234;
            CPUState expected = original, actual = original;
            const uint32_t cmd = (random_u32() & ~63u) | function;
            gte_execute(&expected, cmd);
            const uint32_t calls = g_test_gte_set_calls;
            psx_hle_gte_execute(&actual, cmd);
            CHECK(same_gte(expected, actual));
            CHECK(!std::memcmp(original.gpr, actual.gpr, sizeof actual.gpr));
            CHECK(g_test_gte_set_calls == calls + (PSX_EXECUTION_ENHANCED ? 0 : 1));
            if (PSX_EXECUTION_ENHANCED) CHECK(actual.gte_ts_done == 1234);
        }
    }
    /* The untimed entry must still visit the camera interpolation observer. */
    CPUState cpu{};
    g_psx_projection_command = substitute_projection;
    projection_callback_calls = 0;
    psx_hle_gte_execute(&cpu, 0x80001u);
    g_psx_projection_command = nullptr;
    CHECK(projection_callback_calls == 1);
    /* A normal call immediately after HLE still owns faithful latency. */
    const uint32_t calls = g_test_gte_set_calls;
    gte_execute(&cpu, 6);
    CHECK(g_test_gte_set_calls == calls + 1);
    return 0;
}

int test_command_marshaling() {
    constexpr std::array<uint8_t, 22> kFunctions = {
        0x01, 0x06, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14,
        0x16, 0x1B, 0x1C, 0x1E, 0x20, 0x28, 0x29, 0x2A,
        0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F,
    };

    for (uint8_t function : kFunctions) {
        for (unsigned iteration = 0; iteration < 96; ++iteration) {
            CPUState seed;
            randomize_gte(seed);  // deliberately raw/noncanonical legacy state
            CPUState expected = seed;
            CPUState actual = seed;
            const uint32_t cmd = (random_u32() & ~0x3Fu) | function;
            gte_test_execute_reference(&expected, cmd);
            gte_execute(&actual, cmd);
            if (!same_gte(expected, actual))
                return fail_state("command marshal", iteration, function, cmd,
                                  expected, actual);
            if (actual.gte_data[15] != actual.gte_data[14] ||
                actual.gte_data[28] != actual.gte_data[29] ||
                actual.gte_data[31] != gte_read_data(&actual, 31))
                return fail_value("command canonical aliases", iteration,
                                  function, cmd, actual.gte_data[14],
                                  actual.gte_data[15]);
        }
    }

    /* MVMVA's matrix/vector/translation dependencies are all selected by the
     * command word. Exhaust every selector plus sf/lm combination explicitly. */
    for (uint32_t mx = 0; mx < 4; ++mx) {
        for (uint32_t vv = 0; vv < 4; ++vv) {
            for (uint32_t tv = 0; tv < 4; ++tv) {
                for (uint32_t sf = 0; sf < 2; ++sf) {
                    for (uint32_t lm = 0; lm < 2; ++lm) {
                        CPUState seed;
                        randomize_gte(seed);
                        CPUState expected = seed;
                        CPUState actual = seed;
                        const uint32_t cmd = 0x12u | (mx << 17) | (vv << 15) |
                                             (tv << 13) | (sf << 19) | (lm << 10);
                        gte_test_execute_reference(&expected, cmd);
                        gte_execute(&actual, cmd);
                        if (!same_gte(expected, actual))
                            return fail_state("MVMVA selectors", mx * 16u + vv * 4u + tv,
                                              static_cast<unsigned>(sf * 2u + lm),
                                              cmd, expected, actual);
                    }
                }
            }
        }
    }

    /* Stateful command streams catch FIFO/alias state that single-command
     * comparisons can accidentally reinitialize away. */
    for (unsigned iteration = 0; iteration < 64; ++iteration) {
        CPUState expected;
        randomize_gte(expected);
        CPUState actual = expected;
        for (unsigned step = 0; step < 128; ++step) {
            const uint8_t function = kFunctions[random_u32() % kFunctions.size()];
            const uint32_t cmd = (random_u32() & ~0x3Fu) | function;
            gte_test_execute_reference(&expected, cmd);
            gte_execute(&actual, cmd);
            if (!same_gte(expected, actual))
                return fail_state("command stream", iteration, function, cmd,
                                  expected, actual);
        }
    }

    /* Projection commands update a host-only precise-SXY FIFO. Reset that
     * global state around each path so the old and direct marshalers can be
     * compared independently instead of executing twice on one timeline. */
    for (uint8_t function : kFunctions) {
        CPUState seed;
        randomize_gte(seed);
        CPUState expected = seed;
        CPUState actual = seed;
        const uint32_t cmd = (random_u32() & ~0x3Fu) | function;
        seed_precise_snapshot();
        gte_test_execute_reference(&expected, cmd);
        const auto expected_precise = precise_snapshot();
        seed_precise_snapshot();
        gte_execute(&actual, cmd);
        const auto actual_precise = precise_snapshot();
        if (!same_precise(expected_precise, actual_precise))
            return fail_value("command precise provenance", 0, function,
                              cmd, 1u, 0u);
    }
    return 0;
}

int test_command_timing_hook() {
    CPUState cpu{};
    g_test_cycle = 100u;
    cpu.gte_ts_done = 175u;
    g_test_gte_set_calls = 0;
    g_test_gte_last_latency = 0;
    gte_execute(&cpu, 0x06u);
    if (g_test_gte_set_calls != 1u || g_test_gte_last_latency != 13u ||
        cpu.gte_ts_done != 188u)
        return fail_value("command timing serialization", 0, 0x06u, 0,
                          188u, static_cast<uint32_t>(cpu.gte_ts_done));
    gte_execute(&cpu, 0x30u);
    if (g_test_gte_set_calls != 2u || g_test_gte_last_latency != 55u ||
        cpu.gte_ts_done != 243u)
        return fail_value("back-to-back command timing", 0, 0x30u, 0,
                          243u, static_cast<uint32_t>(cpu.gte_ts_done));
    return 0;
}

int test_precise_sxy_invalidation() {
    CPUState cpu{};
    for (uint8_t reg = 0; reg < 32; ++reg) {
        gte_test_set_precise_valid_mask(0xFu);
        gte_write_data(&cpu, reg, 0x12345678u);
        /* The PGXP shadow drops exactly the register(s) the guest wrote:
         * SXY0/SXY1 clear their own slot; SXY2 and SXYP clear both mirrors
         * (regs 14+15). The untouched slots stay live — their register
         * values did not change (SXYP's hardware FIFO shift makes 12/13
         * stale in VALUE, which validate-on-read handles at use; liveness
         * alone is not a correctness claim). */
        uint32_t expected = 0xFu;
        if (reg == 12 || reg == 13) expected &= ~(1u << (reg - 12));
        else if (reg == 14 || reg == 15) expected &= ~0xCu;
        const uint32_t actual = gte_test_get_precise_valid_mask();
        if (actual != expected)
            return fail_value("precise SXY invalidation", 0, reg,
                              0x12345678u, expected, actual);
    }

    for (uint16_t wide_reg = 32; wide_reg <= 255; ++wide_reg) {
        const uint8_t reg = static_cast<uint8_t>(wide_reg);
        gte_test_set_precise_valid_mask(0xFu);
        gte_write_data(&cpu, reg, 0x87654321u);
        if (gte_test_get_precise_valid_mask() != 0xFu)
            return fail_value("invalid data preserves precise SXY", 0, reg,
                              0x87654321u, 0xFu,
                              gte_test_get_precise_valid_mask());
    }

    for (uint16_t wide_reg = 0; wide_reg <= 255; ++wide_reg) {
        const uint8_t reg = static_cast<uint8_t>(wide_reg);
        gte_test_set_precise_valid_mask(0xFu);
        gte_write_ctrl(&cpu, reg, 0xCAFEBABEu);
        if (gte_test_get_precise_valid_mask() != 0xFu)
            return fail_value("ctrl preserves precise SXY", 0, reg,
                              0xCAFEBABEu, 0xFu,
                              gte_test_get_precise_valid_mask());
    }

    gte_test_set_precise_valid_mask(0xFu);
    gte_test_set_timeline_generations(41u, 73u);
    gte_canonicalize_cpu_state(&cpu);
    if (gte_test_get_precise_valid_mask() != 0u)
        return fail_value("canonicalize invalidates precise SXY", 0, 0,
                          0, 0, gte_test_get_precise_valid_mask());
    if (gte_test_get_precision_generation() != 42u)
        return fail_value("canonicalize advances precision generation", 0, 0,
                          0, 42u, gte_test_get_precision_generation());
    if (gte_test_get_geometry_generation() != 74u)
        return fail_value("canonicalize advances geometry generation", 0, 0,
                          0, 74u, gte_test_get_geometry_generation());

    /* Generation zero is reserved. Wrapping must clear stale generations and
     * restart at one rather than resurrecting ancient generation-one entries. */
    gte_test_set_precise_valid_mask(0xFu);
    gte_test_set_timeline_generations(0xFFFFFFFFu, 0xFFFFFFFFu);
    gte_precision_timeline_invalidate();
    if (gte_test_get_precise_valid_mask() != 0u ||
        gte_test_get_precision_generation() != 1u ||
        gte_test_get_geometry_generation() != 1u)
        return fail_value("timeline generation wrap", 0, 0, 0, 1u,
                          gte_test_get_precision_generation());
    return 0;
}

int test_precise_nclip_is_title_scoped() {
    CPUState cpu{};
    gte_precision_tracking_set(1);
    g_test_precise_nclip_enabled = 1;

    /* Native determinant is +1. The validated 16.16 positions have a negative
     * exact determinant, so only the title-scoped branch helper may see it. */
    const uint32_t packed[3] = {0x00000001u, 0xFFFFFFFEu, 0xFFFFFFFFu};
    const int32_t x16[3] = {101245, -109694, -34340};
    const int32_t y16[3] = {19509, -59658, -20365};
    for (uint32_t i = 0; i < 3; ++i) {
        cpu.gte_data[12 + i] = packed[i];
        gte_test_seed_precise_projection(i, packed[i], x16[i], y16[i], 100);
    }
    gte_nclip_stats_reset();
    g_debug_last_store_pc = 0x80012340u;
    uint64_t hit0 = 0, fallback0 = 0, disagree0 = 0;
    gte_nclip_precise_stats(&hit0, &fallback0, &disagree0);
    gte_execute(&cpu, 0x06u);
    uint64_t hit1 = 0, fallback1 = 0, disagree1 = 0;
    gte_nclip_precise_stats(&hit1, &fallback1, &disagree1);
    if (cpu.gte_data[24] != 1u || hit1 != hit0 + 1u ||
        fallback1 != fallback0 || disagree1 != disagree0 + 1u)
        return fail_value("precise NCLIP preserves guest MAC0", 0, 0x06u,
                          0, 1u, cpu.gte_data[24]);
    /* The branch consumer sees the precise (negative) sign only for the MAC0
     * the NCLIP produced; any other value keeps its native sign. Every branch
     * shape tests this one sign. */
    if (gte_nclip_exact_sign(1, 0x80011000u) != -1 ||
        gte_nclip_exact_sign(2, 0x80011000u) != 1 ||
        gte_nclip_exact_sign(0, 0x80011004u) != 0)
        return fail_value("title-scoped precise NCLIP predicate", 0, 0x06u,
                          0, 1u, 0u);
    {
        GteNclipSiteStat sites[GTE_NCLIP_STAT_CAP];
        const int n = gte_nclip_site_stats(sites, GTE_NCLIP_STAT_CAP);
        uint32_t evals = 0, flips = 0, falls = 0;
        for (int i = 0; i < n; ++i)
            if (sites[i].pc == 0x80011000u) {
                evals = sites[i].evals; flips = sites[i].flips;
                falls = sites[i].fallbacks;
            }
        if (evals != 2u || flips != 1u || falls != 1u)
            return fail_value("exact NCLIP site attribution", 0, 0x06u,
                              0, 2u, evals);
        GteNclipFuncStat funcs[GTE_NCLIP_STAT_CAP];
        const int nf = gte_nclip_func_stats(funcs, GTE_NCLIP_STAT_CAP);
        uint32_t fn = 0, fd = 0;
        for (int i = 0; i < nf; ++i)
            if (funcs[i].func == 0x80012340u) { fn = funcs[i].nclips; fd = funcs[i].disagree; }
        if (fn != 1u || fd != 1u)
            return fail_value("NCLIP producer attribution", 0, 0x06u, 0, 1u, fn);
    }

    /* A stale packed-word shadow must fail closed to the native sign and count
     * as a fallback, never as a precise hit. */
    gte_test_seed_precise_projection(0, packed[0] ^ 1u,
                                     x16[0], y16[0], 100);
    gte_execute(&cpu, 0x06u);
    uint64_t hit2 = 0, fallback2 = 0, disagree2 = 0;
    gte_nclip_precise_stats(&hit2, &fallback2, &disagree2);
    g_test_precise_nclip_enabled = 0;
    if (cpu.gte_data[24] != 1u || hit2 != hit1 ||
        fallback2 != fallback1 + 1u || disagree2 != disagree1 ||
        gte_nclip_exact_sign(1, 0x80011000u) != 1)
        return fail_value("stale precise NCLIP falls back natively", 0, 0x06u,
                          0, 1u, cpu.gte_data[24]);
    return 0;
}

int test_saturated_nclip_keeps_architectural_result() {
    CPUState cpu{};
    gte_precision_tracking_set(1);
    g_test_precise_nclip_enabled = 1;
    cpu.gte_ctrl[26] = 300;
    const int px[] = {1100, 1200, 1300}, py[] = {100, 200, 400};
    for (unsigned i=0;i<3;i++) {
        uint32_t packed = 1023u | ((uint32_t)py[i] << 16);
        cpu.gte_data[12+i] = packed; cpu.gte_data[17+i] = 1000;
        gte_test_seed_precise_projection(i, packed, px[i]*65536, py[i]*65536, 1000);
    }
    gte_execute(&cpu, 6);
    int sign = 0;
    if (cpu.gte_data[24] != 0 || !gte_nclip_native_wide_sign(0, &sign) || sign != 1 ||
        gte_nclip_native_wide_sign(1, &sign))
        return fail_value("saturated winding rescue preserves native zero MAC0",0,6,0,0,cpu.gte_data[24]);
    CHECK(!gte_nclip_zero_positive(0, 0x80011008u));
    /* Both quad MAC0 values can saturate to the same zero. Keep each exact
     * sign separately: the preceding triangle points forward, latest back. */
    for (unsigned i=0;i<3;i++) {
        uint32_t packed=1023u | ((uint32_t)py[2-i]<<16);
        cpu.gte_data[12+i]=packed;
        gte_test_seed_precise_projection(i,packed,px[2-i]*65536,py[2-i]*65536,1000);
    }
    gte_execute(&cpu,6);
    if (cpu.gte_data[24]!=0 || !gte_nclip_native_wide_sign(0,&sign) || sign!=-1 ||
        !gte_nclip_native_wide_previous_sign(0,&sign) || sign!=1 ||
        gte_nclip_native_wide_previous_sign(1,&sign))
        return fail_value("quad retains independent saturated winding results",0,6,0,0,cpu.gte_data[24]);
    gte_execute(&cpu,6);
    if (!gte_nclip_native_wide_previous_sign(0,&sign) || sign!=-1)
        return fail_value("preceding winding advances one command only",0,6,0,0,1);
    cpu.gte_data[17] = 10;
    gte_execute(&cpu, 6);
    if (gte_nclip_native_wide_sign(0, &sign) || cpu.gte_data[24] != 0)
        return fail_value("near depth retains native reject",0,6,0,0,cpu.gte_data[24]);
    cpu.gte_data[17] = 1000;
    gte_execute(&cpu, 6);
    gte_precision_timeline_invalidate();
    if (gte_nclip_native_wide_sign(0, &sign) || gte_nclip_native_wide_previous_sign(0,&sign))
        return fail_value("timeline invalidation clears winding rescue",0,6,0,0,1);
    gte_test_seed_precise_projection(0, cpu.gte_data[12] ^ 1u, px[0]*65536, py[0]*65536, 1000);
    gte_execute(&cpu, 6);
    if (gte_nclip_native_wide_sign(0, &sign))
        return fail_value("stale NCLIP projection refuses rescue",0,6,0,0,1);
    g_test_precise_nclip_enabled = 0;
    return 0;
}

int test_zero_nclip_requires_current_safe_projection() {
    CPUState cpu{};
    gte_precision_tracking_set(1);
    g_test_precise_nclip_enabled = 1;
    pgxp_set_culling(0);
    gte_nclip_exact_set_enabled(1);
    /* Captured terrain quad801ACC44, first three vertices. Both first vertices
     * quantize to (221,45), although their precise face has positive area. */
    const int16_t rt[] = {3518,0,-2096,-375,4029,-629,2061,732,3460};
    for (unsigned i = 0; i < 4; ++i)
        cpu.gte_ctrl[i] = (uint16_t)rt[2*i] | ((uint32_t)(uint16_t)rt[2*i+1] << 16);
    cpu.gte_ctrl[4] = (uint32_t)(int32_t)rt[8];
    cpu.gte_ctrl[5] = (uint32_t)-4521; cpu.gte_ctrl[6] = 2122;
    cpu.gte_ctrl[7] = (uint32_t)-5132;
    cpu.gte_ctrl[24] = 160u << 16; cpu.gte_ctrl[25] = 120u << 16;
    cpu.gte_ctrl[26] = 350;
    const int16_t v[3][3] = {{9970,-1315,6223},{9938,-1315,6172},{9970,-894,6223}};
    auto project = [&](bool reverse = false) {
        for (unsigned i = 0; i < 3; ++i) {
            unsigned j = reverse ? 2-i : i;
            cpu.gte_data[2*i] = (uint16_t)v[j][0] | ((uint32_t)(uint16_t)v[j][1] << 16);
            cpu.gte_data[2*i+1] = (uint32_t)(int32_t)v[j][2];
        }
        gte_execute(&cpu, 0x00280030u);
        gte_execute(&cpu, 6);
    };
    const uint32_t site = 0x8013FF14u;
    gte_nclip_stats_reset();
    project();
    CHECK(cpu.gte_data[24] == 0);
    CHECK(gte_nclip_zero_positive(0, site));
    GteNclipSiteStat stat{};
    CHECK(gte_nclip_site_stats(&stat, 1) == 1 && stat.pc == site &&
          stat.evals == 1 && stat.flips == 1 && stat.fallbacks == 0);
    CHECK(!gte_nclip_zero_positive(-1, site) && !gte_nclip_zero_positive(1, site));
    CHECK(cpu.gte_data[24] == 0);
    gte_nclip_exact_set_enabled(0);
    CHECK(!gte_nclip_zero_positive(0, site));
    gte_nclip_exact_set_enabled(1);
    project(true);
    CHECK(cpu.gte_data[24] == 0 && !gte_nclip_zero_positive(0, site));
    project();
    gte_execute(&cpu, 0x2D);
    CHECK(!gte_nclip_zero_positive(0, site));
    project();
    gte_write_data(&cpu, 24, 0);
    CHECK(!gte_nclip_zero_positive(0, site));
    project();
    gte_write_ctrl(&cpu, 26, 350);
    CHECK(!gte_nclip_zero_positive(0, site));
    project();
    cpu.gte_data[17] = 10;
    gte_execute(&cpu, 6);
    CHECK(!gte_nclip_zero_positive(0, site));
    project();
    gte_precision_speculative_begin();
    CHECK(!gte_nclip_zero_positive(0, site));
    gte_precision_speculative_end();
    gte_precision_timeline_invalidate();
    CHECK(!gte_nclip_zero_positive(0, site));
    project();
    gte_test_seed_precise_projection(0, cpu.gte_data[12] ^ 1u, 221 << 16, 45 << 16, 4906);
    gte_execute(&cpu, 6);
    CHECK(!gte_nclip_zero_positive(0, site));
    g_test_precise_nclip_enabled = 0;
    return 0;
}

/* PGXP precise culling (docs/ENHANCEMENTS.md G1.12): NCLIP's MAC0 takes the
 * exact sign only while geometry correction and culling are armed, only when
 * the three SXY shadows are believed, and never in a compared pass. */
int test_pgxp_culling() {
    gte_precision_tracking_set(1);
    gte_geometry_correction_set(1);
    pgxp_set_preserve_projection(1);
    pgxp_set_tolerance(-1.0f);

    /* A far road row: the three vertices all round to y = 113 (MAC0 0, the
     * game culls it); exact y 112.64 / 113.12 / 113.83 (positive area). */
    const uint32_t packed[3] = {(113u << 16) | 100u, (113u << 16) | 220u,
                                (113u << 16) | 160u};
    const int32_t x16[3] = {100 << 16, 220 << 16, 160 << 16};
    const int32_t y16[3] = {(112 << 16) + 41943, (113 << 16) + 7864,
                            (113 << 16) + 54394};
    const int64_t cross =
        ((int64_t)x16[1] - x16[0]) * ((int64_t)y16[2] - y16[0]) -
        ((int64_t)y16[1] - y16[0]) * ((int64_t)x16[2] - x16[0]);
    const int32_t area = (int32_t)((uint64_t)(cross + (1ll << 31)) >> 32);
    auto nclip = [&](const uint32_t (&w)[3], const int32_t (&xs)[3],
                     const int32_t (&ys)[3], uint32_t *flag) {
        CPUState cpu{};
        for (uint32_t i = 0; i < 3; ++i) {
            cpu.gte_data[12 + i] = w[i];
            gte_test_seed_precise_projection(i, w[i], xs[i], ys[i], 100);
        }
        gte_execute(&cpu, 0x06u);
        if (flag) *flag = cpu.gte_ctrl[31];
        return static_cast<int32_t>(cpu.gte_data[24]);
    };
    PGXPStats a{}, b{};
    pgxp_get_stats(&a);
    uint32_t flag_off = 0, flag_on = 0;

    /* Culling off: the guest sees the hardware's 0; the disagreement is
     * counted (the crack exposure), not corrected. */
    pgxp_set_culling(0);
    if (int32_t m = nclip(packed, x16, y16, &flag_off); m != 0)
        return fail_value("culling off keeps MAC0", 0, 0x06u, 0, 0u, (uint32_t)m);
    pgxp_get_stats(&b);
    if (b.nclip_disagree != a.nclip_disagree + 1 ||
        b.nclip_corrected != a.nclip_corrected)
        return fail_value("culling off counts the disagreement", 0, 0x06u, 0,
                          1u, (uint32_t)(b.nclip_disagree - a.nclip_disagree));

    /* Culling on: MAC0 is the exact doubled area, rounded (>= 1). */
    pgxp_set_culling(1);
    if (area < 1)
        return fail_value("test area", 0, 0x06u, 0, 1u, (uint32_t)area);
    if (int32_t m = nclip(packed, x16, y16, &flag_on); m != area)
        return fail_value("culling on takes the exact sign", 0, 0x06u, 0,
                          (uint32_t)area, (uint32_t)m);
    if (flag_on != flag_off)
        return fail_value("culling keeps FLAG", 0, 0x06u, 0, flag_off, flag_on);
    pgxp_get_stats(&a);
    if (a.nclip_corrected != b.nclip_corrected + 1)
        return fail_value("culling on counts the correction", 0, 0x06u, 0, 1u,
                          (uint32_t)(a.nclip_corrected - b.nclip_corrected));

    /* Reversed winding: the exact sign is negative, magnitude at least 1. */
    {
        const uint32_t w[3] = {packed[0], packed[2], packed[1]};
        const int32_t xs[3] = {x16[0], x16[2], x16[1]};
        const int32_t ys[3] = {y16[0], y16[2], y16[1]};
        if (int32_t m = nclip(w, xs, ys, nullptr); m != -area)
            return fail_value("culling keeps the reversed sign", 0, 0x06u, 0,
                              (uint32_t)-area, (uint32_t)m);
    }
    /* A native-positive sliver whose exact winding is reversed: the game
     * now sees it back-facing. Native (0,0),(10,0),(5,1): MAC0 +10; exact
     * y2 = -0.25 (inside the window). */
    {
        const uint32_t w[3] = {0u, 10u, (1u << 16) | 5u};
        const int32_t xs[3] = {0, 10 << 16, 5 << 16};
        const int32_t ys[3] = {0, 0, -(1 << 14)};
        pgxp_set_culling(0);
        if (int32_t m = nclip(w, xs, ys, nullptr); m != 10)
            return fail_value("native sliver MAC0", 0, 0x06u, 0, 10u, (uint32_t)m);
        pgxp_set_culling(1);
        if (int32_t m = nclip(w, xs, ys, nullptr); m != -3)
            return fail_value("reversed sliver MAC0", 0, 0x06u, 0,
                              (uint32_t)-3, (uint32_t)m);
    }
    /* Signs that agree keep the integer MAC0 exactly. */
    {
        const uint32_t w[3] = {0u, 10u, (4u << 16) | 5u};
        const int32_t xs[3] = {0, 10 << 16, 5 << 16};
        const int32_t ys[3] = {1 << 15, 0, (4 << 16) + (1 << 15)};
        if (int32_t m = nclip(w, xs, ys, nullptr); m != 40)
            return fail_value("agreeing sign keeps MAC0", 0, 0x06u, 0, 40u,
                              (uint32_t)m);
    }
    /* A stale shadow (the register word changed under it) fails closed. */
    {
        const uint32_t w[3] = {packed[0], packed[1], packed[2]};
        CPUState cpu{};
        for (uint32_t i = 0; i < 3; ++i) {
            cpu.gte_data[12 + i] = w[i];
            gte_test_seed_precise_projection(i, w[i] ^ (i == 2 ? 1u : 0u),
                                             x16[i], y16[i], 100);
        }
        gte_execute(&cpu, 0x06u);
        if (cpu.gte_data[24] != 0u)
            return fail_value("stale shadow keeps MAC0", 0, 0x06u, 0, 0u,
                              cpu.gte_data[24]);
    }
    /* Compared passes see the hardware result: the overlay shadow diff (its
     * interpreter pass included) and a speculative pass. */
    g_test_shadow_diff = 1;
    if (int32_t m = nclip(packed, x16, y16, nullptr); m != 0)
        return fail_value("shadow diff holds culling off", 0, 0x06u, 0, 0u,
                          (uint32_t)m);
    g_test_shadow_diff = 0;
    gte_precision_speculative_begin();
    {
        CPUState cpu{};
        for (uint32_t i = 0; i < 3; ++i) cpu.gte_data[12 + i] = packed[i];
        gte_execute(&cpu, 0x06u);
        gte_precision_speculative_end();
        if (cpu.gte_data[24] != 0u)
            return fail_value("speculative pass holds culling off", 0, 0x06u,
                              0, 0u, cpu.gte_data[24]);
    }
    /* Geometry correction off: nothing is corrected or counted. */
    gte_geometry_correction_set(0);
    pgxp_get_stats(&a);
    if (int32_t m = nclip(packed, x16, y16, nullptr); m != 0)
        return fail_value("geometry off keeps MAC0", 0, 0x06u, 0, 0u, (uint32_t)m);
    pgxp_get_stats(&b);
    if (b.nclip_precise != a.nclip_precise || b.nclip_disagree != a.nclip_disagree)
        return fail_value("geometry off counts nothing", 0, 0x06u, 0, 0u,
                          (uint32_t)(b.nclip_precise - a.nclip_precise));
    pgxp_set_culling(0);
    pgxp_set_preserve_projection(0);
    return 0;
}

int test_precision_speculative_transaction() {
    constexpr uint32_t address = 0x00123450u;
    constexpr uint32_t packed = 0x00420021u;
    constexpr int32_t x16 = 0x00123456;
    constexpr int32_t y16 = -0x00034567;
    constexpr uint16_t z = 0x4567u;

    gte_precision_tracking_set(1);
    gte_geometry_correction_set(1);
    gte_test_set_timeline_generations(41u, 73u);
    gte_test_seed_precise_projection(2, packed, x16, y16, z);
    gte_precision_store_word(address, 14);
    gte_test_seed_geometry(packed, x16, y16);

    int32_t got_x = 0, got_y = 0;
    uint16_t got_z = 0;
    if (!gte_precision_load_word(address, packed, &got_x, &got_y, &got_z) ||
        got_x != x16 || got_y != y16 || got_z != z)
        return fail_value("precision seed lookup", 0, 14, packed, z, got_z);
    if (!gte_geometry_correction_lookup(packed, &got_x, &got_y) ||
        got_x != x16 || got_y != y16)
        return fail_value("geometry seed lookup", 0, 0, packed,
                          static_cast<uint32_t>(x16), static_cast<uint32_t>(got_x));

    gte_precision_speculative_begin();
    gte_precision_speculative_begin();
    if (gte_precision_load_word(address, packed, &got_x, &got_y, &got_z) != 0)
        return fail_value("speculative precision read", 0, 0, packed, 0, 1);
    if (gte_geometry_correction_lookup(packed, &got_x, &got_y) != 0)
        return fail_value("speculative geometry read", 0, 0, packed, 0, 1);
    gte_precision_invalidate_word(address);  // suppressed: must not age live entry
    CPUState cpu{};
    gte_write_data(&cpu, 14, 0xDEADBEEFu);  // mutates speculative SXY only
    gte_precision_speculative_end();
    if (gte_precision_load_word(address, packed, &got_x, &got_y, &got_z) != 0)
        return fail_value("nested speculation remains isolated", 0, 0,
                          packed, 0, 1);
    gte_precision_speculative_end();

    if (gte_test_get_precise_valid_mask() != 0x4u)
        return fail_value("speculative SXY restore", 0, 0, 0, 0x4u,
                          gte_test_get_precise_valid_mask());
    if (!gte_precision_load_word(address, packed, &got_x, &got_y, &got_z) ||
        got_x != x16 || got_y != y16 || got_z != z)
        return fail_value("authoritative precision survives", 0, 0, packed,
                          z, got_z);
    if (!gte_geometry_correction_lookup(packed, &got_x, &got_y) ||
        got_x != x16 || got_y != y16)
        return fail_value("authoritative geometry survives", 0, 0, packed,
                          static_cast<uint32_t>(x16), static_cast<uint32_t>(got_x));

    /* A raw savestate/import restore is authoritative even when host polling
     * reaches it during speculation. Defer its timeline invalidation until the
     * outer transaction closes, but never restore pre-load provenance. */
    gte_precision_speculative_begin();
    gte_precision_speculative_begin();
    gte_precision_timeline_invalidate();
    if (gte_test_get_precision_generation() != 41u ||
        gte_test_get_geometry_generation() != 73u)
        return fail_value("deferred timeline invalidation", 0, 0, 0, 41u,
                          gte_test_get_precision_generation());
    gte_precision_speculative_end();
    if (gte_test_get_precision_generation() != 41u)
        return fail_value("nested timeline remains deferred", 0, 0, 0, 41u,
                          gte_test_get_precision_generation());
    gte_precision_speculative_end();
    if (gte_test_get_precise_valid_mask() != 0u ||
        gte_test_get_precision_generation() != 42u ||
        gte_test_get_geometry_generation() != 74u ||
        gte_precision_load_word(address, packed, nullptr, nullptr, nullptr) != 0 ||
        gte_geometry_correction_lookup(packed, nullptr, nullptr) != 0)
        return fail_value("authoritative restore invalidation", 0, 0, 0, 42u,
                          gte_test_get_precision_generation());

    /* Exercise each cache's wrap independently and prove stale entries miss,
     * rather than checking generation counters alone. */
    gte_test_set_timeline_generations(0xFFFFFFFFu, 90u);
    gte_test_seed_precise_projection(2, packed, x16, y16, z);
    gte_precision_store_word(address, 14);
    gte_test_seed_geometry(packed, x16, y16);
    gte_precision_timeline_invalidate();
    if (gte_test_get_precision_generation() != 1u ||
        gte_test_get_geometry_generation() != 91u ||
        gte_precision_load_word(address, packed, nullptr, nullptr, nullptr) != 0 ||
        gte_geometry_correction_lookup(packed, nullptr, nullptr) != 0)
        return fail_value("independent precision wrap", 0, 0, 0, 1u,
                          gte_test_get_precision_generation());

    gte_test_set_timeline_generations(120u, 0xFFFFFFFFu);
    gte_test_seed_precise_projection(2, packed, x16, y16, z);
    gte_precision_store_word(address, 14);
    gte_test_seed_geometry(packed, x16, y16);
    gte_precision_timeline_invalidate();
    if (gte_test_get_precision_generation() != 121u ||
        gte_test_get_geometry_generation() != 1u ||
        gte_precision_load_word(address, packed, nullptr, nullptr, nullptr) != 0 ||
        gte_geometry_correction_lookup(packed, nullptr, nullptr) != 0)
        return fail_value("independent geometry wrap", 0, 0, 0, 1u,
                          gte_test_get_geometry_generation());
    return 0;
}

int test_render_pose() {
    PSXModRenderView pose = {};
    pose.struct_size = sizeof pose;
    pose.rotation_q12[2] = 4096; pose.rotation_q12[4] = 4096;
    pose.rotation_q12[6] = -4096;
    GTEState g;
    g.RT[0][0] = g.RT[1][1] = g.RT[2][2] = 4096;
    g.V0[0] = -800; g.V0[2] = 200; g.H = 400;
    gte_render_pose_set(&pose);
    PSXRecomp::GTE::gte_rtps_internal(&g, g.V0, true);
    if (g.MAC1 != 200 || g.MAC3 != 800 || g.TR[0] != 0)
        return fail_value("rigid rotation before division",0,0,0,800,g.MAC3);
    pose = {}; pose.struct_size = sizeof pose;
    pose.rotation_q12[0] = pose.rotation_q12[4] = pose.rotation_q12[8] = 4096;
    pose.projection = 1; pose.fx_q16 = 200 << 16; pose.fy_q16 = 100 << 16;
    pose.cx_delta_q16 = 10 << 16; pose.cy_delta_q16 = -5 * 65536;
    g = GTEState(); g.RT[0][0] = g.RT[1][1] = g.RT[2][2] = 4096;
    g.V0[0] = 80; g.V0[1] = 160; g.V0[2] = 800;
    gte_render_pose_set(&pose);
    PSXRecomp::GTE::gte_rtps_internal(&g, g.V0, true);
    if ((int16_t)g.SXY[2] != 30 || (int16_t)(g.SXY[2] >> 16) != 15)
        return fail_value("asymmetric projection",0,0,0,30,g.SXY[2] & 65535);
    pose.projection_h_ref = 400; g.H = 133;
    gte_render_pose_set(&pose);
    PSXRecomp::GTE::gte_rtps_internal(&g, g.V0, true);
    if ((int16_t)g.SXY[2] != 16 || (int16_t)(g.SXY[2] >> 16) != 1)
        return fail_value("authored focal ratio",0,0,0,16,g.SXY[2] & 65535);
    pose = {}; gte_render_pose_set(&pose);
    /* Composition with [video] fov_scale (scaled guest H): an absolute override
     * (projection_h_ref == 0) replaces the guest projection entirely, so the
     * result does not depend on fov_scale; with projection_h_ref the focal
     * lengths follow the fov-scaled guest H. Outside the override (pose reset)
     * the guest path, including fov_scale, is untouched. */
    {
        GTEState base;
        base.RT[0][0] = base.RT[1][1] = base.RT[2][2] = 4096;
        base.V0[0] = 80; base.V0[1] = 160; base.V0[2] = 800;
        base.H = 400; base.OFX = 256 << 16; base.OFY = 120 << 16;
        PSXModRenderView ov = {}; ov.struct_size = sizeof ov;
        ov.rotation_q12[0] = ov.rotation_q12[4] = ov.rotation_q12[8] = 4096;
        ov.projection = 1; ov.fx_q16 = 200 << 16; ov.fy_q16 = 100 << 16;
        GTEState a = base, b = base, c = base, guest1 = base, guest2 = base;
        gte_set_fov_scale(1, 1);
        gte_render_pose_set(&ov);
        PSXRecomp::GTE::gte_rtps_internal(&a, a.V0, true);
        gte_set_fov_scale(1, 2);
        PSXRecomp::GTE::gte_rtps_internal(&b, b.V0, true);
        if (a.SXY[2] != b.SXY[2])
            return fail_value("absolute override independent of fov_scale",0,0,0,a.SXY[2],b.SXY[2]);
        ov.projection_h_ref = 400;
        gte_render_pose_set(&ov);                     /* scaled H = 200 -> half focal */
        PSXRecomp::GTE::gte_rtps_internal(&c, c.V0, true);
        if ((int16_t)c.SXY[2] != 256 + 10 || (int16_t)(c.SXY[2] >> 16) != 120 + 10)
            return fail_value("h_ref override follows fov-scaled H",0,0,0,266,c.SXY[2] & 65535);
        PSXModRenderView none = {}; gte_render_pose_set(&none);
        PSXRecomp::GTE::gte_rtps_internal(&guest2, guest2.V0, true);
        gte_set_fov_scale(1, 1);
        PSXRecomp::GTE::gte_rtps_internal(&guest1, guest1.V0, true);
        if (guest1.SXY[2] == guest2.SXY[2])
            return fail_value("guest path keeps fov_scale after override reset",0,0,0,guest1.SXY[2],guest2.SXY[2]);
        if (guest1.SXY[2] == a.SXY[2])
            return fail_value("override differs from guest projection (sanity)",0,0,0,guest1.SXY[2],a.SXY[2]);
    }
    gte_set_fov_scale(1, 1);
    std::puts("PASS: rigid rotation and asymmetric projection before division");
    return 0;
}

int test_render_view_parallax() {
    const int32_t zero[3] = {0, 0, 0}, offset[3] = {24, 0, 0};
    int shifts[2] = {};
    for (int i = 0; i < 2; ++i) {
        GTEState base;
        base.RT[0][0] = base.RT[1][1] = base.RT[2][2] = 4096;
        base.H = 400; base.OFX = 256 << 16; base.OFY = 120 << 16;
        base.V0[2] = i ? 3200 : 800;
        GTEState baseline = base, eye = base, expected = base, repeat = base;
        gte_render_view_set(zero);
        PSXRecomp::GTE::gte_rtps_internal(&baseline, baseline.V0, true);
        expected.TR[0] += 24;
        PSXRecomp::GTE::gte_rtps_internal(&expected, expected.V0, true);
        gte_render_view_set(offset);
        PSXRecomp::GTE::gte_rtps_internal(&eye, eye.V0, true);
        PSXRecomp::GTE::gte_rtps_internal(&repeat, repeat.V0, true);
        if (eye.SXY[2] != expected.SXY[2] || eye.SZ[3] != expected.SZ[3] ||
            eye.MAC1 != expected.MAC1 || eye.TR[0] != base.TR[0] ||
            eye.SXY[2] != repeat.SXY[2]) {
            gte_render_view_set(zero);
            return fail_value("render view equals pre-divide translation", 0, 0, 0,
                              expected.SXY[2], eye.SXY[2]);
        }
        shifts[i] = (int16_t)eye.SXY[2] - (int16_t)baseline.SXY[2];
    }
    gte_render_view_set(zero);
    if (shifts[0] != 12 || shifts[1] != 3)
        return fail_value("near/far render-view displacement", 0, 0, 0, 12u,
                          static_cast<uint32_t>(shifts[0]));
    std::puts("PASS: view offset gives 12px at Z=800, 3px at Z=3200; TR unchanged; no accumulation");
    return 0;
}

int test_pgxp_probe_does_not_count_geometry_lookup() {
    constexpr uint32_t packed = (80u << 16) | 160u;
    constexpr int32_t x16 = (160 << 16) + 0x4000;
    constexpr int32_t y16 = (80 << 16) + 0x2000;
    const int fallback_was_enabled = pgxp_position_fallback();
    gte_geometry_correction_set(1);
    gte_test_seed_geometry(packed, x16, y16);
    pgxp_set_position_fallback(1);

    uint32_t lookups0 = 0, hits0 = 0, miss_unrecorded0 = 0, miss_ambiguous0 = 0;
    gte_geometry_correction_stats(&lookups0, &hits0, &miss_unrecorded0,
                                  &miss_ambiguous0);
    if (pgxp_probe_precise_vertex(0xFFFFFFFFu, packed, 160, 80) !=
        PGXP_SRC_FALLBACK)
        return fail_value("PGXP fallback probe source", 0, 0, packed,
                          PGXP_SRC_FALLBACK, PGXP_SRC_NATIVE);
    uint32_t lookups1 = 0, hits1 = 0, miss_unrecorded1 = 0, miss_ambiguous1 = 0;
    gte_geometry_correction_stats(&lookups1, &hits1, &miss_unrecorded1,
                                  &miss_ambiguous1);
    if (lookups1 != lookups0 || hits1 != hits0 ||
        miss_unrecorded1 != miss_unrecorded0 ||
        miss_ambiguous1 != miss_ambiguous0)
        return fail_value("PGXP probe leaves geometry counters unchanged", 0, 0,
                          packed, lookups0, lookups1);

    int32_t got_x = 0, got_y = 0;
    uint16_t got_z = 1;
    if (pgxp_get_precise_vertex(0xFFFFFFFFu, packed, 160, 80,
                                &got_x, &got_y, &got_z) != PGXP_SRC_FALLBACK ||
        got_x != x16 || got_y != y16 || got_z != 0)
        return fail_value("PGXP draw uses geometry fallback", 0, 0, packed,
                          static_cast<uint32_t>(x16), static_cast<uint32_t>(got_x));
    uint32_t lookups2 = 0, hits2 = 0, miss_unrecorded2 = 0, miss_ambiguous2 = 0;
    gte_geometry_correction_stats(&lookups2, &hits2, &miss_unrecorded2,
                                  &miss_ambiguous2);
    gte_geometry_correction_set(0);
    pgxp_set_position_fallback(fallback_was_enabled);
    if (lookups2 != lookups1 + 1u || hits2 != hits1 + 1u ||
        miss_unrecorded2 != miss_unrecorded1 ||
        miss_ambiguous2 != miss_ambiguous1)
        return fail_value("draw increments geometry counters", 0, 0, packed,
                          lookups1 + 1u, lookups2);
    return 0;
}

} // namespace

/* Preserve projection precision (docs/ENHANCEMENTS.md G1.11) changes the
 * host-only SXY shadow and nothing the guest can see: RTPS/RTPT leave exactly
 * the reference oracle's registers (SXY, MAC0, IR, FLAG, ...) with it on, and
 * a qualifying vertex's shadow is the exact projection. */
int test_preserve_projection_is_shadow_only() {
    gte_precision_tracking_set(1);
    for (uint32_t function : {0x01u, 0x30u}) {
        for (unsigned iteration = 0; iteration < 256; ++iteration) {
            CPUState seed;
            randomize_gte(seed);
            CPUState expected = seed;
            CPUState actual = seed;
            const uint32_t cmd = (random_u32() & ~0x3Fu) | function;
            gte_test_execute_reference(&expected, cmd);
            pgxp_set_preserve_projection(1);
            gte_execute(&actual, cmd);
            pgxp_set_preserve_projection(0);
            if (!same_gte(expected, actual))
                return fail_state("preserve projection guest state", iteration,
                                  function, cmd, expected, actual);
        }
    }

    /* One qualifying vertex with fractional MAC bits: RT not a pure scale,
     * a translation, H = 300, OFX/OFY = (160.25, 119.5). */
    CPUState cpu{};
    auto pack = [](int32_t lo, int32_t hi) {
        return (static_cast<uint32_t>(hi & 0xFFFF) << 16) |
               static_cast<uint32_t>(lo & 0xFFFF);
    };
    const int32_t rt[9] = {4100, 3, 0, 0, 4090, 5, 0, 0, 4096};
    cpu.gte_ctrl[0] = pack(rt[0], rt[1]);
    cpu.gte_ctrl[1] = pack(rt[2], rt[3]);
    cpu.gte_ctrl[2] = pack(rt[4], rt[5]);
    cpu.gte_ctrl[3] = pack(rt[6], rt[7]);
    cpu.gte_ctrl[4] = static_cast<uint32_t>(rt[8]);
    const int32_t tr[3] = {7, -3, 40};
    cpu.gte_ctrl[5] = static_cast<uint32_t>(tr[0]);
    cpu.gte_ctrl[6] = static_cast<uint32_t>(tr[1]);
    cpu.gte_ctrl[7] = static_cast<uint32_t>(tr[2]);
    const int32_t ofx = (160 << 16) + 0x4000, ofy = (119 << 16) + 0x8000;
    cpu.gte_ctrl[24] = static_cast<uint32_t>(ofx);
    cpu.gte_ctrl[25] = static_cast<uint32_t>(ofy);
    cpu.gte_ctrl[26] = 300u;
    const int32_t v[3] = {101, -53, 937};
    cpu.gte_data[0] = pack(v[0], v[1]);
    cpu.gte_data[1] = static_cast<uint32_t>(v[2]);
    const uint32_t rtps = 0x00080001u;   /* RTPS, sf=1, lm=0 */
    const int64_t mac1 = (int64_t)tr[0] * 4096 + (int64_t)rt[0] * v[0] +
                         (int64_t)rt[1] * v[1] + (int64_t)rt[2] * v[2];
    const int64_t mac2 = (int64_t)tr[1] * 4096 + (int64_t)rt[3] * v[0] +
                         (int64_t)rt[4] * v[1] + (int64_t)rt[5] * v[2];
    const int64_t mac3 = (int64_t)tr[2] * 4096 + (int64_t)rt[6] * v[0] +
                         (int64_t)rt[7] * v[1] + (int64_t)rt[8] * v[2];
    const double ex = ofx / 65536.0 + (double)mac1 * 300.0 / (double)mac3;
    const double ey = ofy / 65536.0 + (double)mac2 * 300.0 / (double)mac3;

    CPUState ir_run = cpu;
    gte_execute(&ir_run, rtps);
    PreciseState ir{};
    gte_test_get_precise_projection(2, &ir.packed, &ir.x16, &ir.y16, &ir.z,
                                    &ir.valid);
    pgxp_set_preserve_projection(1);
    CPUState ppp_run = cpu;
    gte_execute(&ppp_run, rtps);
    pgxp_set_preserve_projection(0);
    PreciseState ppp{};
    gte_test_get_precise_projection(2, &ppp.packed, &ppp.x16, &ppp.y16, &ppp.z,
                                    &ppp.valid);
    if (!same_gte(ir_run, ppp_run))
        return fail_state("preserve projection keeps the guest SXY", 0, 1, rtps,
                          ir_run, ppp_run);
    const uint32_t sxy2 = ppp_run.gte_data[14];
    const int32_t native_x = static_cast<int16_t>(sxy2 & 0xFFFFu);
    const int32_t native_y = static_cast<int16_t>(sxy2 >> 16);
    if (!ir.valid || ir.packed != sxy2 || (ir.x16 >> 16) != native_x ||
        (ir.y16 >> 16) != native_y)
        return fail_value("IR-path shadow truncates to SXY2", 0, 14, sxy2,
                          static_cast<uint32_t>(native_x),
                          static_cast<uint32_t>(ir.x16 >> 16));
    if (!ppp.valid || ppp.packed != sxy2 || ppp.z != ir.z)
        return fail_value("exact-projection shadow keys the same word", 0, 14,
                          sxy2, sxy2, ppp.packed);
    if (ppp.x16 != (int32_t)std::floor(ex * 65536.0) ||
        ppp.y16 != (int32_t)std::floor(ey * 65536.0))
        return fail_value("exact-projection shadow", 0, 14, sxy2,
                          static_cast<uint32_t>((int32_t)std::floor(ex * 65536.0)),
                          static_cast<uint32_t>(ppp.x16));
    if (ppp.x16 == ir.x16 && ppp.y16 == ir.y16)
        return fail_value("exact projection differs from the IR path", 0, 14,
                          sxy2, 0, 0);

    /* Divide overflow (H >= 2*SZ3): the guest quotient saturates, so the
     * shadow stays on the IR path even with the mode on. */
    CPUState near_cpu = cpu;
    near_cpu.gte_data[1] = 100u;   /* SZ3 = 140 < H/2 */
    pgxp_set_preserve_projection(1);
    gte_execute(&near_cpu, rtps);
    pgxp_set_preserve_projection(0);
    PreciseState sat{};
    gte_test_get_precise_projection(2, &sat.packed, &sat.x16, &sat.y16, &sat.z,
                                    &sat.valid);
    const uint32_t near_sxy = near_cpu.gte_data[14];
    if (!sat.valid ||
        (sat.x16 >> 16) != static_cast<int16_t>(near_sxy & 0xFFFFu))
        return fail_value("divide overflow keeps the IR shadow", 0, 14,
                          near_sxy, near_sxy & 0xFFFFu,
                          static_cast<uint32_t>(sat.x16 >> 16));
    return 0;
}

int test_projection_scale() {
    double value = 2;
    for (const char* bad : {"", "nan", "inf", "9", "0", "-1", "2junk", "1e999", "1e-999"}) {
        CHECK(!psx_projection_scale_parse(bad, &value) && value == 2);
    }
    CHECK(psx_projection_scale_parse(" 8.0 ", &value) && value == 8);
    CHECK(!psx_projection_scale_valid(std::numeric_limits<double>::quiet_NaN()));
    CHECK(psx_projection_scale_denominator(0.00001) == 1);
    CPUState seed{};
    seed.gte_ctrl[0] = 4096; seed.gte_ctrl[2] = 4096; seed.gte_ctrl[4] = 4096;
    seed.gte_ctrl[26] = 320; seed.gte_data[0] = 100; seed.gte_data[1] = 1000;
    const uint32_t cmd = 0x80001;
    CPUState stock=seed; gte_set_fov_scale(1,1); gte_execute(&stock,cmd);
    CPUState identity=seed; gte_set_fov_scale(1000,1000); gte_execute(&identity,cmd);
    CHECK(same_gte(stock,identity));
    CPUState widened=seed; gte_set_fov_scale(1,2); gte_execute(&widened,cmd);
    CPUState reference=seed; reference.gte_ctrl[26]=160;
    gte_set_fov_scale(1,1); gte_execute(&reference,cmd);
    CHECK(widened.gte_ctrl[26] == 320 && widened.gte_data[14] == reference.gte_data[14]);
    CHECK(widened.gte_data[14] != stock.gte_data[14]);
    for (auto ratio : {std::pair<int,int>{0,1}, {-1,1}, {1,0}, {1,-1}}) {
        CPUState reset=seed; gte_set_fov_scale(ratio.first,ratio.second); gte_execute(&reset,cmd);
        CHECK(same_gte(stock,reset));
    }
    /* Guest H == 0 is a real (degenerate) projection: it must stay 0, not be
     * clamped to 1, under any scale. */
    {
        CPUState h0=seed; h0.gte_ctrl[26]=0;
        CPUState h0_stock=h0; gte_set_fov_scale(1,1); gte_execute(&h0_stock,cmd);
        CPUState h0_scaled=h0; gte_set_fov_scale(1,2); gte_execute(&h0_scaled,cmd);
        CHECK(h0_scaled.gte_ctrl[26] == 0);
        CHECK(same_gte(h0_stock,h0_scaled));
        gte_set_fov_scale(1000,500); h0_scaled=h0; gte_execute(&h0_scaled,cmd);
        CHECK(same_gte(h0_stock,h0_scaled));
    }
    /* Netplay forces identity so peers cannot diverge on SXY/MAC/FLAG. */
    {
        g_test_netplay_active = 1;
        CPUState np=seed; gte_set_fov_scale(1,2); gte_execute(&np,cmd);
        g_test_netplay_active = 0;
        CHECK(same_gte(stock,np));
        CPUState off=seed; gte_set_fov_scale(1,2); gte_execute(&off,cmd);
        CHECK(!same_gte(stock,off));
    }
    gte_set_fov_scale(1,1);
    return 0;
}

int main() {
    if (int rc = test_projection_scale()) return rc;
    if (int rc = test_hardware_register_semantics()) return rc;
    if (int rc = test_canonicalizer()) return rc;
    if (int rc = test_reads()) return rc;
    if (int rc = test_writes()) return rc;
    if (int rc = test_sequence_fuzz()) return rc;
    if (int rc = test_command_marshaling()) return rc;
    if (int rc = test_hle_command_contract()) return rc;
    if (int rc = test_projection_override_restores_transform()) return rc;
    if (int rc = test_command_timing_hook()) return rc;
    if (int rc = test_precise_sxy_invalidation()) return rc;
    if (int rc = test_precise_nclip_is_title_scoped()) return rc;
    if (int rc = test_saturated_nclip_keeps_architectural_result()) return rc;
    if (int rc = test_zero_nclip_requires_current_safe_projection()) return rc;
    if (int rc = test_precision_speculative_transaction()) return rc;
    if (int rc = test_pgxp_probe_does_not_count_geometry_lookup()) return rc;
    if (int rc = test_preserve_projection_is_shadow_only()) return rc;
    if (int rc = test_pgxp_culling()) return rc;
    if (int rc = test_render_view_parallax()) return rc;
    if (int rc = test_render_pose()) return rc;
    /* Signed host projection spans the camera plane without changing any
     * architectural register, including unsigned SZ and divider/screen flags. */
    pgxp_set_enabled(1);
    for (int depth : {-158,-1,0,1,149,150,151,900}) {
        GTEState native{}, enhanced{};
        native.RT[0][0]=native.RT[1][1]=native.RT[2][2]=4096;
        native.V0[0]=100;native.V0[1]=30;native.TR[2]=depth;
        native.H=300;native.OFX=256*65536;native.OFY=120*65536;
        enhanced=native;
        pgxp_set_projection_tracking(0);
        PSXRecomp::GTE::gte_rtps(&native,0x0180001u);
        pgxp_set_projection_tracking(1);
        PSXRecomp::GTE::gte_rtps(&enhanced,0x0180001u);
        PGXPProjection p;
        if (std::memcmp(&native,&enhanced,sizeof native) ||
            !pgxp_get_gte_projection(2,enhanced.SXY[2],&p) ||
            p.z!=depth || p.x!=256*depth+30000 || p.y!=120*depth+9000 || p.near_z!=150) {
            std::fprintf(stderr,"FAIL signed camera projection at depth %d\n",depth);
            return 1;
        }
    }
    /* The clipper's projection (hx, OFY/MAC2*H term, near_z = H/2) must use the
     * same scaled H as the guest's SXY, or culling disagrees with the draw. */
    gte_set_fov_scale(1,2);
    for (int depth : {1,149,900}) {
        GTEState g{};
        g.RT[0][0]=g.RT[1][1]=g.RT[2][2]=4096;
        g.V0[0]=100;g.V0[1]=30;g.TR[2]=depth;
        g.H=300;g.OFX=256*65536;g.OFY=120*65536;
        pgxp_set_projection_tracking(1);
        PSXRecomp::GTE::gte_rtps(&g,0x0180001u);
        PGXPProjection p;
        if (!pgxp_get_gte_projection(2,g.SXY[2],&p) ||
            p.z!=depth || p.x!=256*depth+15000 || p.y!=120*depth+4500 || p.near_z!=75) {
            std::fprintf(stderr,"FAIL scaled-H clipper projection at depth %d\n",depth);
            gte_set_fov_scale(1,1);
            return 1;
        }
    }
    gte_set_fov_scale(1,1);
    pgxp_set_projection_tracking(0);
    std::puts("PASS: canonical GTE register helpers match GTEState transfer oracle");
    return 0;
}
