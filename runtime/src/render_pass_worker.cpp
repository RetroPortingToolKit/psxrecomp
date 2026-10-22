/* Double-buffered private guest draw execution. The emulation owner publishes
 * values, the worker owns guest execution, and only the render owner consumes
 * the sealed GPU list. Neither owner waits for an extra frame. */
#include "render_pass_worker.h"
#include "mod_plugins.h"
#include "mod_execution.h"
#include "mod_runtime.h"
#include "cpu_state.h"
#include "gpu.h"
#include "gpu_gl_renderer.h"
#include "gte_view.h"
#include "pgxp.h"
#include "psx_memory.h"
#include "psx_cycles.h"
#include "psx_cycle_freeze.h"
#include "psx_execution.h"
#include "psx_icache.h"
#include "psx_bios_backend.h"
#include "overlay_loader.h"
#include "render_thread.h"
#include "interrupts.h"
#include "timers.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <csetjmp>
extern "C" {
uint32_t dma_snapshot_bytes(void);
void dma_snapshot_write(uint8_t *);
int dma_snapshot_read(const uint8_t *, uint32_t);
int mod_runtime_execution_ready(void);
int psx_game_execution_lookup(uint32_t, PsxExecutionCode *);
int psx_netplay_active(void);
void psx_irq_set_cause_ptr(uint32_t *);
extern PSX_THREAD_LOCAL void (*g_overlay_flush_pending_cycles)(void);
extern PSX_THREAD_LOCAL int g_host_store_depth, g_dma_exec_depth, g_call_unit_depth;
extern PSX_THREAD_LOCAL uint32_t g_debug_last_store_pc;
}
namespace {
constexpr size_t PayloadCapacity = 64u * 1024u;
struct Job {
    std::atomic<bool> queued{false}, worker_done{true}, consumer_done{true};
    bool posted = true, ok = false;
    uint64_t serial = 0, cycle = 0;
    uint32_t period = 2, open = 0, vblank = 0, gcs[3]{}, icache[1024]{};
    int icache_active = -1;
    CPUState cpu{};
    PSXModRenderPass pass{};
    PSXModRenderPassFn draw = nullptr;
    alignas(16) unsigned char payload[PayloadCapacity]{};
    const PsxBiosBackend *bios = nullptr;
    PsxMemoryBacking *memory = memory_backing_create();
    PsxMemoryExecutionState *metadata = memory_execution_create();
    PGXPExecutionSnapshot *pgxp = pgxp_execution_snapshot_create();
    GTEExecutionSnapshot *gte = gte_execution_snapshot_create();
    GpuExecutionSnapshot *gpu = gpu_execution_snapshot_create();
    GlExecutionProducerState *gl = gl_renderer_execution_create();
    OverlayExecutionSnapshot *overlays = overlay_execution_create();
    ModExecutionSnapshot *mods = mod_execution_create();
    RtCommandList *commands = rt_list_create(16u << 20);
    uint32_t dma_bytes = dma_snapshot_bytes();
    std::unique_ptr<uint8_t[]> dma;
    uint16_t timer_counter[3]{}, timer_target[3]{};
    uint32_t timer_mode[3]{}, timer_frac[3]{};
    int32_t timer_irq[3]{};
    Job() : dma(new (std::nothrow) uint8_t[dma_bytes]) {}
    bool valid() const { return memory && metadata && pgxp && gte && gpu && gl &&
        overlays && mods && commands && dma; }
    ~Job() {
        memory_backing_destroy(memory); memory_execution_destroy(metadata);
        pgxp_execution_snapshot_destroy(pgxp); gte_execution_snapshot_destroy(gte);
        gpu_execution_snapshot_destroy(gpu); gl_renderer_execution_destroy(gl);
        overlay_execution_destroy(overlays); mod_execution_destroy(mods);
        rt_list_destroy(commands);
    }
};
struct Worker {
    std::unique_ptr<Job> jobs[2];
    std::thread thread;
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    std::atomic<bool> enabled{false};
    std::atomic<uint64_t> published{0}, completed{0}, late{0}, aborted{0}, refused{0}, misses{0}, calls{0};
    std::atomic<uint32_t> address{0};
    std::atomic<const char *> failure{nullptr};
    uint64_t serial = 0;
};
Worker W;
thread_local std::jmp_buf landing;
thread_local bool armed;
void fault(const char *reason, uint32_t addr) {
    W.failure.store(reason); W.address.store(addr);
    if (!std::strcmp(reason, "dispatch_miss")) ++W.misses;
    if (!armed) std::abort();
    std::longjmp(landing, 1);
}
void overrun() { if (armed) fault("watchdog", 0); }
void capture_fault(const char *reason) { fault(reason, 0); }
int lookup(void *user, CPUState *cpu, uint32_t addr, PsxExecutionCode *code, void (**flush)(void)) {
    Job *job = static_cast<Job *>(user);
    if (overlay_execution_lookup(job->overlays, addr, code, flush) ||
        psx_game_execution_lookup(addr, code)) { ++W.calls; return 1; }
    uint32_t phys = addr & 0x1FFFFFFFu;
    if ((phys >= 0x1FC00000u || phys < 0x10000u) && job->bios && job->bios->execution_lookup) {
        int result = job->bios->execution_lookup(cpu, addr, code);
        if (result) ++W.calls;
        return result;
    }
    return 0;
}
void execute(Job &job) {
    job.ok = false;
    memory_backing_bind(job.memory);
    memory_execution_bind(job.metadata, &job.cpu.cop0[12]);
    psx_irq_set_cause_ptr(&job.cpu.cop0[13]);
    psx_cycle_count = job.cycle;
    psx_guest_cycle_scale_restore(job.gcs);
    interrupts_set_cycles_since_vblank(job.vblank);
    std::memcpy(g_psx_icache_tv, job.icache, sizeof job.icache);
    g_psx_icache_active = job.icache_active;
    timers_set_snapshot(job.timer_counter, job.timer_mode, job.timer_target, job.timer_irq, job.timer_frac);
    if (!dma_snapshot_read(job.dma.get(), job.dma_bytes) ||
        !pgxp_execution_snapshot_bind(job.pgxp) || !gte_execution_snapshot_bind(job.gte) ||
        !gpu_execution_snapshot_bind(job.gpu)) std::abort();
    mod_execution_bind(job.mods);
    ModFunctionEntryContext empty{};
    mod_runtime_function_entry_context_restore(&empty);
    g_host_store_depth = g_dma_exec_depth = g_call_unit_depth = 0;
    g_debug_last_store_pc = 0;
    psx_execution_begin(&job, lookup, fault);
    overlay_execution_bind(job.overlays);
    rt_capture_begin(job.commands, capture_fault);
    gl_renderer_execution_bind(job.gl);
    PsxCycleFreeze freeze{};
    if (!psx_cycle_freeze_begin(&freeze, 8000000u, overrun)) std::abort();
    if (setjmp(landing) == 0) {
        armed = true;
        gl_renderer_pass_begin(job.pass.x, job.pass.y, job.pass.w, job.pass.h,
            job.open, job.period, 0);
        job.ok = job.draw(&job.cpu, job.payload, job.pass.alpha_q16) != 0;
        armed = false;
    } else {
        armed = false; ++W.aborted;
        g_psx_cyc_local_acc = nullptr; g_psx_cyc_bb_defer = 0; g_psx_cyc_batch = 0;
    }
    psx_execution_end();
    psx_cycle_freeze_end(&freeze);
    gl_renderer_pass_end(job.ok ? job.pass.alpha_q16 : 0, job.ok);
    rt_capture_end();
    mod_runtime_function_entry_context_restore(&empty);
    g_host_store_depth = g_dma_exec_depth = g_call_unit_depth = 0;
    gpu_execution_snapshot_unbind(); gte_execution_snapshot_unbind();
    pgxp_execution_snapshot_unbind(); memory_backing_bind(nullptr);
    psx_irq_set_cause_ptr(nullptr);
}
void run() {
    for (;;) {
        Job *job = nullptr;
        {
            std::unique_lock<std::mutex> lock(W.mutex);
            W.wake.wait(lock, [] { return W.stop ||
                W.jobs[0]->queued.load() || W.jobs[1]->queued.load(); });
            if (W.stop) return;
            for (auto &slot : W.jobs)
                if (slot->queued.load(std::memory_order_acquire) &&
                    (!job || slot->serial < job->serial)) job = slot.get();
            if (job) job->queued.store(false);
        }
        if (!job) continue;
        execute(*job);
        job->worker_done.store(true, std::memory_order_release);
    }
}
void consume(void *user) {
    Job &job = *static_cast<Job *>(user);
    if (!W.enabled.load() || rt_frames_ahead() > 0 ||
        !job.worker_done.load(std::memory_order_acquire)) ++W.late;
    else if (job.ok) {
        if (gl_renderer_execution_consume(job.commands)) ++W.completed;
        else ++W.refused;
    }
    job.consumer_done.store(true, std::memory_order_release);
}
bool start() {
    if (W.thread.joinable()) return true;
    for (auto &slot : W.jobs) {
        slot.reset(new (std::nothrow) Job);
        if (!slot || !slot->valid()) { for (auto &other : W.jobs) other.reset(); return false; }
    }
    static bool exit_registered = false;
    if (!exit_registered) {
        if (std::atexit(render_pass_worker_stop) != 0) {
            for (auto &slot : W.jobs) slot.reset(); return false;
        }
        exit_registered = true;
    }
    W.stop = false;
    W.thread = std::thread(run);
    return true;
}
}
extern "C" int psx_mod_set_worker_render_passes(int enabled) {
    if (!enabled) { render_pass_worker_stop(); return 1; }
    W.enabled.store(true); return 1;
}
extern "C" int psx_mod_render_pass_async(CPUState *cpu, const PSXModRenderPass *pass,
    PSXModRenderPassFn draw, const void *payload, size_t bytes) {
    if (!W.enabled.load() || !cpu || !pass || pass->struct_size != sizeof(*pass) ||
        !draw || (!payload && bytes) || bytes > PayloadCapacity || psx_netplay_active() ||
        psx_mod_render_pass_status() != PSX_MOD_RENDER_PASS_READY || !mod_runtime_execution_ready()) {
        ++W.refused; return 0;
    }
    bool builtin = false;
    for (uint32_t i = 0; i < psx_bios_builtin_registry_count; ++i)
        if (psx_bios_builtin_registry[i] == psx_bios_active) builtin = true;
    if (!builtin || !psx_bios_active->execution_lookup || !start()) { ++W.refused; return 0; }
    Job *job = nullptr;
    for (auto &slot : W.jobs)
        if (slot->worker_done.load(std::memory_order_acquire) &&
            slot->consumer_done.load(std::memory_order_acquire)) { job = slot.get(); break; }
    if (!job) { ++W.late; return 0; }
    if (g_overlay_flush_pending_cycles) g_overlay_flush_pending_cycles();
    job->cpu = *cpu; job->pass = *pass; job->draw = draw; job->bios = psx_bios_active;
    if (bytes) std::memcpy(job->payload, payload, bytes);
    memory_backing_capture(job->memory); memory_execution_capture(job->metadata);
    if (!pgxp_execution_snapshot_capture(job->pgxp) || !gte_execution_snapshot_capture(job->gte) ||
        !gpu_execution_snapshot_capture(job->gpu) || !gl_renderer_execution_capture(job->gl) ||
        !overlay_execution_capture(job->overlays)) { ++W.refused; return 0; }
    mod_execution_capture(job->mods);
    dma_snapshot_write(job->dma.get());
    timers_get_snapshot(job->timer_counter, job->timer_mode, job->timer_target, job->timer_irq, job->timer_frac);
    job->cycle = psx_cycle_count;
    job->vblank = interrupts_get_cycles_since_vblank();
    psx_guest_cycle_scale_snapshot(job->gcs);
    std::memcpy(job->icache, g_psx_icache_tv, sizeof job->icache);
    job->icache_active = g_psx_icache_active;
    job->open = render_pass_worker_take_plan(&job->period);
    job->serial = ++W.serial; job->posted = false; job->ok = false;
    job->worker_done.store(false); job->consumer_done.store(false);
    {
        std::lock_guard<std::mutex> lock(W.mutex);
        job->queued.store(true, std::memory_order_release);
    }
    ++W.published; W.wake.notify_one(); return 1;
}
extern "C" void render_pass_worker_flush(void) {
    if (!W.enabled.load() || !W.thread.joinable()) return;
    /* The canonical presentation boundary is the extra's admission deadline.
     * Process publication order even when the two reusable slots alternate. */
    for (unsigned n = 0; n < 2; ++n) {
        Job *next = nullptr;
        for (auto &slot : W.jobs)
            if (!slot->posted && !slot->consumer_done.load(std::memory_order_acquire) &&
                (!next || slot->serial < next->serial)) next = slot.get();
        if (!next) break;
        if (gl_renderer_execution_enqueue(consume, next)) next->posted = true;
        else { next->posted = true; next->consumer_done.store(true, std::memory_order_release); ++W.late; }
    }
}
extern "C" void render_pass_worker_stop(void) {
    W.enabled.store(false);
    if (!W.thread.joinable()) return;
    { std::lock_guard<std::mutex> lock(W.mutex); W.stop = true; }
    W.wake.notify_one(); W.thread.join();
    rt_drain();
    for (auto &slot : W.jobs) slot.reset();
}
extern "C" void render_pass_worker_stats(RenderPassWorkerStats *out) {
    if (!out) return;
    *out = {W.published.load(), W.completed.load(), W.late.load(), W.aborted.load(),
        W.refused.load(), W.misses.load(), W.calls.load(), W.address.load(), W.failure.load()};
}

extern "C" void render_pass_worker_quiesce(void) {
    bool enabled = W.enabled.load();
    render_pass_worker_stop();
    W.enabled.store(enabled);
}
