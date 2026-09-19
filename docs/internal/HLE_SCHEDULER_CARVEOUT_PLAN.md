# PSX Cooperative-Thread Scheduler — Deterministic Carve-Out Plan

Status: DRAFT for ChatGPT confer (2026-06-29). Sub-plan of `FAITHFUL_TIMING_PLAN.md`.
Governed by CLAUDE.md §0 AMENDMENT (LLE-first; faithful HLE subsystem replacement
permitted) + recomp-template `PRINCIPLES.md` "LLE Is the Baseline; HLE Is a
Subsystem Replacement, Not a Starting Point".

## 0. Why this is a *legitimate* carve-out (policy check)

The four bars from the amendment, all met:
1. **Genuine LLE landmine — non-determinism with no hardware analog.** The host
   coroutine/fiber bridge (`traps.c`) maps each guest thread to a Win32/ucontext
   fiber and overloads `cpu->pc = 0` as a host control signal. This is the root
   surface of the MMX6 cutscene→gameplay freeze and the documented run-to-run
   non-determinism. Fibers have no PS1 analog.
2. **General, not per-game.** Every PSX title's BIOS threads go through the same
   ChangeThread/ReturnFromException/exception path. The replacement is keyed to
   the documented kernel mechanism, not to any title.
3. **Operates on real guest structures + documented mechanism.** It reads/writes
   the actual guest TCB register arrays (`docs/psx_bios_disasm.txt` ExceptionHandler
   0xC80 / ReturnFromException 0xF40 / TCBH `dword_108->entry`), so the rest of the
   still-LLE system sees consistent state.
4. **Beetle-validated.** A thread/event-stream conformance diff vs the independent
   Beetle oracle is part of the deliverable.

This is a *subsystem replacement* on a proven LLE baseline — the recompiled BIOS
stays everywhere else. It is NOT a stub/fake (it runs the faithful mechanism and is
oracle-checked), and NOT the starting point.

## 1. The documented BIOS contract (ground truth)

From `docs/psx_bios_disasm.txt`:
- **TCBH `dword_108->entry` = the current TCB pointer.** This single pointer is the
  whole notion of "which thread is running."
- **ExceptionHandler (0xC80):** save ALL guest regs (incl. SP, EPC, SR, Cause, HI/LO)
  into the *current* TCB's `reg[]`; run the 4 IntRP callback queues (ToT[0], the
  interrupt/event dispatch); restore the kernel's own EntryInt jmp_buf and `jr ra`.
- **ReturnFromException (0xF40):** restore ALL regs FROM the current TCB; `jr k0`
  (= restored EPC) to resume that thread.
- **ChangeThread:** set `dword_108->entry` to the target TCB. The next save/restore
  then targets that thread. **No stack switch** — the thread's guest SP is in its TCB.

Key consequence: a thread's entire context (regs + guest SP + resume EPC) is in its
TCB. Switching threads = restore the other TCB and resume at its EPC. The host C call
stack is irrelevant to guest thread semantics.

## 2. What we have now + its non-determinism (`runtime/src/cpu/traps.c`)

- `psx_save_context_to_tcb` / `psx_restore_context_from_tcb` — **FAITHFUL, KEEP.**
  They already write/read the documented TCB register array (tcb+8 + reg index*4,
  +128 EPC, +132 HI, +136 LO, +140 SR, +144 Cause).
- `psx_change_thread_fiber` / `psx_thread_fiber_entry` / `psx_get_or_create_host_thread`
  — **the host-fiber bridge. This is the carve-out target.** Problems:
  - `current==target` → `cpu->pc = 0; return 1` — `cpu->pc=0` is overloaded as a
    "same-thread/no-op" host signal, but the dispatch layer reads `pc=0` as "guest
    completed," killing an infinite cooperative thread.
  - Cross-thread switch = create/reuse a host fiber, `psx_fiber_switch`. Fiber
    lifetime is non-deterministic; a runnable thread whose fiber "returns" gets
    recreated-per-frame (the MMX6 symptom).
  - `psx_thread_fiber_entry` marks `slot->closed` when dispatch returns — conflating
    "guest thread yielded" with "host fiber ended."

