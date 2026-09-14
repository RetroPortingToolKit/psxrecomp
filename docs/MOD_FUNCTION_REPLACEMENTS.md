# Trusted opt-in native function replacements

Enhanced ports can replace a narrowly selected guest function with statically
linked native code, without assigning its additional PC work a simulated PS1
instruction budget. Nothing is registered by default. This is an enhancement
service, not a change to faithful CPU/GTE/DMA timing.

1. List the guest entry in `[recompiler] mod_function_entry_funcs` and regenerate.
2. During activation of an **opt-in** game plugin, call
   `psx_mod_set_function_replacement(address, callback)`.
3. Validate the guest version, inputs, resources and output capacity before
   changing anything. Return zero to execute the original implementation, or
   nonzero only after completing its caller-visible effects.

Existing function-entry preparation callbacks run first. The replacement
dispatcher captures the incoming `$ra` and publishes that address in `cpu->pc`
when the callback succeeds, using the normal CPS return convention. Interior
compiled continuations bypass the fresh-entry hook. Interpreted entry/local
transfer paths also consult the registry, including when a patched prologue
prevents static dispatch.

The callback owns ABI preservation, memory/precision bookkeeping and any
intentional timing effects. The service does not add cycles, accelerate clocks,
or undo a declined callback's side effects: a callback returning zero **must
leave guest/runtime state untouched**. Never use this to skip unrelated game
logic. Calls and registration are emulation-thread-only.

The registry is bounded at 64 exact, nonzero, word-aligned addresses. Replacing
an existing registration does not consume another slot. A null callback removes
it. The default-empty path returns without scanning the table. Registrations
are process configuration, not savestate payload; do not store pointers or
function registrations in guest RAM.

Regeneration changes the generated-code signature. Existing savestates may be
rejected by the normal compatibility guard; retain the older build for them and
use fresh private checkpoints for same-build comparisons. Do not bypass the
guard to make a performance test load.

Tests cover default-off/declined calls, exact addresses, capacity/reuse/removal,
the return ABI (including a callback that clobbers `$ra`), generated opt-in scope
and continuation ordering, and both interpreted callback sites.

Tracking: beads-eio.3.161. The first consumer is Crash's default-off N9 native
normal-terrain transform experiment, tracked in beads-t8j.8.
