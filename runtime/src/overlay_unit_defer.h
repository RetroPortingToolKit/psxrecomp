/* overlay_unit_defer.h - IRQ policy for nested overlay call units.
 *
 * A callee run as a UNIT (overlay_loader_call_native: the dirty interpreter
 * jal's into an overlay-native shard) used to defer EVERY interrupt check while
 * g_call_unit_depth > 0 (5478d83, an early Ape memcard "partial fix": a
 * cooperative ChangeThread landing inside a nested unit saved an inconsistent
 * (resume_pc, sp) snapshot).
 *
 * That blanket deferral is a liveness hole. A callee that busy-waits on a flag
 * only an interrupt handler can set (libcard MemCardSync polling 0x800BF178,
 * set by the HwCARD event callback; the card protocol itself is SIO-IRQ driven
 * per byte) never sees an IRQ, spins forever natively, and the game hangs on
 * "Checking MEMORY CARD" (Legend of Dragoon Continue / Save). It needs an
 * INTERPRETED caller with a NATIVE callee, so it appears or vanishes with the
 * overlay cache contents: cold cache = both interpreted, fully warm = both
 * native, partially warm = wedge. Bounding the deferral by age does not fix it
 * (a one-frame bound still delivers one IRQ per frame, far below the per-byte
 * SIO rate; ~5k guest cycles is needed) - the deferral itself is the wrong tool.
 *
 * Hardware delivers the IRQ immediately, and so does the interpreter at every
 * entry. What a unit really needs is protection from a cooperative thread
 * SWITCH landing inside it, and psx_check_interrupts already provides that:
 * the "Fix #2" path keys on g_call_unit_depth (at_outermost) and defers the
 * switch to the outermost dispatch boundary. So the default is: deliver.
 * The legacy blanket deferral stays available as an A/B knob
 * (PSX_OVERLAY_UNIT_IRQ_DEFER=1) to bisect any title that regressed with it
 * gone. Header-only so the policy is unit-testable
 * (runtime/tests/test_overlay_unit_defer.c). */
#ifndef OVERLAY_UNIT_DEFER_H
#define OVERLAY_UNIT_DEFER_H

/* Default for the legacy blanket deferral: OFF (deliver IRQs inside units). */
#define OVERLAY_UNIT_IRQ_DEFER_DEFAULT 0

/* 1 = a block-leader interrupt check made at `unit_depth` must be skipped. */
static inline int overlay_unit_irq_must_defer(int unit_depth, int legacy_blanket_defer)
{
    return unit_depth > 0 && legacy_blanket_defer;
}

#endif /* OVERLAY_UNIT_DEFER_H */
