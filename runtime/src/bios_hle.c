/* bios_hle.c — opt-in High-Level Emulation tier for PSX BIOS kernel services.
 *
 * See bios_hle.h for the design (LLE default + oracle, per-game opt-in,
 * unimplemented services fall through to the recompiled BIOS, boot shell-skip,
 * always-on observability ring).
 *
 * v1 service coverage — the B0 event family, ground-truthed instruction by
 * instruction against the SCPH1001 kernel (ROM 0xBFC11644..0xBFC11A84, kernel
 * RAM 0x1B44..0x1F8C; cross-checked with docs/psx_bios_disasm.txt B0 table @
 * 0x874 and PSX-SPX "Kernel: Events"):
 *
 *   B0:0x07 DeliverEvent(class, spec)
 *   B0:0x08 OpenEvent(class, spec, mode, func)
 *   B0:0x09 CloseEvent(event)
 *   B0:0x0B TestEvent(event)
 *   B0:0x0C EnableEvent(event)
 *   B0:0x0D DisableEvent(event)
 *
 * EvCB table: base pointer at kernel [0x120], size in bytes at [0x124], one
 * 0x1C-byte entry per event: +0x00 class, +0x04 status, +0x08 spec, +0x0C
 * mode, +0x10 func. Status values: 0 free, 0x1000 disabled, 0x2000 enabled,
 * 0x4000 ready/delivered. Mode values: 0x1000 = call callback on delivery,
 *0x2000 = mark ready (polled via TestEvent/WaitEvent).
 *
 * Everything else (WaitEvent — it blocks; threads; pads; the card/CD stack;
 * all of A0/C0) falls through to LLE in v1, exactly like gbarecomp's
 * machine-state SWIs. The recompiled BIOS stays fully load-bearing for them.
 *
 * TIMING NOTE: cycle costs charged here are approximations of the kernel
 * routine's dynamic instruction count (vector stub + table handler + body).
 * LLE accrues the exact count by executing; cycle-exact HLE is a documented
 * limitation of the opt-in tier. LLE remains the timing oracle.
 */

#include "bios_hle.h"
#include "native_bios_services.h"
#include "interrupts.h"
#include "psx_scheduler.h"
#include "psx_memory.h"

#include "cpu_state.h"
#include "psx_cycles.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* fntrace.c — game-entry handoff latch (drives the boot turbo window). */
extern int fntrace_is_game_started(void);

/* ── hook slot (referenced by the generated dispatch trampoline) ─────────── */

int (*g_psx_bios_hle_hook)(struct CPUState* cpu, uint32_t phys) = NULL;

/* ── mode state ──────────────────────────────────────────────────────────── */

static int s_native_kernel_on = 0;
static int s_call_hle_on     = 0;   /* service kernel calls in HLE */
static int s_boot_skip_on    = 0;   /* one-shot shell intercept */
static int s_shell_skipped   = 0;   /* boot skip already fired */

/* Per-image anchors come from the LINKED recompiled BIOS itself
 * (psx_bios_image, emitted into <stem>_dispatch.c from the BIOS profile's
 * [recompiler.runtime_exports]). For SCPH1001 the shell is unpacked to RAM
 * 0x30000 by LoadRunShell (ROM 0xBFC06FF0: memcpy(0x80030000, 0xBFC18000,
 * 0x67FF0)) and entered via an indirect call, which always routes through
 * the dispatcher. A profile that omits an anchor exports 0 = the feature is
 * STRUCTURALLY UNAVAILABLE on that BIOS (never "address 0"): the boot skip
 * then simply never fires. Event delivery can use the caller's link on
 * an image whose event table ABI is known. */
#include "psx_bios_image.h"

/* Kernel EvCB table anchors (docs/psx_bios_disasm.txt; kseg1 like the kernel
 * itself uses so reads/writes are plain RAM traffic). */
#define KADDR_EVCB_BASE 0xA0000120u
#define KADDR_EVCB_SIZE 0xA0000124u

/* EvCB field offsets / values (SCPH1001 kernel + PSX-SPX). */
#define EVCB_STRIDE   0x1Cu
#define EV_CLASS      0x00u
#define EV_STATUS     0x04u
#define EV_SPEC       0x08u
#define EV_MODE       0x0Cu
#define EV_FUNC       0x10u
#define EVST_FREE     0x0000u
#define EVST_DISABLED 0x1000u
#define EVST_ENABLED  0x2000u
#define EVST_READY    0x4000u
#define EVMD_CALLBACK 0x1000u
#define EVMD_MARK     0x2000u

int psx_bios_hle_event_abi_supported(void)
{
    if (psx_bios_image.deliver_event_ret) return 1;
    /* The pinned OpenBIOS EventInfo and kernel globals use the same EvCB
     * layout. Keep unverified firmware revisions out of the call tier. */
    const char *sha256 = psx_bios_image.image_sha256;
    return sha256 && strcmp(sha256,
        "fabe498fbf224e4721f12f31b6f5fe0659205e341dc4e5c5f91b9bd1a1011c57") == 0;
}

