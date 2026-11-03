# Primary same-thread RFE register restoration

This is a new authored correction to the
[primary register restore at upstream 7cddab8237e1](https://github.com/RetroPortingToolKit/psxrecomp/blob/7cddab8237e1f73b7067b8401af602d90a96b80d/runtime/src/interrupts.c),
by Alexandros Mandravillis (Alexbeav). A real handler's final jump register is
architectural state; restoring the interrupted snapshot must not overwrite that
actual result on the qualified same-thread RFE return.

The added local captures the original delivery EPC before handler dispatch.
The primary copy omits only GPR26 when mode 1, real RFE, outermost delivery,
stable nonzero TCB and both original-EPC comparisons qualify. It does not
manufacture a register value or alter the return PC. Other restore modes,
legacy/syscall policy, other GPRs, HI/LO and the secondary deferred copy retain
their existing behavior. RestoreState is not separately disqualified: a redirect
that ultimately returns to the original EPC with the same TCB can qualify.

The regression uses the real current interrupts.c and traps.c, the existing
upstream inert link fixture, and a new synthetic handler with arbitrary register
values. It contains no ROM-derived instructions, retail addresses/captures, generated
BIOS/game code, reference-emulator body or passive observer. Source-specific
scheduler/redirect policies, drive configuration and recording infrastructure
are deliberately excluded.

The current base fails 64 qualifying K0 checks. The fixed source is checked in
modes 0 through 3, with real-RFE/syscall/legacy reasons, null/stable/changed TCB,
KSEG aliases, changed targets/global EPC, actual sentinel fallback and arbitrary
live K0 values. The existing restore-edge IRQ regression is also run.
Nested delivery and RestoreState execution are not exercised by these fixtures.
No new retail replay or whole-title accuracy claim is made.
