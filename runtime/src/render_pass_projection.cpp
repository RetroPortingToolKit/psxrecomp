#include "render_pass_projection.h"
#include "render_pass_motion.h"
#include "cpu_state.h"
#include "psx_cycle_freeze.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>
#include <unordered_map>
#include <vector>

namespace {
struct Projection {
    uint64_t key;
    uint32_t vertices[6];
    uint32_t caller, command;
    PSXMotionMatrix current{}, previous{};
    bool blend = false;
};
PSXMotionMatrix matrix(const CPUState* cpu) {
    PSXMotionMatrix m{};
    for (unsigned i = 0; i < 9; ++i)
        m.r[i] = int16_t(cpu->gte_ctrl[i / 2] >> (16 * (i % 2)));
    for (unsigned i = 0; i < 3; ++i) m.t[i] = int32_t(cpu->gte_ctrl[5 + i]);
    return m;
}
void set_matrix(CPUState* cpu, const PSXMotionMatrix& m) {
    for (unsigned i = 0; i < 4; ++i)
        cpu->gte_ctrl[i] = uint16_t(m.r[2*i]) | (uint32_t(uint16_t(m.r[2*i+1])) << 16);
    cpu->gte_ctrl[4] = uint32_t(int32_t(m.r[8]));
    for (unsigned i = 0; i < 3; ++i) cpu->gte_ctrl[5+i] = uint32_t(m.t[i]);
}
Projection projection(const CPUState* cpu, uint32_t command) {
    Projection p{};
    p.current = matrix(cpu);
    p.command = command;
    p.caller = cpu->gpr[31] & 0x1FFFFFFFu;
    const unsigned count = (command & 63) == 0x30 ? 6 : 2;
    p.key = 1469598103934665603ull;
    auto hash = [&](uint32_t v) { p.key = (p.key ^ v) * 1099511628211ull; };
    hash(p.caller); hash(command);
    for (unsigned i = 0; i < count; ++i) {
        // VZ is signed 16-bit; ignore unused high halves in backing registers.
        p.vertices[i] = (i & 1) ? uint16_t(cpu->gte_data[i]) : cpu->gte_data[i];
        hash(p.vertices[i]);
    }
    return p;
}
bool same_vertices(const Projection& a, const Projection& b) {
    return a.caller == b.caller && a.command == b.command &&
           std::memcmp(a.vertices, b.vertices, sizeof a.vertices) == 0;
}
double distance(const PSXMotionMatrix& a, const PSXMotionMatrix& b) {
    double d = 0;
    for (unsigned i=0; i<3; ++i) { double v=double(a.t[i])-b.t[i]; d += v*v; }
    // Distinguish coincident instances with different rotations.
    for (unsigned i=0; i<9; ++i) { double v=(double(a.r[i])-b.r[i])/16; d += v*v; }
    return d;
}
bool same_matrix(const PSXMotionMatrix& a, const PSXMotionMatrix& b) {
    return std::memcmp(a.r, b.r, sizeof a.r) == 0 &&
           std::memcmp(a.t, b.t, sizeof a.t) == 0;
}
}

struct PSXProjectionHistory {
    PSXProjectionHistory* next = nullptr;
    uint32_t capacity;
    std::vector<Projection> current, previous;
    std::unordered_map<uint64_t, std::vector<uint32_t>> index;
    std::vector<uint8_t> used;
    PSXProjectionStats stats{};
    bool capturing = false, replaying = false, history_valid = false;
    double alpha = 1;
};
static PSX_THREAD_LOCAL PSXProjectionHistory* active = nullptr;
// Intrusive list has no global destructor: title replay objects may release
// their histories from static destructors, in either link initialization order.
static PSXProjectionHistory* histories = nullptr;
extern "C" void psx_projection_reset_session(void) {
    // A load normally arrives between frames, when active is null. Invalidate
    // idle histories too so a new timeline cannot blend against the old one.
    for (auto* h = histories; h; h = h->next) psx_projection_invalidate(h);
    g_psx_projection_command = nullptr;
}