/* ── always-on ring ──────────────────────────────────────────────────────── */

static PsxHleCallEntry s_ring[PSX_HLE_RING_CAP];
static uint64_t        s_ring_seq = 0;

static void hle_record(uint32_t vector, uint32_t fn, const CPUState* cpu,
                       uint32_t v0, uint8_t route)
{
    /* Thread attribution: current TCB via the kernel's process block chain
     * (0x108 -> PCB, PCB[0] -> running TCB; SCPH1001 layout, stable across
     * kernel versions). Distinguishes which guest thread issued each call —
     * load-bearing for event-consumption races (MMX6 card check). */
    uint32_t pcb = cpu->read_word(0xA0000108u);
    uint32_t pcb_phys = pcb & 0x1FFFFFFFu;
    uint32_t tcb = (pcb != 0u && pcb_phys < psx_ram_live_bytes())
                       ? cpu->read_word(0xA0000000u | pcb_phys) : 0u;
    extern int psx_get_in_exception(void);
    uint8_t in_exc = (uint8_t)(psx_get_in_exception() ? 1u : 0u);

    /* Collapse tight polling cycles: guest code that spins on a handful of
     * calls (e.g. MMX6's 4-way TestEvent card poll at ~175K calls/s) would
     * otherwise flush the whole ring in ~0.1s. If an identical call
     * (vector/fn/args/ra/result/route/thread) exists among the last 8
     * entries, count it there. The first and last occurrence cycles plus the
     * exact repeat count survive; any change in args, result, or issuing
     * thread appends a fresh entry, so every TRANSITION is ring-ordered. */
    uint64_t back = s_ring_seq < 8 ? s_ring_seq : 8;
    for (uint64_t k = 1; k <= back; k++) {
        PsxHleCallEntry* p = &s_ring[(s_ring_seq - k) & (PSX_HLE_RING_CAP - 1)];
        if (p->vector == vector && p->fn == fn && p->route == route &&
            p->a0 == cpu->gpr[4] && p->a1 == cpu->gpr[5] &&
            p->a2 == cpu->gpr[6] && p->a3 == cpu->gpr[7] &&
            p->ra == cpu->gpr[31] && p->v0 == v0 &&
            p->tcb == tcb && p->in_exc == in_exc) {
            p->repeat++;
            p->cycle_last = psx_get_cycle_count();
            return;
        }
    }
    PsxHleCallEntry* e = &s_ring[s_ring_seq & (PSX_HLE_RING_CAP - 1)];
    e->seq    = s_ring_seq;
    e->cycle  = psx_get_cycle_count();
    e->cycle_last = e->cycle;
    e->vector = vector;
    e->fn     = fn;
    e->a0     = cpu->gpr[4];
    e->a1     = cpu->gpr[5];
    e->a2     = cpu->gpr[6];
    e->a3     = cpu->gpr[7];
    e->ra     = cpu->gpr[31];
    e->v0     = v0;
    e->repeat = 1;
    e->tcb    = tcb;
    e->route  = route;
    e->in_exc = in_exc;
    s_ring_seq++;
}

uint64_t psx_hle_ring_seq(void) { return s_ring_seq; }

const PsxHleCallEntry* psx_hle_ring_entry(uint64_t seq_index)
{
    if (seq_index >= s_ring_seq) return NULL;
    if (s_ring_seq - seq_index > PSX_HLE_RING_CAP) return NULL; /* evicted */
    return &s_ring[seq_index & (PSX_HLE_RING_CAP - 1)];
}

/* ── B0 event services (SCPH1001 kernel semantics, on real guest EvCBs) ──── */

/* get_free_EvCB_slot (kernel 0x1D00): first status==0 slot index, else -1. */
static uint32_t ev_free_slot(const CPUState* cpu, uint32_t base, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (cpu->read_word(base + i * EVCB_STRIDE + EV_STATUS) == EVST_FREE)
            return i;
    }
    return 0xFFFFFFFFu;
}

/* OpenEvent (kernel 0x1D8C): claim a free EvCB, fill it, status=disabled,
 * return 0xF1000000|slot; no free slot => 0xFFFFFFFF. */
static void hle_open_event(CPUState* cpu, uint32_t base, uint32_t n)
{
    uint32_t slot = ev_free_slot(cpu, base, n);
    if (slot == 0xFFFFFFFFu) {
        cpu->gpr[2] = 0xFFFFFFFFu;
    } else {
        uint32_t ev = base + slot * EVCB_STRIDE;
        cpu->write_word(ev + EV_CLASS,  cpu->gpr[4]);
        cpu->write_word(ev + EV_SPEC,   cpu->gpr[5]);
        cpu->write_word(ev + EV_MODE,   cpu->gpr[6]);
        cpu->write_word(ev + EV_FUNC,   cpu->gpr[7]);
        cpu->write_word(ev + EV_STATUS, EVST_DISABLED);
        cpu->gpr[2] = 0xF1000000u | slot;
    }
    psx_cpu_charge(40u + 6u * (slot == 0xFFFFFFFFu ? n : slot + 1u));
}

