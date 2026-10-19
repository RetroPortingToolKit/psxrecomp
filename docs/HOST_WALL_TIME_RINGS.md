# Always-on host wall-time rings (hitch attribution)

A gameplay hitch is an unusually long frame, whether from guest execution, host work, or waiting.
These rings record where the time went, continuously from process start in every build, so a probe never arms
anything: it queries a window that already happened.

| Ring | TCP command | Holds |
|------|-------------|-------|
| frame stamps | `frame_wall_ring {"from":SEQ,"n":N}` | flat `f` array with `stride: 6`: `[guest frame, wall us, interpreter insns, static checks, process page faults, process read KiB]`; counters use their low 32 bits; 131072 frames (~36 min at 60 Hz) |
| spans | `host_span_ring {"from":SEQ,"n":N}` | `[seq, kind, t0_us, dur_us, frame, emu, arg]` for every named blocking-capable operation of 50 us or more, 65536 spans |
| host CPU sampler (debug-tools builds) | `host_profile {"frame_lo":A,"frame_hi":B}` | 1 kHz instruction-pointer samples of the emulation thread, 1 Mi samples (~17 min), with per-module totals for samples outside the exe |

Wall time is microseconds since the first ring call. `emu` is 1 for the emulation thread, 0 for a
worker (compiler child, publication preparer).

Readers and writers use nonblocking slot ownership. A contended sample may be dropped or skipped;
readers only return fully published records from the requested ring generation. A missing frame
stamp leaves an interval spanning multiple frames: do not report that interval as one long frame.
Legacy frame captures had three or four columns and no `stride`; consumers must identify their
layout before interpreting them. Process counters include worker threads, and page faults include
soft faults, so neither counter alone proves a blocking disk read.

Span kinds (`runtime/include/host_span_ring.h`): `autocapture_tick`, `provider_poll`,
`publish_commit`, `loader_rescan`, `dll_register`, `lazy_load` (a synchronous first map of a shard
at a dispatch miss), `capture_on_dma`, `capture_before_dma`, `ac_request` (spawning the compiler),
`ac_done_join`, `ac_prepare_image` (worker), `ac_child` (compiler lifetime, worker),
`overlay_dispatch`, `mod_work`, `frame_present`, and `disc_read` (cooked or raw sector read).

Reading a hitch: per frame, subtract the union of overlapping emulation-thread spans from the wall
delta to find the time no span explains; nested spans must not be counted twice. Then ask the host sampler for that frame window. A title's report tool
joins them (`LegendOfDragoonRecomp/tools/lod_hitch_report.py`).

Findings the rings produced (LoD, 2026-10-12): a toolchain staged from another emitter than the
runtime wrote every shard where the loader never looks (now refused loudly, see
`autocompile_check_toolchain_hash`), and shards compiled during play were mapped by the emulation
thread because `compile_overlays.py` never announced them (now `PSX_SHARD_PUBLISHED`, see
`announce_published_shard`).
