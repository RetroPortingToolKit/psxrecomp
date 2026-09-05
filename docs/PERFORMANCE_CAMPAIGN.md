# OpenGL performance campaign

Tracking: central Beads `beads-jod2`, beneath `beads-eio.3` (psxrecomp framework).
Branch: `perf/opengl-burndown-20260904`.
Starting revision: `bf61b1054313e1c80d5c2bf1b14a1e50f2e54b8a`.
This is the audited integration revision, not a claim about a shipped release.

## Objective and constraints

Improve sustained guest speed, frame consistency, and resource use at identical
settings and correctness. OpenGL is the primary renderer. Vulkan completeness
is a separate qualification exercise, never a substitute for an OpenGL fix.
Use GPT-5.5 agents for bounded implementation and independent adversarial review
of their code, measurements, and acceptance claims.

The primary route is Ape Escape, Crabby Beach, approaching and facing the water.
Inspect existing `ApeEscapeRecomp/saves/scph1001` states before choosing one;
preserve originals and record BIOS, state hash, scene, and input sequence.
Add quiet gameplay, facing away, and a transition, plus Tomba 2 and Mega Man X6
regression routes. Compare guest speed, game-produced frames, and host presents
separately to avoid treating original-game slowdown as host overload.

Test unrestricted, 50%, and 25% host CPU budgets, with affinity recorded as a
separate variable. Never change guest clocks to simulate hardware. CPU throttling
does not emulate older caches, memory bandwidth, or integrated graphics. Real
acceptance targets are an older 2-core/4-thread integrated-graphics PC and a
Deck-class handheld, with desktop and supported-platform regression coverage.

## Work packages

This table defines the approved work, not its live issue status. Beads owns
status and dependencies; experiment results belong in the evidence ledger.

| ID | Work and candidate experiments | Required evidence |
| --- | --- | --- |
| P0-1 | Repeatable route runner, artifact/config identity, machine-readable A/B reports | Same guest work, reproducible baseline, measured noise |
| P0-2 | Release flags, effective settings, overlay inventory, cache rejection and compiler failures | Clean-install Windows/Linux behavior without development caches |
| P1-1 | Unused audio/GPU histories, memory observers, tracing/logging, diagnostic allocations | Measured saved work; retain functional observers and cheap health counters |
| P1-2 | GL batch breaks, texture/CLUT dependencies, packing, readbacks, uploads, buffer streaming | Identical order, masks, blending, texture feedback, VRAM and enhanced output |
| P1-3 | Swap blocking, pacer ownership, spinning, refresh/focus, interpolation and audio delivery | No speed drift, latency regression or new underruns |
| P1-4 | Overlay discovery, validation, loading, fallback, compilation contention and cache growth | Correct identity across replacement, self-modification, restore and cold installs |
| P2-1 | Generated code, dispatch, continuations, state traffic, interpreter, lookup/linking | Exceptions, delay slots, resumes and invalidation across all execution tiers |
| P2-2 | RAM/MMIO, aliases, observers, cycle publication, deadlines and idle detection | Exact cycle charges, side effects and event ordering |
| P2-3 | GTE/PGXP state copies, inactive hooks and shadow validation | Exact GTE state and unchanged enabled enhancements |
| P2-4 | SPU/resampling, CD/XA, MDEC, DMA, disc/decompression/card I/O; SIMD/batching/prefetch | Audio, IRQs, backpressure, decode output and completion timing |
| P2-5 | Allocation, bandwidth, locality, texture caches, mods/UI, rewind, netplay | Bounded memory, enabled-feature parity and inexpensive inactive paths |
| P2-6 | PGO/LTO, compiler options, code layout, portable SIMD and build costs | Multi-title gains, arithmetic parity and portable release artifacts |
| P3-1 | Threads, helper processes and GPU compute for attributed hotspots | End-to-end constrained-host gains including all transfer/sync costs |
| P3-2 | Software renderer hotspots and separate Vulkan qualification | Explicit coverage gaps; OpenGL acceptance remains mandatory |
| P4-1 | Release integration and recurring performance gates | Clean-install route, correctness and resource checks on shipped artifacts |

Complete broad attribution before concentrating implementation on the largest
cost. Every area receives a measured disposition, including already optimized,
rejected, or deferred with a concrete missing prerequisite. Do not infer impact
from source size, suggestive names, or another console's bottlenecks.

## Measurement and acceptance

Follow [HOST_OPTIMIZATION_CONTRACT.md](HOST_OPTIMIZATION_CONTRACT.md).
Use at least three interleaved A/B pairs with all raw results, minima, medians,
discard reasons, a preregistered primary metric, and the 5% retention gate for
added complexity. Use forced faithful/candidate selectors where practical.

Separate warm-cache throughput (fixed guest work; no compiler activity) from
first-use and transition campaigns where compilation/loading is the subject.
The latter must be explicitly labeled and must not be represented as satisfying
the quiet-host warm-cache gate. Use 120-second paced windows for stutter.
Record source and artifact hashes, generated code, BIOS, toolchain, effective
settings, overlay contents, power state, pacing owner and instrumentation.

Attribute CPU samples and thread waits, GL scene/present time, readback/upload
bytes, execution tiers, device work, audio underruns, memory, allocations, page
faults and I/O. Expensive profiling is for attribution; final timing must also
run without it. Missing or reset counters must never become zero-cost evidence.

Compare deterministic guest state at intermediate checkpoints, including cycles,
deadlines, pending exceptions, device state, RAM/VRAM/SPU RAM and overlay identity.
Add rendered-output/audio comparisons and candidate hit witnesses. Exercise
invalidation, aliases, exceptions, DMA, save/load, rewind and netplay as affected.
Do not claim parity from a fallback that silently avoids the candidate.

For offload, specify ownership, immutable inputs, publication deadlines, bounded
queues, backpressure, cancellation and restore before coding. Compare inline
against one worker first. No races on guest memory, altered device ordering, or
implicit cross-thread GL context use. Count IPC, copying, driver contention,
memory and synchronization in the gain, especially on low-core-count/iGPU hosts.

## Initial evidence and completion

Source inspection found always-on audio PCM history and GP0 history, potential
GL full-VRAM readback, and multiple VRAM copies. These are hypotheses, not
measured causes of Crabby Beach slowdown. Widescreen overhang detection is
functional; its census already has a gate. Vulkan already has a staging cache.
Cycle batching, event deadlines, sparse probes, MDEC SIMD and GPU-direct GL
presentation already exist and must not be proposed as new work without audit.

Append accepted, rejected and incomplete experiments to an evidence ledger with
raw-data links. Keep issue status in central Beads and record landed commits.
Do not close the campaign until every work package has a disposition and accepted
improvements reach validated title releases. Target sustained real-time execution
at preserved settings on the selected hardware, without new correctness, audio,
or pacing failures. Record hardware limitations rather than hiding them.