/* CloseEvent (kernel 0x1E1C): status=free, return 1. */
static void hle_close_event(CPUState* cpu, uint32_t base)
{
    uint32_t ev = base + (cpu->gpr[4] & 0xFFFFu) * EVCB_STRIDE;
    cpu->write_word(ev + EV_STATUS, EVST_FREE);
    cpu->gpr[2] = 1u;
    psx_cpu_charge(36u);
}

/* TestEvent (kernel 0x1EC8): ready => re-arm to enabled and return 1, else 0. */
static void hle_test_event(CPUState* cpu, uint32_t base)
{
    uint32_t ev = base + (cpu->gpr[4] & 0xFFFFu) * EVCB_STRIDE;
    if (cpu->read_word(ev + EV_STATUS) == EVST_READY) {
        cpu->write_word(ev + EV_STATUS, EVST_ENABLED);
        cpu->gpr[2] = 1u;
    } else {
        cpu->gpr[2] = 0u;
    }
    psx_cpu_charge(40u);
}

/* EnableEvent (kernel 0x1F10) / DisableEvent (kernel 0x1F4C): a non-free
 * event's status is set to enabled/disabled; return 1 either way. NOTE the
 * kernel really does discard a pending READY on EnableEvent — faithful. */
static void hle_set_event_status(CPUState* cpu, uint32_t base, uint32_t status)
{
    uint32_t ev = base + (cpu->gpr[4] & 0xFFFFu) * EVCB_STRIDE;
    if (cpu->read_word(ev + EV_STATUS) != EVST_FREE)
        cpu->write_word(ev + EV_STATUS, status);
    cpu->gpr[2] = 1u;
    psx_cpu_charge(40u);
}

/* DeliverEvent (kernel 0x1B44): for every ENABLED EvCB matching class+spec,
 * mode MARK => status=READY; mode CALLBACK => call the handler with the
 * usual guest call ABI. */
static void hle_deliver_event(CPUState* cpu, uint32_t base, uint32_t n,
                              uint32_t return_address)
{
    uint32_t cls  = cpu->gpr[4];
    uint32_t spec = cpu->gpr[5];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t ev = base + i * EVCB_STRIDE;
        if (cpu->read_word(ev + EV_STATUS) != EVST_ENABLED) continue;
        if (cpu->read_word(ev + EV_CLASS) != cls)  continue;
        if (cpu->read_word(ev + EV_SPEC)  != spec) continue;
        uint32_t mode = cpu->read_word(ev + EV_MODE);
        if (mode == EVMD_MARK) {
            cpu->write_word(ev + EV_STATUS, EVST_READY);
        } else if (mode == EVMD_CALLBACK) {
            uint32_t func = cpu->read_word(ev + EV_FUNC);
            if (func != 0) {
                /* The nested dispatcher stops when the callback reaches this
                 * link. The outer call still owns its return to the game. */
                uint32_t saved_ra = cpu->gpr[31];
                cpu->gpr[31] = return_address;
                extern int g_exec_phase;   /* wall-time sampler (dirty_ram_interp.c) */
                int prev_phase = g_exec_phase;
                g_exec_phase = 3;   /* compiled route; dirty/native callees re-tag inside */
                psx_dispatch_call(cpu, func, return_address);
                g_exec_phase = prev_phase;
                cpu->gpr[31] = saved_ra;
            }
        }
        /* mode==anything else: the kernel loop skips it — so do we. */
    }
    /* Return value: the kernel returns with v0 = last comparison scratch; no
     * caller may depend on it (libapi types DeliverEvent void). Set 0 for a
     * stable, observable value. */
    cpu->gpr[2] = 0u;
    psx_cpu_charge(36u + 12u * n);
}

static void hle_undeliver_event(CPUState *cpu, uint32_t base, uint32_t count,
                                uint32_t cls, uint32_t spec)
{
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t ev = base + i * EVCB_STRIDE;
        if (cpu->read_word(ev + EV_STATUS) == EVST_READY &&
            cpu->read_word(ev + EV_CLASS) == cls &&
            cpu->read_word(ev + EV_SPEC) == spec &&
            cpu->read_word(ev + EV_MODE) == EVMD_MARK)
            cpu->write_word(ev + EV_STATUS, EVST_ENABLED);
    }
}

#if defined(PSX_NATIVE_KERNEL) && PSX_NATIVE_KERNEL
void psx_native_bios_event(CPUState *cpu, uint32_t cls, uint32_t spec, int deliver)
{
    uint32_t base = cpu->read_word(KADDR_EVCB_BASE);
    uint32_t size = cpu->read_word(KADDR_EVCB_SIZE);
    if (!base || size % EVCB_STRIDE || !psx_ram_resolve(base, size, NULL))
        return;
    uint32_t count = size / EVCB_STRIDE;
    if (deliver) {
        cpu->gpr[4] = cls;
        cpu->gpr[5] = spec;
        hle_deliver_event(cpu, base, count, PSX_NATIVE_IRQ_CALLBACK_LINK);
    } else {
        hle_undeliver_event(cpu, base, count, cls, spec);
    }
}
#endif