## 3. Proposed design — deterministic TCB scheduler (no host fibers)

**Core loop (the unwind boundary).** Replace the fiber switch with a top-level
scheduler trampoline entered at the exception/syscall boundary:

```
for (;;) {
    uint32_t epc = psx_restore_context_from_tcb(cpu, current_tcb);  // sets guest SP too
    cpu->pc = epc;
    psx_dispatch(cpu, epc);            // runs the thread until it yields
    // a yield (ChangeThread / RFE-to-other / WaitEvent-block) longjmps back here
    current_tcb = g_next_tcb;          // set by the yield, = dword_108->entry
}
```

- **Thread switch (ChangeThread / cross-thread RFE):** save current context to the
  current TCB, set `dword_108->entry = target` (the real guest pointer), set
  `g_next_tcb = target`, and `longjmp` back to the scheduler loop. The native stack
  unwinds; nothing is lost because guest control flow is dispatch-based (tail-call
  trampoline + every-block-leader re-entry, the CLASS-B fix) and all guest state is
  in CPUState + guest RAM + the TCB.
- **Same-thread RFE (`current==target`):** restore the current TCB, set `cpu->pc =
  EPC`, return into dispatch normally — **never** `cpu->pc = 0`. (This is ChatGPT's
  earlier fix, now structural rather than a fiber special-case.)
- **No host fibers, no per-thread native stacks, no `cpu->pc=0` sentinel.** Fully
  deterministic: thread selection is a function of guest TCB state only.

**Open design question (confer):** how much to HLE.
- (A) *Minimal — replace only the switch MECHANISM.* Keep the recompiled BIOS
  ExceptionHandler/RFE running as LLE; only swap the fiber switch for the
  unwind+dispatch loop. Most LLE-faithful; smallest change.
- (B) *Fuller — HLE the scheduler/exception primitives.* A C reimplementation of
  ChangeThread/RFE/exception-entry on the TCBs, bypassing the recompiled BIOS for
  those functions. More deterministic/faster, more HLE.
Lean: start at (A) — it removes the non-determinism (which lives in *our* bridge,
not the BIOS) while staying maximally LLE; escalate to (B) only if (A) can't be made
deterministic.

## 4. Risks / unknowns (confer)

- **Native C-locals across a yield.** GAME code keeps all state in gpr/RAM (CLASS-B
  safe), but the recompiled BIOS emitter uses `psx_delay_` C-locals. If a yield can
  occur while BIOS code holds C-local state, unwinding loses it. Mitigation: yields
  occur at defined syscall/exception boundaries; characterize whether any yield
  unwinds through BIOS C-local frames.
- **longjmp vs the existing exception_jmpbuf / pending-longjmp machinery** already in
  traps.c — must unify, not stack two escape mechanisms.
- **Nested exceptions / critical sections** (EnterCriticalSection IM masking) and the
  IntRP callback queue running mid-switch.
- **Re-entrancy of `psx_dispatch`** from the scheduler loop vs from normal calls.

## 5. Diagnosis-confirmation gate (do FIRST, cheap, no rebuild risk)

