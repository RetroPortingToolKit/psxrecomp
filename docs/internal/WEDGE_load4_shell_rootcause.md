# RESOLVED: load=4 boot wedge — faithful guest-cycle pad ACK timing

Branch wt/tomba2-load-accuracy. When `PSX_RAM_READ_WAIT_CYCLES=4` (the
Beetle-oracle-accurate single-load value, vs the old DuckStation 6), Tomba 2's
boot deterministically wedged in the BIOS shell. **Root-caused and FIXED
2026-06-27.** This is the faithful-core cascade exactly as CLAUDE.md predicts:
an accurate CPU cost exposed an inaccurate *device* timing.

## Symptom (was reproducible, deterministic)

- pc=0 / halt; `total_checks` frozen (~134,756,056), `current_func` in the BIOS
  shell (0x1FC2CE28 / 0x1FC2C3E0 — the exact landing varies by a few-cycle
  observer-effect, axis-7), `i_stat` 0x80, `i_mask` 0x0D.
- Wedge mechanism (live-verified registers + disasm): a BIOS-shell GUI/object
  builder indexes a 1672-byte-stride table at 0x80110EC8 by a handle `s1`
  (`s1*1672 + base`) with **only a lower-bound (`bgez`) guard, no upper bound**.
  At the wedge `s1 = 0x68 = 104` → `v0 = 0x80110EC8 + 104*1672 = 0x8013B608`, a
  wild pointer; the struct copy corrupts RAM → downstream pc=0. (RAM at the shell
  funcs was zeroed by the time of the halt — post-mortem RAM is unreliable; the
  always-on rings are the ground truth.)

## CONFIRMED root cause (step A — validated, not hypothesised)

The proximate runaway immediately before the wedge is **continuous controller
(pad) polling**, NOT memcard enumeration as previously hypothesised. Live
evidence (`sio_irq_dump` on the wedged runtime): the last 150+ SIO IRQs are
**100% `source=pad`, `delay=4`, `active_device=PAD`, `mc_state=0`** (card idle).

The unfaithfulness (source-confirmed, cross-ref Beetle frontio/sio + axis5 doc
D1/Fix-6): the **pad fast-path** in `sio_write` (`sio.c:1386`) bypassed the
guest-cycle-paced shifter and armed `sio_irq_countdown = SIO_IRQ_DELAY_PAD (=4)`,
which `sio_tick(0)` decrements **once per SIO register access** (sio_tick is only
ever called with cycles=0 — see memory.c:389/498/589, interrupts.c:268). So pad
ACK→IRQ7 timing was **access-count-paced, not guest-cycle-paced**. The card path
(cycle-paced shifter via `sio_advance` ← `psx_advance_cycles`) was already
faithful; only the pad path was not.

Causal chain: with the faithful/faster CPU (load=4) the pad ACK IRQ fires at a
different guest-cycle *phase* relative to the cycle-paced timers/VBLANK than it
did at load=6, diverging the BIOS pad-detection state machine and ultimately
producing the out-of-range handle 104. (The handoff's "memcard save-file
enumeration count" attribution was wrong — it is the SIO **controller** path, a
sub-case of the same axis-5 device-timing weakness.)

## FIX (step B — faithful, validated)

`runtime/src/sio/sio.c`, pad fast-path ACK arm: replaced the access-paced
`sio_irq_countdown = SIO_IRQ_DELAY_PAD` with the **guest-cycle-paced ack
scheduler** the card path uses:

```c
sio_pending_ack        = 1;
sio_ack_remaining      = SIO_BAUD_CYCLES_DEFAULT + SIO_ACK_CYCLES_DEFAULT; /* 1088+170 */
sio_pending_ack_irq_en = 1;
g_sio_timing_active    = 1;
```

`sio_advance()` (driven by `psx_advance_cycles()` per instruction) now delivers
the pad IRQ after a fixed *guest-cycle* delay (one byte-shift + ack pulse =
1258 cyc, identical to the card path), independent of how many SIO register
reads the CPU issues while it waits. Runtime-only change — no regen.

## Validation

- load=4 boots **past the wedge to the intro FMV** (screenshot verified, pixels
  on screen — Rule 5), reaching frame 11,000+ stably (wedge was frame 2189,
  total_checks frozen). The same FMV the load=6 branch reached.
- Ruler #1 (BIOS EvCB-search [c5c→ca4], native 4500 vs Beetle 4382): native 54
  vs Beetle 56, steady −2 = the **known unmodeled load ReadFudge** gap on the
  load=4 branch (a listed remaining Stage-2 component), NOT a regression — the
  SIO change cannot alter CPU instruction cost, and native being *lower* rules
  out a spurious mid-window IRQ.

## Remaining completeness follow-up (recommended, not faithfulness-blocking)

The pad fast-path is now *faithful* but is still a separate code path from the
card shifter. axis5 Fix-6 / the unfinished "1.0e-e2" (comment at sio.c:1868)
specs fully **removing the pad fast-path** so pad bytes flow through the unified
cycle-paced shifter (modeling the byte-shift-in-progress STAT transitions too).
That is a path-unification refactor; it needs pad *input* validation at the
title/menu (not just boot-to-FMV) before adopting. Both pad and card ACK timing
are already guest-cycle-paced, so there is no remaining fake/access-paced site in
the shipping (SIO_MODEL_CYCLE_PACED) config.

Cross-title note: the pad path is shared — regen+smoke Tomba1/MMX6/Ape before any
cross-title merge of this branch (Rule -1: breaking other titles is acceptable;
they regenerate).