/* B0 vector service. Returns 1 iff serviced in HLE. */
static int hle_service_b0(CPUState* cpu, uint32_t fn)
{
    /* Kernel executive not initialized yet (InitEvents hasn't run) => the
     * EvCB table doesn't exist; everything stays LLE. */
    uint32_t base = cpu->read_word(KADDR_EVCB_BASE);
    if (base == 0) return 0;
    uint32_t n = cpu->read_word(KADDR_EVCB_SIZE) / EVCB_STRIDE;
    if (n == 0 || n > 0x1000u) return 0; /* implausible table => LLE */

    switch (fn) {
    case 0x07u: {
        uint32_t return_address = s_native_kernel_on ? cpu->gpr[31] : psx_bios_image.deliver_event_ret;
        if (!return_address) return_address = cpu->gpr[31];
        if (!return_address) return 0;
        hle_deliver_event(cpu, base, n, return_address);
        return 1;
    }
    case 0x08u: hle_open_event(cpu, base, n);                     return 1;
    case 0x09u: hle_close_event(cpu, base);                       return 1;
    case 0x0Bu: hle_test_event(cpu, base);                        return 1;
    case 0x0Cu: hle_set_event_status(cpu, base, EVST_ENABLED);    return 1;
    case 0x0Du: hle_set_event_status(cpu, base, EVST_DISABLED);   return 1;
    case 0x20u:
        hle_undeliver_event(cpu, base, n, cpu->gpr[4], cpu->gpr[5]);
        cpu->gpr[2] = 0;
        psx_cpu_charge(36u + 12u * n);
        return 1;
    default:    return 0; /* WaitEvent, threads, pads, card, ... */
    }
}

/* ── boot shell-skip ─────────────────────────────────────────────────────── */

static int hle_boot_shell_skip(CPUState* cpu)
{
    if (!s_boot_skip_on || s_shell_skipped || fntrace_is_game_started())
        return 0;
    s_shell_skipped = 1;
    /* Return straight to LoadRunShell's call site: BIOS Main() proceeds to
     * step 8 (SYSTEM.CNF + game EXE load) exactly as if the shell had exited
     * immediately with no disc-menu interaction. Kernel state at this point
     * was built entirely by the real recompiled kernel init. */
    cpu->gpr[2] = 0u;
    hle_record(psx_bios_image.shell_entry_phys, 0u, cpu, 0u, PSX_HLE_ROUTE_BOOT);
    return 1;
}

int psx_bios_hle_boot_turbo_active(void)
{
    return s_boot_skip_on && !fntrace_is_game_started();
}

/* ── the hook ────────────────────────────────────────────────────────────── */

#if defined(PSX_NATIVE_KERNEL) && PSX_NATIVE_KERNEL
static void native_exception_return(CPUState *cpu)
{
    /* SYS03's same-thread return restores the actual saved TCB and uses the
     * existing synchronous exception escape. It must not return to a guest
     * caller's ordinary C continuation. */
    cpu->gpr[4] = 3;
    cpu->gpr[5] = psx_sched_current_tcb(cpu);
    (void)psx_syscall(cpu, 0);
}

static uint32_t native_builtin_irq_call(CPUState *cpu, uint32_t node,
                                        int verifier);

static uint32_t native_irq_call(CPUState *cpu, uint32_t function)
{
    const uint32_t phys = function & 0x1fffffffu;
    if (phys >= PSX_NATIVE_IRQ_TOKENS &&
        phys - PSX_NATIVE_IRQ_TOKENS < PSX_NATIVE_IRQ_NODE_COUNT * 8 &&
        !(phys & 3u) && cpu->read_word(phys) == PSX_NATIVE_EXCEPTION_MARKER)
        return native_builtin_irq_call(cpu, (phys - PSX_NATIVE_IRQ_TOKENS) / 8,
                                       (phys & 4u) != 0);
    cpu->gpr[31] = PSX_NATIVE_IRQ_CALLBACK_LINK;
    extern int g_exec_phase;
    int previous_phase = g_exec_phase;
    g_exec_phase = 3;
    psx_dispatch_call(cpu, function, PSX_NATIVE_IRQ_CALLBACK_LINK);
    g_exec_phase = previous_phase;
    return cpu->gpr[2];
}