Before committing to the carve-out, confirm the bug is the scheduler seam (#2) and
not the cycle model (#1), via the **thread1 timeline conformance diff** (now that the
exact-store-PC tooling is fixed): a thread1-scoped ring (dispatch/call/return/syscall/
RFE/ChangeThread + sp/TCB_EPC + value@0x801FEB78 + state-handler inputs) on our build
AND Beetle, diffed from the last common milestone (func_8001CB3C struct memset) to the
first native-only bad outcome. First divergent row classifies state-machine-read vs
scheduler-order vs device-timing. (The carve-out has determinism value regardless, but
we carve the RIGHT thing only after this.)

## 6. Conformance test (the durable validation)

Cross-process thread/event-stream diff (CLAUDE.md §16 two-process harness): same
boot→checkpoint on `psx-runtime` + `psx-beetle`, capture ChangeThread/RFE/DeliverEvent/
WaitEvent/TCB-state transitions with guest-cycle+TCB+EPC, assert row-for-row match.
This is both the carve-out's acceptance test and a permanent regression guard.

## 7. Phasing

1. Confirm diagnosis (§5) — thread1 timeline diff vs Beetle.
2. Build the conformance harness (§6) — reusable.
3. Implement design (A) — deterministic TCB scheduler, remove fibers.
4. Validate: MMX6 reaches gameplay (user visual), conformance diff passes, CROSS-TITLE
   regress (Tomba1/Tomba2/BIOS boot) — Rule -1 / pre-merge gates.
5. Only then: merge; resume Stage-2 cycle-accuracy / enhancement phase.

## 8. Confer questions for ChatGPT

1. Is the unwind+dispatch TCB-scheduler (longjmp to a top-level loop, re-dispatch the
   target EPC, no host fibers) sound given dispatch-based guest control flow + every-
   block-leader re-entry? Any case where a host stack IS load-bearing for guest
   semantics (BIOS `psx_delay_` C-locals across a yield)?
2. Minimal (A) vs fuller-HLE (B) — which, and where's the line?
3. How to unify the new longjmp boundary with the existing exception_jmpbuf /
   pending-exception-longjmp / EnterCriticalSection machinery without double-escape.
4. Nested-exception / IntRP-callback-queue-mid-switch hazards to design for up front.
5. Does this design plausibly fix the MMX6 freeze (thread1 parked in func_8002000C
   reading 0x801FEB78), or is that gated on a separate cycle-timing (#1) root we must
   confirm first?

## 9. ChatGPT confer result (2026-06-29) — design APPROVED ("A+"), with invariants

ChatGPT endorsed the carve-out: **remove the host-fiber bridge, keep the BIOS
event/thread semantics LLE.** "The non-determinism is not the BIOS contract; it is
your bridge layered on top of it." Choice: **A+ (not full B)** — replace the bridge
only; keep BIOS ExceptionHandler / IntRP callback queues / Open-Enable-Deliver-Wait
event semantics / TCB save-restore layout as LLE.

**The one invariant the design lives or dies on:** guest thread context lives ONLY in
`CPUState` / RAM / TCB reg array — NEVER in a host fiber stack, generated **C locals**,
a `pc=0` sentinel, or a global resume PC. A yield/switch/RFE may unwind the host stack
ONLY after all guest-visible state is committed (gpr[], hi/lo, load-delay/interlock,
branch-delay/EPC, `cpu->pc` = real dispatchable resume PC; current TCB saved if
suspending), and the escape happens at a **block-leader boundary** — never longjmp
from arbitrary helper code mid-translated-instruction.

**The C-local hazard (my Q1) — the real one to engineer:** the recompiled BIOS emitter
uses `psx_delay_*` C locals for load-delay/interlock. Rule: *no scheduler escape while
pending delay/interlock state exists only in C locals* — flush it to CPUState before
any escape (or keep escapes strictly at clean block-leader boundaries where no such
pending state exists).

**Structured escapes, ONE path (replaces pc=0 and unifies with exception_jmpbuf):**
```
enum psx_run_reason_t { CONTINUE, YIELD_TO_TCB, RESUME_CURRENT, GUEST_EXIT, FATAL };
struct psx_sched_escape_t { reason; uint32_t target_tcb; uint32_t resume_pc; };
// outer scheduler loop:
for (;;) {
    uint32_t cur = psx_current_tcb();
    uint32_t pc  = psx_restore_context_from_tcb(cpu, cur);   // restores guest SP too
    if (!psx_is_dispatchable(pc)) fatal_bad_tcb_epc(cur, pc);// fail-closed
    cpu->pc = pc;
    psx_dispatch(cpu, pc);
    switch (cpu->sched.reason) { ... YIELD_TO_TCB -> cur=target; RESUME_CURRENT; GUEST_EXIT->return; default: fatal; }
}
// thread switch:  save_to_tcb(cur,resume_pc); set_current_tcb(target); sched={YIELD_TO_TCB,target}; longjmp(scheduler_jmpbuf,1);
// same-thread RFE: restore_from_tcb(cur); sched={RESUME_CURRENT}; longjmp(scheduler_jmpbuf,1);
```
Existing `exception_jmpbuf` may remain internally during transition, but its catch must
**immediately translate into the scheduler escape object** — it must NOT independently
restore GPRs or rewrite PC. **Cross-thread switches ALWAYS return to the scheduler
boundary** — never restore the target inside a nested syscall handler and continue on
the same host stack. Same-thread RFE initially routes through the same boundary too
(one path; optimize later).

**The invariant that already bit us once:** after a BIOS/TCB restore,
`psx_check_interrupts` must NOT restore old `saved_gpr[]` over it.

**Nested-exception / IntRP guards to design up front (my Q4):** track `exception_depth`,
`in_int_rp_callback`, `in_tcb_save_restore`, `critical_depth`/IM-mask; allow a scheduler
escape ONLY when the current TCB context is complete (never switch mid register-save
sequence or mid-IntRP-queue). IntRP callback queue ORDER must be preserved across an
escape. EnterCriticalSection affects interrupt *delivery*, not thread *identity* — keep
those concerns separate.

**MMX6 fix verdict (my Q5):** plausibly fixes the **fiber-exit/recreate class**, but do
NOT assume it fixes the whole cutscene transition until the **thread1 timeline diff**
confirms the first divergence is scheduler-induced. The "273 frames early" could be a
separate producer/timing root that the scheduler swap would leave behind. ⇒ §5
diagnosis gate stays FIRST.

### Minimal implementation plan (ChatGPT, 8 steps)
1. Add `scheduler_jmpbuf` + `psx_sched_escape_t`.
2. Replace `psx_change_thread_fiber` with `psx_request_thread_switch(target_tcb, resume_pc)`.
3. `psx_request_thread_switch` saves current TCB + longjmps to scheduler with YIELD_TO_TCB.
4. `ReturnFromException`/RFE restores from current TCB or requests a target switch, then
   longjmps with a structured reason.
5. Remove ALL `cpu->pc = 0` host-control signaling.
6. Make a runnable-thread dispatch returning `pc=0` fail-fast.
7. Keep BIOS TCB save/restore layout as-is.
8. Add trace rings: `sched_escape_ring` (reason, current_tcb, target_tcb, resume_pc,
   pc, ra, sp) + `tcb_save_restore_ring` (tcb, epc, sp, ra, status, cause).

### Revised order of operations
1. **§5 diagnosis gate first** — thread1 timeline diff vs Beetle (now that exact-store-PC
   tooling is fixed) to confirm scheduler-induced divergence.
2. Build §6 conformance harness (reusable).
3. Implement the 8-step minimal plan + the C-local-flush / nested-exception guards.
4. Validate: MMX6 gameplay (user-visual) + conformance diff + CROSS-TITLE regress
   (Tomba1/Tomba2/BIOS boot) per Rule -1 / pre-merge gates.

## 10. PROGRESS (branch spike/hle-tcb-scheduler)

- ✅ **§5 diagnosis gate PASSED** (2026-06-29) — scheduler-induced confirmed: thread1's
  host fiber recreated EVERY frame (thread_trace kind10 fiber_entry → kind13 dispatch-exit
  pc=0 → kind11 closed → repeat). func_8002000C confirmed legit (Beetle runs it too, from
  caller 0x800215A4). Residual to validate post-impl: whether the ~273-frames-early
  upstream divergence still parks func_8002000C on thread1 early.
- ✅ **Step 1 DONE** (commit ab8ed36) — `runtime/include/psx_scheduler.h` (psx_run_reason_t
  + psx_sched_escape_t + design/invariant doc) + traps.c definitions (g_scheduler_jmpbuf,
  g_sched_escape, psx_is_dispatchable fail-closed guard rejecting 0 + the exc sentinel).
  INERT (host-fiber bridge still authoritative), builds clean (traps.c only, no game-C
  regen), no behavior change.
- ✅ **Steps 2-8 (MINIMAL "A+") IMPLEMENTED + structurally validated** (2026-06-29,
  runtime-only, UNCOMMITTED). Scope refined from the broad 8-step list to the minimal
  bridge replacement ChatGPT endorsed: the Beetle-confirmed non-determinism lives ONLY
  in the `!in_exception` host-fiber bridge; the in-exception RFE path already escapes via
  `exception_jmpbuf` (no fibers) and was left intact. Implemented:
  - `psx_scheduler_run` (traps.c) — outer trampoline wrapping the top-level psx_dispatch
    (main.cpp now calls it instead of bare `psx_dispatch`); setjmp(g_scheduler_jmpbuf),
    restore-current-TCB / dispatch / on-escape reset g_psx_dispatch_depth + g_psx_call_bail;
    boot/pre-TCB runs raw cpu->pc; one-level g_sched_return_tcb switch-back safety net;
    fail-closed psx_is_dispatchable on every restored EPC.
  - `psx_request_thread_switch` replaces the fiber switch: save yielder ctx (resume=$ra),
    set dword_108->entry=target, longjmp to the scheduler. No fibers, no cpu->pc=0 signal.
    Only fires with in_exception==0 ⇒ never unwinds a live exception_jmpbuf frame.
  - `sched_escape_ring` (step 8) added. Fiber funcs kept compiled-but-dead this pass.
  - **HIDDEN TOGGLE**: env `PSX_HLE_SCHEDULER` (default 1=HLE; 0=legacy LLE fibers),
    read once (mode can't flip mid-run); `psx_change_thread` branches on it; startup
    banner prints the active mode. Trampoline is transparent in LLE mode.
  - **VALIDATED LIVE (HLE)**: thread1 trace now shows ONLY kinds 3/8/20/1/2 (new
    TCB-switch path, 50× cross-thread commits) and ZERO fiber kinds 10/11/13 — the
    per-frame fiber recreate is ELIMINATED. Builds clean (no -Werror).
- ⛔ **Freeze NOT resolved — separate UPSTREAM root confirmed** (ChatGPT Q5 caution
  realized). HLE build reaches the IDENTICAL pre-freeze state (gp0_draw frozen at 337079)
  and wedges on the SAME `0x800CD3F8` dispatch miss, now a tight loop (~4000/s; render
  thread stuck, gp0_writes still advancing + audio plays ⇒ black screen + music). The
  scheduler swap removed the fiber-recreate class but the 0xCD3F8 wedge has an upstream
  cause (the ~273-frames-early thread1 divergence). **Per user (2026-06-29): keep HLE
  default until the upstream root is fixed.**
- ⏭ **NEXT — UPSTREAM ROOT**: thread1 first-divergence timeline diff vs Beetle (where
  does 0x800CD3F8 land on thread1's stack/dispatch path, and where does our thread1 PC
  first diverge from Beetle's). BLOCKER (Rule 15): Beetle's fntrace per-target filter is
  broken (total is global, filter ignored) — FIX that tool before trusting Beetle
  per-target traces. THEN diff from the last common milestone to the first 0xCD3F8.
- ⏭ **DEFERRED (post-fix)**: route in-exception RFE through structured escapes + unify
  exception_jmpbuf + nested-exception guards (the broader original steps 4-7); delete the
  now-dead fiber bridge; cross-title regress (Tomba1/Tomba2/BIOS) per pre-merge gates.
- ⏭ **(superseded sub-items of the original step list)**: outer scheduler loop at the exception/syscall boundary;
  `psx_request_thread_switch` (save current TCB + longjmp YIELD_TO_TCB); route RFE through
  structured escapes; remove all cpu->pc=0 signalling; runnable-thread dispatch pc=0
  fail-fast; tighten psx_is_dispatchable to registered-entry/block-leader; add
  sched_escape_ring + tcb_save_restore_ring; commit pending psx_delay_* C-locals before
  any escape; nested-exception guards (exception_depth / in_int_rp_callback /
  in_tcb_save_restore / critical_depth); unify with the existing exception_jmpbuf (ONE
  escape path). Then validate + cross-title regress.
