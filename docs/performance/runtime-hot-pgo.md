# Selective shared-runtime PGO

The optional `tools/runtime_hot_pgo.py` workflow applies GCC profile guidance to
`pgxp.cpp`, `psx_cycles.c` and `memory.c` in one runtime target. These shared
helpers were hot in actual production workloads. It leaves generated game code,
runtime algorithms and execution contracts unchanged. The ordinary non-PGO build
remains the portable default; another compiler needs no profile data or new hook.

The first qualification supports retaining this workflow: near matched-load
adjacent Ape Escape pairs used about **4–5% less process CPU per guest frame**,
repeated with the build order reversed. Tomba's 15-bit movie workload was neutral
within variation. This is per-title training, not a universal pretrained speedup.
No profile blobs, game binaries, BIOS or game content belong in the repository.

## Reproduce the build recipe

Use GCC, Ninja, CMake 3.19+, Python 3.9+, and a dedicated build directory. Complete
the title's normal supported regeneration/dependency setup first. Supply exact
absolute tools/paths and the same production CMake arguments in a JSON file:

```json
{
  "source": "/work/title/generated",
  "build": "/work/title/pgo-build",
  "framework": "/work/psxrecomp",
  "cmake": "/usr/bin/cmake",
  "target": "psx-runtime",
  "binary": "Game",
  "companions": ["Game.execution.json"],
  "configure_args": [
    "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_C_COMPILER=/usr/bin/gcc",
    "-DCMAKE_CXX_COMPILER=/usr/bin/g++",
    "-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja",
    "-DPSX_EXECUTION_PROFILE=ENHANCED"
  ]
}
```

`source` is the directory containing the title's actual generated CMake project;
`binary` and optional `companions` are exact paths relative to `build`. On Windows
include `.exe`. Include the title/dependency paths and qualified feature flags in
`configure_args`; the example does not replace title setup. Do not put the
workflow's controlled `-D` options there.

```sh
python tools/runtime_hot_pgo.py baseline --config recipe.json --jobs 2
python tools/runtime_hot_pgo.py generate --config recipe.json --jobs 2
# Stage and run the generate binary through representative actual production
# work, preserving assets/cache/settings and closing normally to flush profiles.
python tools/runtime_hot_pgo.py use --config recipe.json --jobs 2
```

Each phase retains its binary, named companions, hashes, exact commands and
dependency receipts under `build/runtime-hot-pgo/<target>/`. These are build
artifacts, not complete runnable packages. Use the title's supported asset
stager and `release_stage.py stage-execution` as usual. Keep complete baseline
and use packages separate when measuring, with matching settings/workloads and
normal audio emulation. Instrumented training timing is never a gain sample.

All three phases keep `PSX_DEBUG_TOOLS=OFF`, `PSX_RUNTIME_IPO=OFF` and global
`PSX_PGO` empty. Changing DEBUG changes the selected control flow and needs fresh
training. The driver fixes the hook and exports actual compiler commands. It
rejects stale selected source/header/compiler/workflow inputs, changed normalized
compile commands/configuration, absent/ambiguous/empty profiles and profile
flags on another TU/target. It follows Ninja's actual included headers, not a
repository crawl. GCC also treats missing profiles and coverage mismatches as
errors. Existing receipts/data are preserved; use a new dedicated build tree for
a new attempt. Keep the same build/object paths from generate through use.

The hook targets only the chosen executable, including when shared runtime
sources also appear in PGXP siblings or oracle/test targets. Those targets keep
ordinary flags and do not consume this profile. The driver currently supports
GCC/Ninja only; it does not change existing global `PSX_PGO` behavior. It never
launches a game, infers representative training from file existence, or approves
a title automatically. Record training workload, real progress, orderly close,
source/build identity and per-game production measurements before adoption.

## Initial qualification and limits