static uint32_t native_builtin_irq_call(CPUState *cpu, uint32_t node,
                                        int verifier)
{
    const uint32_t events = cpu->read_word(KADDR_EVCB_BASE);
    const uint32_t count = cpu->read_word(KADDR_EVCB_SIZE) / EVCB_STRIDE;
    if (node < 4) {
        const uint32_t bits[] = {1u << IRQ_TIMER0, 1u << IRQ_TIMER1,
                                  1u << IRQ_TIMER2, 1u << IRQ_VBLANK};
        const uint32_t bit = bits[node];
        if (verifier) {
            if (!(cpu->read_word(0x1f801070u) & cpu->read_word(0x1f801074u) & bit))
                return 0;
            /* Event delivery belongs to the verifier even if a game replaces
             * or removes the HandlerInfo's acknowledgement callback. */
            cpu->gpr[4] = 0xf2000000u + node;
            cpu->gpr[5] = 2;
            cpu->gpr[31] = PSX_NATIVE_IRQ_CALLBACK_LINK;
            hle_deliver_event(cpu, events, count, PSX_NATIVE_IRQ_CALLBACK_LINK);
            return 1;
        }
        if (cpu->read_word(PSX_NATIVE_TIMER_AUTO_ACK + node * 4u)) {
            cpu->write_word(0x1f801070u, ~bit);
            native_exception_return(cpu);
            exit(1);
        }
    } else if (node == PSX_NATIVE_PAD_CARD_IRQ_NODE) {
        if (verifier)
            return (cpu->read_word(0x1f801070u) & cpu->read_word(0x1f801074u) &
                    (1u << IRQ_VBLANK)) != 0;
        psx_native_card_vblank(cpu);
    } else if (node == 4 && verifier) {
        const uint8_t order[] = {IRQ_CDROM, IRQ_SPU, IRQ_GPU, IRQ_PIO,
            IRQ_SIO1, IRQ_VBLANK, IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_SIO0, IRQ_DMA};
        /* BIOS event classes are not the traversal index. Timer 2 shares
         * timer 1's generic event class, as in the pinned kernel. */
        const uint8_t classes[] = {3, 9, 2, 10, 11, 1, 5, 6, 6, 8, 4};
        for (unsigned i = 0; i < sizeof order; ++i) {
            const uint32_t irq = order[i], bit = 1u << irq;
            if (!(cpu->read_word(0x1f801070u) & cpu->read_word(0x1f801074u) & bit))
                continue;
            cpu->gpr[4] = 0xf0000000u + classes[i];
            cpu->gpr[5] = 0x1000;
            cpu->gpr[31] = PSX_NATIVE_IRQ_CALLBACK_LINK;
            hle_deliver_event(cpu, events, count, PSX_NATIVE_IRQ_CALLBACK_LINK);
            if (cpu->read_word(PSX_NATIVE_GENERIC_IRQ_ACK + irq * 4))
                cpu->write_word(0x1f801070u, ~bit);
        }
    } else if (node == 5 && verifier) {
        const uint32_t tcb = psx_sched_current_tcb(cpu);
        if (!tcb || !psx_ram_resolve(tcb, 0xc0, NULL)) {
            fprintf(stderr, "native syscall verifier: invalid current thread\n");
            exit(1);
        }
        const uint32_t cause = cpu->read_word(tcb + 0x98u) & 0x3cu;
        if (!cause) return 0; /* hardware IRQ */
        if (cause != 0x20u) {
            fprintf(stderr, "native syscall verifier: unsupported exception %02x\n", cause);
            exit(1);
        }
        cpu->write_word(tcb + 0x88u, cpu->read_word(tcb + 0x88u) + 4);
        const uint32_t fn = cpu->read_word(tcb + 8 + 4 * 4);
        uint32_t sr = cpu->read_word(tcb + 0x94u);
        if (fn == 1) {
            cpu->write_word(tcb + 0x10u, (sr & 0x404u) == 0x404u);
            cpu->write_word(tcb + 0x94u, sr & ~0x404u);
        } else if (fn == 2) {
            cpu->write_word(tcb + 0x94u, sr | 0x404u);
        } else if (fn == 3) {
            const uint32_t target = cpu->read_word(tcb + 8 + 5 * 4);
            const uint32_t process = cpu->read_word(0x108u);
            if (!psx_ram_resolve(target, 0xc0, NULL) || !psx_ram_resolve(process, 4, NULL)) {
                fprintf(stderr, "native syscall verifier: invalid thread target\n");
                exit(1);
            }
            cpu->write_word(process, target);
            cpu->write_word(tcb + 0x10u, 1);
        } else if (fn != 0) {
            cpu->gpr[4] = 0xf0000010u;
            cpu->gpr[5] = 0x4000;
            cpu->gpr[31] = PSX_NATIVE_IRQ_CALLBACK_LINK;
            hle_deliver_event(cpu, events, count, PSX_NATIVE_IRQ_CALLBACK_LINK);
        }
        native_exception_return(cpu);
        exit(1);
    }
    return 0;
}

