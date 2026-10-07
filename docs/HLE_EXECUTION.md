# Build-selected performance implementations

Use the [shared HLE policy](https://github.com/mstan/recomp-ai-rules/blob/main/HLE.md):
maintain a runnable LLE reference, replace expensive operations behind their real
caller interfaces, and measure the result. Small unnoticeable differences are
allowed. Internal instruction, scratch-state and cycle identity are not universal
HLE requirements. A normal release selects qualified HLE; it does not execute
both implementations or offer live implementation switching.

## Selecting a build

Configure separate output directories with `-DPSX_EXECUTION_PROFILE=ENHANCED`
(default) or `-DPSX_EXECUTION_PROFILE=REFERENCE`. Oracle runtime targets always
select REFERENCE. The setting selects only declared implementation families;
it does not turn arbitrary game functions into HLE or certify their performance.
Existing video mods and resident-loading preferences are independent product
features. Older BIOS runtime controls have not yet been migrated by this helper.

Each runtime writes `<executable>.execution.json`. Startup reports its profile
and identity. Snapshot layout compatibility and the pre-frame network content
gate incorporate the execution identity, including no-mod LAN sessions. Do not
promise cross-profile savestate compatibility or netplay. Normal memory cards
remain game data. Keep generated outputs, overlay caches and packages in their
own build directories; do not overlay a candidate's runtime on another profile.

After copying a candidate executable, package its contract with the shared
stager (use the build's sidecar as `--manifest` if the binary was renamed):

```sh
python tools/release_stage.py stage-execution --binary path/to/staged/Game.exe --manifest path/to/build/Game.execution.json
```

This validates the contract digest, checks that the staged binary embeds that
identity, and records the binary SHA-256 beside it. A mismatched or missing
sidecar stops staging. Use the same command for the executable inside AppDir
before creating an AppImage. It verifies artifact consistency, not gameplay.

## A title implementation

After creating the runtime target, remove the replaced translation unit from
its common source list and register both implementations:

```cmake
psxrecomp_add_implementation(psx-runtime
    NAME packet-builder CONTRACT title-disc-abi-v1
    HLE_SOURCES "${PRIVATE_PACKET_SOURCE}"
    LLE_SOURCES "${ORIGINAL_PACKET_SOURCE}"
    CONTRACT_FILES "${PRIVATE_ADAPTER_HEADER}")
```

The helper links exactly one list. Supply only selected-build headers in
CONTRACT_FILES when the HLE source is private. Missing selected input, an invalid
profile or a duplicate family is an error. A reference build needs no private
HLE files. Source and contract-file hashes identify a family independently of
absolute paths; changed inputs trigger CMake reconfiguration. Include all helper
headers/recipes that affect the contract. Generated bodies remain local inputs.
Runtime targets also hash the shared GTE service contract. The optional PGXP
build sibling receives the same selected implementation families automatically.

The title record names verified disc/code identities, supported entry points,
arguments, required register/memory results, continuation, observable side
effects and completion ordering. Unsupported cases need defined behavior. A
title may retain an original continuation where useful; the framework does not
require dynamic fallback or representation conversion.

## Shared geometry operation

`psx_hle_gte_execute(cpu, command)` is for a native service that owns batch
timing. It retains the same GTE math, precision propagation and projection
capture/replay as `gte_execute`. ENHANCED omits individual command latency;
REFERENCE uses the ordinary timed entry. The service resolves pending work and
charges/publishes the operation's completion at its caller boundary. There is
no mutable timing mode to leak through a nested call or watchdog unwind.

This is an integration primitive, not a standalone FPS improvement. Ape's
packet producer is its first intended consumer. The original `gte_execute`
always retains faithful timing in both profiles.

## Validation

The profile fixture links both implementations of one ABI, checks output and
portable identity, verifies changed selected sources and headers invalidate the
identity, and builds REFERENCE without private HLE inputs. The GTE suites compare
all 22 command families with the reference, exercise projection observers and
prove that a normal GTE call after HLE still arms its own latency.

Title promotion additionally needs isolated HLE/LLE caller-result comparisons,
measured useful gameplay throughput/load time, clean audio, and owner playtest
acceptance. Preserve the LLE floor even when it is slower than real time.