The machine-readable [qualification data](runtime-hot-pgo-qualification.json)
retains per-game samples, binary/source identities, host exposure, audio counters
and lifecycle output witnesses. Public paths and the physical hostname use
deterministic aliases; raw receipt SHA-256 values identify the unchanged private
originals. The measured source was clean engine
`3db04d1e8bb9f93e4e9685c4585b259d9331c046` with qualified host pacing and shared
presentation images enabled, native CPU spans off and the LLE GL batcher.
GCC 15.2.0 compiled production DEBUG_OFF binaries. Only these three TUs received:

```text
generate: -fprofile-generate=<data> -fprofile-prefix-path=<build> -fprofile-update=atomic
use:      -fprofile-use=<data> -fprofile-prefix-path=<build> -fprofile-partial-training
          -Werror=missing-profile -Werror=coverage-mismatch
```

Generate alone adds the matching profile link option. Baseline/use have no gcov
counters; three real profiles were consumed per title. The reusable hook keeps
these flags and adds target isolation/validation around the measured recipe.

| Workload | Production result | Interpretation |
| --- | --- | --- |
| Ape Escape, phase-three world, PGXP on, 9×, 16:9, interpolation/rewind off | Two opposite-order blocks; comparable-load adjacent pairs reduced CPU 5.18% and 4.23%; process-cycle counts also decreased | Repeatable bounded CPU improvement; block means (6.6% and 8.1%) are load-confounded |
| Tomba, cold 15-bit MDEC movie, PGXP off, 5× | CPU means 6.399→6.315 ms/frame (1.3%); overlapping ranges | Neutral/inconclusive, no demonstrated regression |

All runs retained approximately 60 guest Hz and matching work/progress. Available
host RAM was 12–17 GiB. Other services/compilers were present; pool reservations
and actual host samples are retained. GPU clocks/load varied. Process cycle
accounting is not a direct physical clock or hardware branch/cache measurement.
There is no isolated GPU, bandwidth, power, high-resolution headroom or memory
saving claim. Smaller binaries and training throughput are not runtime gains.

Audio underruns were zero in the production comparisons, but overflow drops
remain a caveat: Ape's first block had use-only drops 246/264 (baseline zero),
while the reverse block was clean for both; Tomba had baseline-only drops
245/211 (use zero). This does not establish a candidate-caused regression or
clean physical audio. Retain this caveat for the final combined owner check.

The actual production binaries also completed two eight-boundary current-build
snapshot/restore/resimulation windows each. Warm/cold comparisons were clean;
every serialized section matched baseline/use at boundaries 900 and 1060,
including synchronized native VRAM, GPU registers and MDEC output. Ape restore
drift was zero. Tomba retained the same pre-existing auxiliary 1/8 restore drift
in both choices. These bounded checks establish resumed progress and native
state/output, not all gameplay, every high-resolution presentation pixel,
physical controls/audio, or cross-build netplay. Final owner adoption remains
part of the combined review handoff.

## Workflow contract fixture

On Windows, the hook uses native separators for GCC's literal build-prefix
comparison. The driver rejects profile paths at MinGW's 260-character limit
before compiling the instrumented target; choose a shorter dedicated build
directory when needed. Training must actually emit all three profiles.

`tools/tests/test_runtime_hot_pgo.py` is an explicit real GCC/Ninja fixture. Pass
absolute `cmake`, `gcc`, `gxx`, `ninja` executables in `--tools` JSON, a fresh
`--scratch` directory and an output `--receipt`. It builds the normal fallback,
baseline/generate/use and two siblings sharing all selected files in a
subdirectory. It runs actual code to emit all three profiles, compares outputs,
and checks prebuild artifact containment and driver stale/missing/configuration
guards plus GCC's fatal missing
profile/changed-CFG diagnostics. Native GCC configurations with Python and Ninja
register it as `runtime_hot_pgo_contract` in the BIOS-free recompiler CTest tree.
It runs serially with at most two nested compiler jobs. Reserve compiler capacity
for the CTest invocation. Each run retains a fresh work directory and receipt;
`PSX_RUNTIME_HOT_PGO_TEST_SCRATCH_ROOT` selects their parent. Set a short parent
for deep Windows build paths so the existing profile-path guard remains valid.
Retain the receipt and remove the exact disposable scratch tree after review.