static void native_irq_chain(CPUState *cpu, unsigned priority)
{
    uint32_t handlers = cpu->read_word(0x100u);
    if (!handlers) return;
    if (!psx_ram_resolve(handlers, 32, NULL)) {
        fprintf(stderr, "native IRQ: invalid priority table\n");
        exit(1);
    }
    uint32_t node = cpu->read_word(handlers + priority * 8);
    uint32_t remaining = g_psx_ram_size / 4;
    while (node) {
        if (!remaining-- || (node & 3u) || !psx_ram_resolve(node, 12, NULL)) {
            fprintf(stderr, "native IRQ: invalid handler chain at priority %u\n", priority);
            exit(1);
        }
        uint32_t handler = cpu->read_word(node + 4);
        uint32_t verifier = cpu->read_word(node + 8);
        if (verifier) {
            uint32_t result = native_irq_call(cpu, verifier);
            if (result && handler) {
                cpu->gpr[4] = result;
                native_irq_call(cpu, handler);
            }
        }
        /* The callback may change or unlink its own HandlerInfo. Read the
         * next pointer only after it returns, as the kernel does. */
        node = cpu->read_word(node);
    }
}

static void native_exception_entry(CPUState *cpu)
{
    const uint32_t tcb = psx_sched_current_tcb(cpu);
    if (!tcb || !psx_get_in_exception()) {
        fprintf(stderr, "native exception entry without an active TCB\n");
        exit(1);
    }

    /* IRQ delivery has already pushed SR. The shared scheduler context
     * writer pushes it when persisting a context, so temporarily unpush the
     * low bits to record the actual exception-state SR just once. */
    uint32_t sr = cpu->cop0[12];
    cpu->cop0[12] = (sr & ~0x3fu) | ((sr >> 2) & 0xfu);
    psx_sched_save_context(cpu, tcb, cpu->cop0[14]);
    cpu->cop0[12] = sr;

    const uint32_t pending = cpu->read_word(0x1f801070u) & cpu->read_word(0x1f801074u);
    if (cpu->read_word(PSX_NATIVE_CD_HANDLERS_ACTIVE) &&
        (pending & ((1u << IRQ_CDROM) | (1u << IRQ_DMA)))) {
        fprintf(stderr, "native BIOS CD IRQ provider gap: kernel CD handlers still active\n");
        exit(1);
    }
    cpu->gpr[29] = PSX_NATIVE_EXCEPTION_STACK;
    cpu->gpr[30] = PSX_NATIVE_EXCEPTION_STACK;
    cpu->gpr[28] = 0;

    /* Native providers are ordinary writable HandlerInfo nodes. C0 enqueue,
     * guest prepend/unlink and patched callback pointers all affect this same
     * priority walk; no second implicit delivery follows the registered chain. */
    for (unsigned priority = 0; priority < 4; ++priority) {
        native_irq_chain(cpu, priority);
    }
    const uint32_t jump = cpu->read_word(PSX_NATIVE_EXCEPTION_JUMP_PTR);
    if (!jump) {
        native_exception_return(cpu);
        exit(1);
    }
    if (!psx_ram_resolve(jump, 48, NULL)) {
        fprintf(stderr, "native IRQ provider gap: no usable custom exception hook "
                "(I_STAT=%04x I_MASK=%04x)\n",
                cpu->read_word(0x1f801070u), cpu->read_word(0x1f801074u));
        exit(1);
    }
    cpu->gpr[4] = jump;
    cpu->gpr[5] = 1;
    if (psx_native_bios_service(cpu, 0xa0, 0x14) != PSX_NATIVE_BIOS_TRANSFER) {
        fprintf(stderr, "native exception hook failed to restore its jump buffer\n");
        exit(1);
    }
    psx_restore_state_escape();
}
#endif