extern "C" PSXProjectionHistory* psx_projection_create(uint32_t capacity) {
    if (!capacity || capacity > 262144) return nullptr;
    auto* h = new (std::nothrow) PSXProjectionHistory;
    if (h) {
        h->capacity = capacity; h->current.reserve(capacity); h->previous.reserve(capacity);
        h->next = histories; histories = h;
    }
    return h;
}
extern "C" void psx_projection_invalidate(PSXProjectionHistory* h) {
    if (!h) return;
    if (active == h) { active = nullptr; g_psx_projection_command = nullptr; }
    h->current.clear(); h->previous.clear(); h->index.clear(); h->used.clear();
    h->stats = {}; h->capturing = h->replaying = h->history_valid = false;
}
extern "C" void psx_projection_destroy(PSXProjectionHistory* h) {
    psx_projection_invalidate(h);
    for (auto** link = &histories; *link; link = &(*link)->next) {
        if (*link == h) { *link = h->next; break; }
    }
    delete h;
}
extern "C" void psx_projection_capture_begin(PSXProjectionHistory* h) {
    if (!h || g_psx_render_pass_active) return;
    h->previous.swap(h->current); h->current.clear();
    h->stats = {}; h->capturing = true; h->replaying = false;
    active = h; g_psx_projection_command = psx_projection_command;
}
extern "C" void psx_projection_capture_end(PSXProjectionHistory* h, uint32_t ticks,
                                             double max_move) {
    if (!h) return;
    h->capturing = false;
    if (active == h) { active = nullptr; g_psx_projection_command = nullptr; }
    h->stats.captured = uint32_t(h->current.size());
    h->index.clear();
    if (h->history_valid && ticks && ticks <= 8 && !h->stats.overflow) {
        for (uint32_t i=0; i<h->previous.size(); ++i) h->index[h->previous[i].key].push_back(i);
        h->used.assign(h->previous.size(), 0);
        const double limit = max_move * ticks;
        for (auto& p : h->current) {
            auto it = h->index.find(p.key);
            uint32_t best = UINT32_MAX;
            double best_d = INFINITY;
            if (it != h->index.end()) for (uint32_t i : it->second) {
                if (h->used[i] || !same_vertices(p, h->previous[i])) continue;
                const double d = distance(p.current, h->previous[i].current);
                if (d < best_d) { best = i; best_d = d; }
            }
            if (best == UINT32_MAX) { ++h->stats.unmatched; continue; }
            h->used[best] = 1;
            p.previous = h->previous[best].current;
            bool placed = false;
            for (unsigned i=0; i<3; ++i)
                placed |= std::abs(double(p.current.t[i])-p.previous.t[i]) > limit;
            PSXMotionMatrix midpoint;
            p.blend = !placed && psx_motion_blend_matrix(&p.previous, &p.current, .5, 1.2, &midpoint);
            if (p.blend) {
                ++h->stats.matched;
                if (!same_matrix(p.previous,p.current)) ++h->stats.changed;
            } else ++h->stats.discontinuities;
        }
    } else h->stats.unmatched = h->stats.captured;
    h->history_valid = !h->stats.overflow && !h->current.empty();
    // Index the current frame for replay; visibility/subdivision may reorder
    // projections. Never blend a different vertex because an ordinal shifted.
    h->index.clear();
    for (uint32_t i=0; i<h->current.size(); ++i) h->index[h->current[i].key].push_back(i);
}
extern "C" void psx_projection_replay_begin(PSXProjectionHistory* h, uint32_t alpha) {
    if (!h || !g_psx_render_pass_active) return;
    h->capturing = false; h->replaying = true;
    h->used.assign(h->current.size(), 0);
    h->alpha = std::min(alpha, 65536u) / 65536.0;
    h->stats.replayed = 0; active = h; g_psx_projection_command = psx_projection_command;
}
extern "C" void psx_projection_replay_end(PSXProjectionHistory* h) {
    if (!h) return;
    h->replaying = false;
    if (active == h) { active = nullptr; g_psx_projection_command = nullptr; }
}
extern "C" void psx_projection_stats(const PSXProjectionHistory* h, PSXProjectionStats* out) {
    if (out) *out = h ? h->stats : PSXProjectionStats{};
}
extern "C" int psx_projection_command(CPUState* cpu, uint32_t command) {
    auto* h = active;
    if (!h || ((command & 63) != 1 && (command & 63) != 0x30)) return 0;
    // A watchdog may unwind a replay callback without its normal end call.
    // The sandbox flag, rather than a plugin-local flag, owns replay lifetime.
    if (h->replaying && !g_psx_render_pass_active) {
        active = nullptr; g_psx_projection_command = nullptr; h->replaying = false; return 0;
    }
    Projection p = projection(cpu, command);
    if (h->capturing && !g_psx_render_pass_active) {
        if (h->current.size() == h->capacity) ++h->stats.overflow;
        else h->current.push_back(p);
        return 0;
    }
    if (!h->replaying || !g_psx_render_pass_active) return 0;
    auto it = h->index.find(p.key);
    if (it == h->index.end()) return 0;
    for (uint32_t i : it->second) {
        const auto& c = h->current[i];
        if (h->used[i] || !same_vertices(c,p) || !same_matrix(c.current,p.current)) continue;
        h->used[i] = 1;
        if (!c.blend || same_matrix(c.previous,c.current)) return 0;
        PSXMotionMatrix intermediate;
        psx_motion_blend_matrix(&c.previous, &c.current, h->alpha, 1.2, &intermediate);
        set_matrix(cpu, intermediate); ++h->stats.replayed;
        return 1;
    }
    return 0;
}
