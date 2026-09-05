# Performance campaign tools

These tools begin P0-1 in the [campaign](../PERFORMANCE_CAMPAIGN.md). They collect
diagnostic evidence, not an automatic optimization acceptance verdict. See the
[experiment ledger](EXPERIMENTS.md) for verified local artifacts and limitations.

## Scoped host budget

Use a native Windows Python, not an MSYS path shim. For example, from the
campaign worktree in PowerShell:

```powershell
$python = 'C:\Users\Matthew\AppData\Local\Programs\Python\Python312\python.exe'
& $python tools/perf_host.py --core-percent 50 --affinity 0x1 --timeout 300 --report .local/host.json -- C:\path\ApeEscapeRecomp.exe --renderer opengl --no-launcher --debug-port 4680
```

Prepare isolated writable settings, saves, and memory cards before launch. The
launcher inherits its working directory; run from the isolated game directory
with an absolute script path when that is where the game expects its assets.
Preserve the title's required DLL search path and BIOS/disc configuration.

`--core-percent 50` budgets the entire job at half of one logical CPU equivalent;
`--cpu-percent 50` budgets half of the whole machine. Neither changes guest
clocks. Affinity is a separate control; holding it at one CPU also removes
parallelism. Compare identical affinity across runs, and separately test normal
affinity. Omit both percentage options for unrestricted execution. On Windows,
the child starts suspended, receives the job cap/affinity, then resumes hidden.
Timeout/error cleanup kills only the launched job. Report JSON includes applied
and read-back limits; stdout/stderr are separate adjacent log files.

Calibrate with a sustained CPU-time workload on each host. Short bursts can
escape effective throttling. A cap does not reproduce older GPU, cache, memory,
or power behavior. Keep real low-end machines in the acceptance matrix.

## Diagnostic route capture

Start a compatible Release build with `PSX_DEBUG_TOOLS=ON` and a local debug
port. Use the same effective settings, prepared overlays, BIOS, and isolated
checkpoint across samples. Do not expose the debug server to untrusted hosts.

```powershell
& $python tools/perf_campaign.py sample --port 4680 --route tools/perf_routes/ape_crabby_water_idle.json --restore-slot 1 --state-file .local/crabby-baseline/saves/scph1001/state_800A3660_slot01.pst --timeout 240 --out .local/A1.json --label A1 --provenance .local/target-A.json
```

The committed route references a local v5 state by SHA256; no game data is
distributed. It holds neutral digital input for 7,200 native route ticks. This
is the stationary water-facing workload, not the analog approach to the water.
The runner rejects analog axes because this runtime's native route protocol
does not support them. `--timeout` bounds route polling, not the entire tool
invocation; individual TCP calls, snapshots, and restore have additional time.

`--state-file` hashes an operator-supplied file; it does not upload it or change
the runtime's save directory. Verify that slot 1 actually resolves to that file.
The runner waits for a successful completed restore generation, not just the
staging acknowledgement. Without restore options, starting state is an operator
precondition. Guest execution continues during sequential setup/snapshots, so
this tool does not yet guarantee identical checkpoint-to-route guest work.

Samples retain raw before/after counters, route completion, phase attribution,
and partial failure evidence. Disabled or missing GL timer data is unavailable,
never zero. `summary.route_wall_s` includes route setup/status-poll overhead;
the counter observation window is longer. Neither is exclusive CPU busy time.
Paced route time alone will not reveal spare CPU headroom at full speed.

## Target provenance and comparison

`--provenance` takes a JSON object with these required fields:

| Field | Meaning |
| --- | --- |
| `artifact_sha256` | SHA256 of the actual running executable |
| `settings_sha256` | SHA256 of a retained manifest covering all effective config, overrides, and mods |
| `bios_sha256` | SHA256 of the BIOS input |
| `overlay_inventory_sha256` | SHA256 of a retained, sorted inventory including generated/cache artifact hashes |
| `host_id` | Stable host identity; retain OS, driver, actual GL device, power and display details alongside it |
| `cpu_budget` | `{"unrestricted": true}` or applied `{"cap": 313, "denominator": 10000, "affinity": "0x1"}` |
| `instrumentation` | Nonempty object recording build flags, profiler and diagnostic environment settings |

Record affinity even for unrestricted runs. Optional `variant_selector` names
the faithful/candidate selector. These declarations are operator evidence, not
attestation of the running process. Keep build/source/toolchain manifests and
host launcher reports with them. Samples without target provenance can be
collected but cannot be compared by the A/B command.

Collect at least three nonoverlapping pairs in A1, B1, A2, B2, A3, B3 order:

```powershell
& $python tools/perf_campaign.py ab --a .local/A1.json --b .local/B1.json --a .local/A2.json --b .local/B2.json --a .local/A3.json --b .local/B3.json --out .local/AB.json
```

Declare the primary metric and direction at capture time. Comparison requires
matching routes, metrics, and environments, stable artifacts/selectors within
each side, successful samples, and timestamp evidence of interleaving. It
reports raw values, best values, and medians. `--discard 2:compiler-activity`
removes the entire original second pair and retains its artifact references and
reason; at least three pairs must remain. Never discard merely slow samples.

An accepted *pair* only means comparison eligibility. Correctness checkpoints,
candidate hit witnesses, noise characterization, profiling-off production
timing, stutter/audio measurements, and low-end hardware qualification remain
separate required campaign gates.

## Tests

```powershell
& $python -m unittest tools.tests.test_perf_campaign tools.tests.test_perf_host tools.tests.test_stall_report
```