#if defined(PSX_NATIVE_KERNEL) && PSX_NATIVE_KERNEL
static void native_bios_unavailable(CPUState *cpu, uint32_t vector, uint32_t fn)
{
    fprintf(stderr, "native BIOS service unavailable: %02x:%02x ra=%08x "
            "args=%08x,%08x,%08x,%08x\n", vector, fn, cpu->gpr[31],
            cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
    exit(1);
}

/* WaitEvent's original 0x1e44..0x1ec4 routine consumes READY once, returns
 * zero for an initially inactive event, and otherwise waits without changing
 * threads. Replace its guest polling loop with device deadlines. The event
 * pointer lives in V1, as in the original routine, and the resume token lives
 * in guest RAM so IRQ thread changes and current-build snapshots can resume
 * without relying on an abandoned host stack. */
static void native_wait_event(CPUState *cpu, int resume)
{
    uint32_t event = resume ? cpu->gpr[3] :
        cpu->read_word(KADDR_EVCB_BASE) + (cpu->gpr[4] & 0xffffu) * EVCB_STRIDE;
    if (!psx_ram_resolve(event, EVCB_STRIDE, NULL))
        native_bios_unavailable(cpu, 0xb0, 0x0a);
    uint32_t status = cpu->read_word(event + EV_STATUS);
    if (!resume && status != EVST_ENABLED && status != EVST_READY) {
        cpu->gpr[2] = 0;
        cpu->pc = cpu->gpr[31];
        psx_cpu_charge(40);
        return;
    }
    cpu->gpr[3] = event;
    while (cpu->read_word(event + EV_STATUS) != EVST_READY) {
        const uint32_t next = psx_idle_cycles_to_next_observable_event();
        cpu->pc = 0x80000000u | PSX_NATIVE_WAIT_EVENT_RESUME;
        psx_advance_cycles(next ? next : 1u);
        psx_check_interrupts_at(cpu, cpu->pc);
    }
    cpu->write_word(event + EV_STATUS, EVST_ENABLED);
    cpu->gpr[2] = 1;
    cpu->pc = cpu->gpr[31];
    psx_cpu_charge(40);
}

static int native_stub_unchanged(CPUState *cpu, const PsxNativeBiosTable *table, uint32_t fn)
{
    uint32_t stub = psx_native_bios_stub(table, fn);
    for (uint32_t word = 0; word < psx_native_bios_stub_words(table, fn); ++word) {
        if (cpu->read_word(stub + word * 4) != psx_native_bios_stub_word(table, fn, word))
            return 0;
    }
    return 1;
}

static int native_service_dispatch(CPUState *cpu, uint32_t vector, uint32_t fn)
{
    if (vector == 0xb0 && fn == 0x0a) {
        native_wait_event(cpu, 0);
        hle_record(vector, fn, cpu, cpu->gpr[2], PSX_HLE_ROUTE_HLE);
        return 1;
    }
    if (vector == 0xb0 && fn == 0x17 && psx_get_in_exception()) {
        native_exception_return(cpu);
        exit(1);
    }
    int handled = vector == 0xb0 && hle_service_b0(cpu, fn);
    if (!handled) handled = psx_native_bios_service(cpu, vector, fn);
    if (!handled) native_bios_unavailable(cpu, vector, fn);
    hle_record(vector, fn, cpu, cpu->gpr[2], PSX_HLE_ROUTE_HLE);
    if (handled == PSX_NATIVE_BIOS_TRANSFER) {
        if (psx_get_in_exception()) psx_restore_state_escape();
        else psx_scheduler_resume_at(cpu->pc);
    }
    return 1;
}
#endif

static int bios_hle_dispatch(struct CPUState* cpu, uint32_t phys)
{
#if defined(PSX_NATIVE_KERNEL) && PSX_NATIVE_KERNEL
    if (s_native_kernel_on) {
        const uint32_t pad_start = psx_native_pad_patch_helper(1) & 0x1fffffffu;
        const uint32_t pad_stop = psx_native_pad_patch_helper(0) & 0x1fffffffu;
        if (phys == pad_start || phys == pad_stop) {
            if (cpu->read_word(phys) != PSX_NATIVE_EXCEPTION_MARKER) return 0;
            cpu->write_word(PSX_NATIVE_CARD_PAD_STARTED, phys == pad_start);
            psx_cpu_charge(16);
            return 1;
        }
        if (phys == PSX_NATIVE_WAIT_EVENT_RESUME) {
            if (cpu->read_word(phys) != PSX_NATIVE_EXCEPTION_MARKER) return 0;
            native_wait_event(cpu, 1);
            return 1;
        }
        if (phys == PSX_NATIVE_EXCEPTION_ENTRY || phys == PSX_NATIVE_EXCEPTION_PATCH_RESUME) {
            if (cpu->read_word(phys) != PSX_NATIVE_EXCEPTION_MARKER) return 0;
            if (phys == PSX_NATIVE_EXCEPTION_PATCH_RESUME) {
                /* The writable SDK prefix has already preserved V0 in
                 * the current thread and used the live V0 for Cause. */
                uint32_t tcb = psx_sched_current_tcb(cpu);
                if (!tcb || !psx_get_in_exception() ||
                    !psx_ram_resolve(tcb, 0xc0u, NULL)) {
                    fprintf(stderr, "native exception continuation without a live TCB\n");
                    exit(1);
                }
                cpu->gpr[2] = cpu->read_word(tcb + 8u + 2u * 4u);
            }
            native_exception_entry(cpu);
            exit(1); /* exception escape must not return */
        }
        if (phys == PSX_NATIVE_INFO_STUB) {
            const PsxNativeBiosTable *table = &psx_native_bios_tables[0];
            if (!native_stub_unchanged(cpu, table, 0x9d)) return 0;
            cpu->gpr[9] = 0x9d;
            return native_service_dispatch(cpu, 0xa0, 0x9d);
        }
        for (unsigned i = 0; i < sizeof(psx_native_bios_tables) / sizeof(psx_native_bios_tables[0]); ++i) {
            const PsxNativeBiosTable *table = &psx_native_bios_tables[i];
            if (phys == table->token) {
                if (cpu->read_word(phys) != PSX_NATIVE_EXCEPTION_MARKER) return 0;
                uint32_t fn = cpu->gpr[9];
                if (fn >= table->count) native_bios_unavailable(cpu, table->vector, fn);
                return native_service_dispatch(cpu, table->vector, fn);
            }
            if (phys == table->vector) {
                for (uint32_t word = 0; word < 4; ++word) {
                    if (cpu->read_word(phys + word * 4) !=
                        psx_native_bios_vector_word(table, word))
                        return 0; /* guest-installed vector body owns the call */
                }
                uint32_t fn = cpu->gpr[9];
                if (fn >= table->count) native_bios_unavailable(cpu, table->vector, fn);
                uint32_t target = cpu->read_word(table->table + fn * 4);
                if (target == psx_native_bios_stub(table, fn) &&
                    native_stub_unchanged(cpu, table, fn))
                    return native_service_dispatch(cpu, table->vector, fn);

                /* A guest-modified table entry or thunk owns this call.
                 * Keep ordinary nested dispatch and mod hooks active. */
                if (!target) native_bios_unavailable(cpu, table->vector, fn);
                psx_dispatch_call(cpu, target, cpu->gpr[31]);
                return 1;
            }
            if (phys >= table->stubs && phys - table->stubs < table->count * 12u &&
                (phys - table->stubs) % 12u == 0) {
                uint32_t fn = (phys - table->stubs) / 12u;
                if (phys != (psx_native_bios_stub(table, fn) & 0x1fffffffu)) return 0;
                if (!native_stub_unchanged(cpu, table, fn)) return 0;
                cpu->gpr[9] = fn;
                return native_service_dispatch(cpu, table->vector, fn);
            }
        }
        if (phys == PSX_NATIVE_BOOT_RETURN_PHYS ||
            (phys >= 0x1fc00000u && phys < 0x1fc80000u)) {
            fprintf(stderr, "native BIOS entry unavailable: %08x ra=%08x\n",
                    phys, cpu->gpr[31]);
            exit(1);
        }
        return 0;  /* Normal game dispatch and runtime overlays remain active. */
    }
#endif
    if (phys == 0xB0u) {
        uint32_t fn = cpu->gpr[9]; /* $t1 — the PSY-Q thunk's function number */
        int handled = s_call_hle_on ? hle_service_b0(cpu, fn) : 0;
        hle_record(0xB0u, fn, cpu, handled ? cpu->gpr[2] : 0u,
                   handled ? PSX_HLE_ROUTE_HLE : PSX_HLE_ROUTE_LLE);
        return handled;
    }
    if (phys == 0xA0u || phys == 0xC0u) {
        /* No A0/C0 services implemented in v1 — observe + fall through. */
        hle_record(phys, cpu->gpr[9], cpu, 0u, PSX_HLE_ROUTE_LLE);
        return 0;
    }
    if (psx_bios_image.shell_entry_phys != 0 &&
        phys == psx_bios_image.shell_entry_phys)
        return hle_boot_shell_skip(cpu);
    return 0;
}

/* ── selection ───────────────────────────────────────────────────────────── */

int psx_bios_hle_dispatch_quiet(uint32_t address) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    if (!g_psx_bios_hle_hook) return 1;
    if (g_psx_bios_hle_hook != bios_hle_dispatch) return 0;
    if (s_native_kernel_on)
        return phys != 0xA0u && phys != 0xB0u && phys != 0xC0u &&
               !psx_native_bios_dispatch_marker(phys) && phys != PSX_NATIVE_INFO_STUB &&
               (phys < 0x8e00u || phys >= 0x9f00u) &&
               (phys < 0x1fc00000u || phys >= 0x1fc80000u);
    return phys != 0xA0u && phys != 0xB0u && phys != 0xC0u &&
        (!psx_bios_image.shell_entry_phys || phys != psx_bios_image.shell_entry_phys);
}

