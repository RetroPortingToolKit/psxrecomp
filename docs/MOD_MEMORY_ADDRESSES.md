# Guarded memory address adapters

A trusted plugin can redirect the effective address of an audited guest memory
instruction with `psx_mod_register_memory_address_plugin(id, pc, expected,
callback)`. Add the PC to `[recompiler] mod_instruction_sites` and regenerate
static code. The callback receives a read-only CPU, PC, original instruction,
and computed effective address; return a replacement address or the input to
decline. No title-specific addresses or data layouts belong in this API.

The active mod plan owns availability. Both the original instruction passed by
the backend and the live RAM word must equal the registered word. This prevents
an unrelated overlay at the same PC from being redirected. Registrations support
LB/LBU/LH/LHU/LW/LWL/LWR, SB/SH/SW/SWL/SWR, and LWC2/SWC2. PC segment aliases
share registrations. Duplicate plugin/PC/word address adapters are rejected.
Different overlay words at one PC are allowed.

Adapters execute in plan order. Each receives the address returned by the
previous adapter. They cannot re-enter guest execution or change guest state.
The low two address bits must stay unchanged; a violating result is ignored
and counted as `mod.memory_address.bad_alignment`. This preserves alignment
faults and byte lanes for unaligned word merges.

Static code computes the address once before the original load/store, keeping
the same value for PGXP. Source registers, store values, destinations, GTE
access, load interlocks, cycle helpers, and pending branch behavior remain in
the normal instruction implementation. Even a load whose destination aliases
its base can therefore be redirected without corrupting the base calculation.
Immediate variants compute their selected offset while guarding the original
instruction. An unconfigured native instruction has no new callback cost.

Dirty-RAM interpretation passes the original fetched word through the decoder
and redirects the address in each memory-op case. Overlay DLLs forward through
`OverlayCallbacks.mod_memory_address`, flushing pending cycle charges just as
other mod callbacks do. ABI 32 rejects stale DLLs. The normal codegen cache
fingerprint also changes with the emitter. Package deactivation and netplay
plan clearing remove adapters with the existing instruction-hook table.

Adapters are for access sites whose data ownership has been proved. A numerical
address resembling an extended array index is not sufficient: title callbacks
must validate their overlay/table/index context. Loop bounds, stack-sized table
copies, UI capacities, saves, and asset identities need separate title work.

Validation extends the mod runtime tests (activation, aliases, instruction/live
RAM guards, CPU preservation, invalid alignment, plan clearing), generator tests
(all memory opcodes, PGXP, branch delay slots, immediate variants, load-to-zero),
and the interpreted-instruction fixture (all memory opcodes and unaligned lanes,
negative offsets, base/destination alias, GTE, and PGXP address parity).
