/* IRQ policy for nested overlay call units (overlay_unit_defer.h).
 *
 * Regression for the Legend of Dragoon "Checking MEMORY CARD" hang: libcard's
 * MemCardSync busy-waits on a flag set by an IRQ-driven event callback. Called
 * from an interpreted caller into a native shard it ran as an atomic call unit
 * and the overlay CI wrappers skipped every interrupt check for the whole call,
 * so the flag was never set. The default must be to DELIVER inside units; the
 * blanket skip exists only as an explicit A/B knob.
 *
 * Run: `ctest -R overlay_unit_defer_test`. */
#include <assert.h>
#include <stdio.h>

#include "../src/overlay_unit_defer.h"

int main(void)
{
    /* Default policy: never defer, at any nesting depth. */
    assert(OVERLAY_UNIT_IRQ_DEFER_DEFAULT == 0);
    for (int depth = -1; depth <= 8; depth++)
        assert(overlay_unit_irq_must_defer(depth, OVERLAY_UNIT_IRQ_DEFER_DEFAULT) == 0);

    /* Legacy A/B knob: defers exactly while inside a unit. */
    assert(overlay_unit_irq_must_defer(0, 1) == 0);
    assert(overlay_unit_irq_must_defer(1, 1) == 1);
    assert(overlay_unit_irq_must_defer(3, 1) == 1);
    assert(overlay_unit_irq_must_defer(-1, 1) == 0);

    printf("overlay_unit_defer_test: ok\n");
    return 0;
}