void psx_bios_hle_configure(int call_hle, int boot_skip)
{
    s_native_kernel_on = 0;
    s_call_hle_on = call_hle && psx_bios_hle_event_abi_supported();
    /* Clamp to what THIS image can actually do, so the state the banner and
     * `hle_dump` report can never claim a skip that bios_hle_dispatch below is
     * structurally unable to perform. Callers are expected to have gone
     * through psx_bios_hle_plan(); this is the backstop, not the policy. */
    s_boot_skip_on = (boot_skip && psx_bios_image.shell_entry_phys != 0) ? 1 : 0;
    /* Rematch / session_reboot re-enters bring-up without process exit; the
     * shell-skip latch must arm again or peers run the interactive shell and
     * appear hung after netplay lockstep. */
    s_shell_skipped = 0;
    g_psx_bios_hle_hook =
        (s_call_hle_on || s_boot_skip_on) ? &bios_hle_dispatch : NULL;
}

void psx_bios_hle_configure_native(void)
{
    s_native_kernel_on = 1;
    s_call_hle_on = 1;
    s_boot_skip_on = 0;
    g_psx_bios_hle_hook = bios_hle_dispatch;
}

int psx_bios_hle_enabled(void)           { return s_call_hle_on; }
int psx_bios_hle_boot_skip_enabled(void) { return s_boot_skip_on; }

const char* psx_bios_hle_backend_name(void)
{
    if (s_native_kernel_on) return "HLE (native kernel)";
    return s_call_hle_on ? "HLE (LLE fallback)" : "LLE (recompiled BIOS)";
}
