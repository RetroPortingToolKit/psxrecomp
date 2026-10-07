#pragma once
#include "cpu_state.h"
#include "mod_plugins.h"
#include "psx_cycle_freeze.h"
#include "render_pass_projection.h"
#include "pgxp.h"
#include <cstring>
#include <vector>

extern "C" uint8_t* memory_get_ram_ptr(void);
extern "C" uint8_t* memory_get_scratchpad_ptr(void);
extern "C" uint32_t memory_get_ram_bytes(void);

/* A draw section may calculate temporary matrices, consume random seeds or
 * update effect caches. Start every replay from the original section's RAM,
 * rather than applying those calculations twice to the completed frame. Only
 * restore inside the framework's machine/VRAM sandbox. */
class PSXDrawReplay {
    CPUState entry{};
    std::vector<uint8_t> ram;
    uint8_t scratch[1024]{};
    uint32_t start = 0, stop = 0, previous_tick = 0;
    bool captured = false, ready = false, have_tick = false;
    bool projections = true;
    PSXProjectionHistory* history = nullptr;
public:
    PSXProjectionStats stats{};
    uint32_t ticks = 0;
    ~PSXDrawReplay() { psx_projection_destroy(history); }
    void invalidate() {
        captured = ready = have_tick = false; ticks = 0;
        psx_projection_invalidate(history);
    }
    // Titles with known actor/camera layouts can apply their motion sets after
    // restore, instead of matching final GTE projections. Both routes share the
    // same draw-entry snapshot and machine sandbox.
    void capture(CPUState* cpu, uint32_t first, uint32_t last,
                 bool capture_projections = true) {
        if (g_psx_render_pass_active) return;
        if (captured) invalidate(); // previous drawing section never completed
        projections = capture_projections;
        if (projections && !history) history = psx_projection_create(65536);
        if ((projections && !history) || first >= last || ((first | last) & 3)) return;
        entry = *cpu; start = first; stop = last;
        ram.resize(memory_get_ram_bytes());
        std::memcpy(ram.data(), memory_get_ram_ptr(), ram.size());
        std::memcpy(scratch, memory_get_scratchpad_ptr(), sizeof scratch);
        captured = true; ready = false;
        if (projections) psx_projection_capture_begin(history);
    }
    bool prepare(uint32_t tick, bool motion_ready = false) {
        if (!captured) { invalidate(); return false; }
        captured = false;
        ticks = have_tick ? tick - previous_tick : 0;
        previous_tick = tick; have_tick = true;
        if (projections) {
            psx_projection_capture_end(history, ticks, 512.0);
            psx_projection_stats(history, &stats);
        }
        ready = ticks && ticks <= 8 && (projections
            ? stats.changed && !stats.overflow : motion_ready);
        return ready;
    }
    bool restore(CPUState* cpu, uint32_t alpha) {
        if (!ready || !g_psx_render_pass_active || ram.size() != memory_get_ram_bytes()) return false;
        std::memcpy(memory_get_ram_ptr(), ram.data(), ram.size());
        std::memcpy(memory_get_scratchpad_ptr(), scratch, sizeof scratch);
        // Raw RAM is now from section entry. Its current-frame precision
        // shadows are stale; the pass checkpoint restores them afterwards.
        pgxp_invalidate_all();
        *cpu = entry;
        if (projections) psx_projection_replay_begin(history, alpha);
        return true;
    }
    bool draw(CPUState* cpu) {
        const bool reached = psx_mod_run_guest_span(cpu, start, stop) != 0;
        if (projections) {
            psx_projection_replay_end(history);
            psx_projection_stats(history, &stats);
        }
        return reached && (!projections || stats.replayed != 0);
    }
    static uint32_t call(CPUState* cpu, uint32_t function, uint32_t a0=0, uint32_t a1=0) {
        const CPUState saved = *cpu;
        cpu->pc = 0; cpu->gpr[4] = a0; cpu->gpr[5] = a1;
        psx_dispatch_call(cpu, function, saved.gpr[31]);
        const uint32_t result = cpu->gpr[2]; *cpu = saved; return result;
    }
    static void rate(uint32_t fps, uint32_t flip) {
        psx_mod_set_frame_interpolation_source(PSX_MOD_FRAME_SOURCE_FLIP);
        psx_mod_set_render_pass_flip(flip);
        psx_mod_set_frame_interpolation_blend(PSX_MOD_FRAME_INTERPOLATION_HOLD);
        psx_mod_set_frame_interpolation(fps);
    }
};
