# GTE deadlines and deferred CPU work

Pepsiman qualification10 first differs at return 1,514: source cycle
856709465, native 856709463, one changed RAM page (1FF000). Both repeats
produce identical RAM/CPU/log captures and later stop after 2,288 returns at
a separate GPU scope guard. The original inputs are unchanged.

Passive pre-fetch tracing is qualified against each untraced control. The
first 96,369 PC/clock entries agree. The following entry is PC 8001E4B8:
source 856413994, native 856413995. The preceding SWC2 reads the result of a
DPCS command issued while prior load-wait credits still overlap CPU steps.
Pending GPR-load values and kernel scratch registers are not assumed equal
at this internal representation boundary.

The runtime calculated its GTE stall from psx_cycle_count before publishing
the instruction's pending base cycle. psx_advance_cycles then published that
pending cycle and added the stale stall, counting the cycle twice. The same
omission could arm a GTE command deadline too early or give an MFC2/CFC2 read
the wrong load-overlap credit. The command latency itself is unchanged:
original Octoshock 2.3 GTE_Instruction returns DPCS cost 8 minus 1.

psx_gte_set, psx_gte_stall and psx_gte_read now publish pending batch and local
charges before reading the clock. This is the same ownership rule already
used by the multiply/divide deadline helpers. It applies to callers using
the shared runtime, including interpreted and generated CPU paths.

The BIOS/disc-free test_gte_deferred.c links the actual psx_cycles.c. Its
short DPCS/ADDIU/SWC2 timing case fails before the fix. It then exercises all
64 latency-table entries, prior command overlap, batch/local/both pending
charges, elapsed/deadline boundaries, read credit and unchanged store credit.
75,661 checks pass at O0 and O2. This fixture verifies shared timing helpers;
full game replays and generated/interpretation coverage remain separate gates.

Diagnosis receipts: pepsiman-boundary1514-cpu-comparison.json,
pepsiman-source-boundary1514-11/passivity.json,
pepsiman-native-boundary1514-12/passivity.json and
 gte-deferred-regression-01/{before,O0,O2}.log in the private accuracy campaign.
No frame-specific clock offset, changed input, GTE arithmetic change, observer
reduction or performance tuning is part of this correction.
