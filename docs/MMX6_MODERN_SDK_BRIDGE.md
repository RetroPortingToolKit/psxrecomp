# Local X6 compatibility bridge on the modern SDK

2026-10-27 campaign integration: this bridge is rebased onto `a38bf783` in the
isolated native-services candidate. It preserves master's guarded memory-address
adapters, frozen immediate selectors, host-only feature exclusion, exact-disc
fingerprints, callback guards and current setjmp/longjmp fixture wrappers.
The combined overlay layout is ABI **35**, appending the replacement callback
after master's ABI34 fields. All game/BIOS/overlay inputs must regenerate.
Newer PCM, override, disc-request, live-option, memory-address and host-output
callbacks use the same explicit ownership restoration. The original validation
below is historical evidence; combined source/build/game qualification is
tracked in `beads-9zi2` and optimization campaign `beads-fobc`.

The combined GCC15.2 build now passes all18 targeted runtime cases, including
the ordinary scheduler-escape fixture, missing-profile/image checks, loader
negotiation, modern settings profiles, own-view isolation, resident loading and
both PGXP compilation flavors. Replacement codegen, compiled-overlay ABI35
forwarding and interpreted-entry guard checks also pass. Fixture-only duplicate
definitions/missing link inputs were repaired, and netplay assertions stay active
in Release. This supersedes the historical fixture failures below for this
combined source; built-game and two-peer qualification remain separate.

This branch bridges X6's native replacement and negotiated loading services to
modern SDK `6f0ccbc6b67fee626de6a7b0945ca3abb26332aa`. It is a local integration
candidate, not a qualified game repin. No games or memory cards were launched.

The base retains `recomp-net` at
`e53059306824edf4f0aec4aae5ae65151b18535f` and `retcomm-rbengine` at
`2a03e73693acee0fb78076ea058642c931bef12e`. Neither gitlink changes. Modern
snapshot providers/readiness, core-plus-mod digest callbacks, renderer policies,
settings-only profile negotiation, and explicit virtual host-device sampling
from `1e33761d` remain present.

## Ported behavior

- Port the bounded, default-empty native replacement service from `a6216c6be`.
  Fresh generated entries, dirty-RAM entries/transfers and overlay forwarding
  consult it after existing function-entry callbacks. Successful replacement
  publishes the incoming return address. Registrations reset with the mod plan.
- Port the loader-only policy from `50c68be75`, using modern content identity
  rather than replacing modern settings negotiation. Only the profile's declared
  loader feature can enter its negotiated package plan. Unrelated offline
  features are disabled in the temporary plan; user preferences are preserved.
- Carry verified-image handling from `83e8b5e11c005e0868e1aef96335258aad157559`.
  Offers and commits resolve against the actual selected image, including before
  an offline commit. Same-path image replacement forces remeasurement. Unknown,
  missing and wrong-revision images cannot reuse an earlier identity.
- Use overlay **ABI32**. Old X6 ABI31 uses the older `advance_cycles` callback
  semantics; the modern branch uses `cpu_charge`. Even where member offsets
  coincide, reusing ABI31 would admit incompatible cached modules. Both old
  ABI30 and ABI31 modules must be rejected and regenerated.
- Include live replacement depth in the callback capture guard and the saved
  scheduler/render callback context. Replacement pointers are not snapshot data.

Loader profiles append three fields to the modern `PSXModNetplayProfile`:
`loading_package_id`, `loading_feature_id`, and `loading_plugin_id`. A profile
must supply all three or none. Simultaneous settings-plus-loader profiles are
explicitly rejected: a combined offer codec is outside this bridge. Existing
modern settings-only profile identity remains unchanged (the fixed SHA fixture
still passes). Loader identity additionally binds its verified plan and flag.

## Game source adaptation required

Do not reuse X6's old positional profile initializer. The modern descriptor has
`state_digest`, `requires_retained_texture_banks` and settings/renderer fields
before the appended loading triple. Populate named fields (or an explicit zeroed
descriptor), set the loading triple and retain a new build-specific compatibility
ID. X6's enhanced renderer should request retained texture banks; this enforces
the modern GL admission policy before boot.

Register an executable-owned snapshot provider under the forced simulation
plugin ID, with explicit wire encoding, validation and readiness. Set
`state_digest` to a canonical authoritative host-state digest. These APIs exist
in the base; the bridge does not implement X6's state provider or justify enabling
rollback, savestates or rewind in its profile. Keep those policies disabled until
the title's current-build restore behavior is validated. Rebuild/regenerate all
affected code and discard older overlay/savestate artifacts.

`ModFunctionEntryContext` also appends `replacement_depth`; all native users of
that host-only structure must rebuild. This context never belongs in guest save
payloads. Memory-card/native progress is a separate contract and is untouched.

## Callback escape correction and remaining blockers

The modern baseline's `PluginCallbackScope` has a nontrivial destructor. Its
function-entry callback can escape through scheduler `longjmp`, which is
undefined in C++ when it skips such a destructor. The bridge replaces all five
scope sites (entry, guest function, instruction, VBlank, activation) with trivial
saved context and explicit restoration on ordinary return and C++ exceptions.
Scheduler landing still restores an abandoned context. Original escape/nested
fixtures remain active, and a throwing callback regression checks ownership and
capture-gate restoration before propagation. No timing RAII remains in the four
gameplay dispatchers or the registry invocation wrappers.

Activation still has host launch timing scopes. Its public contract now explicitly
forbids entering guest execution that can scheduler/watchdog-escape. This change
does not qualify arbitrary game-owned C++ stacks for `longjmp`.

GCC 15.2 (`-O2 -UNDEBUG`, Windows GNU x64) still crashes at the original mode5
scheduler escape with `0xC0000005`, including after the POD correction. An exact
baseline6f control, rebuilt from baseline runtime/profile/test sources and the
complete baseline header set, crashes at the same marker. The control reused
unchanged ancillary objects and C codec libraries. A minimal POD `setjmp`/
`longjmp` control passes; this is not evidence that GCC longjmp always fails.
The cause of the remaining full-fixture GCC failure is unresolved.

Clang 22.1.8 (`ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`, Windows GNU x64)
recompiled every fixture C/C++ translation unit, reusing only unchanged C codec
libraries. The normal full fixture, missing-renderer, profile-options,
option-renderer-missing and native-loading modes pass. The normal run includes
the original mode5/mode6 escapes and the new C++ exception regression.
The missing-profile mode fails an existing file-identity assertion after an
in-place same-size rewrite and restored mtime on NTFS. Other repeated fixture
runs also sometimes fail that assertion. Preserve this unresolved cache-identity
risk; do not represent the whole matrix as green.

## Validation retained on G:

All generated files, private synthetic-disc fixtures, temp and cache paths are
under `G:/Temp/Codex/psxrecomp/x6-parity`; dependencies came from local sources.
No source disc or cards are used. Builds ran with assertions enabled and at most
two compiler jobs.

- GCC focused checks pass: mod snapshot, snapshot boot state, mod core digest/
  content identity, wide/optional-bank/settings/loading policies, savestate title
  policy, rewind context, texture banks, native replacement service, and the
  independent `mod_native_loader_bridge_test` (12 checks).
- The loader fixture includes a default-enabled unrelated offline co-op plugin,
  declared loader admission, host enable/disable, activation, preference
  preservation, SHA/session identity separation, unknown/missing/wrong/same-path
  image rejection, and recovery using the correct image.
- `mod_function_replacement_codegen_test`, interpreted entry guards and compiled
  overlay callback forwarding/ABI32 assertions pass.
- PGXP session (both compilation flavors), netplay view mods and resident
  resources also pass (four more GCC checks). All targets using
  `mod_runtime.cpp` link, including the cache variant; its broad execution remains subject to the
  unresolved scheduler failure. Two existing test wiring omissions were fixed:
  the PGXP hooks session now receives the same empty mod-memory stub as its
  sibling, and the offline resident fixture supplies `psx_netplay_active = 0`.
- The changed frontend, dirty-RAM interpreter, overlay loader and mod runtime
  translation units compile with the configured modern UI. This is not a linked
  game or runtime-playback qualification.

Local evidence directories: `sdk-bridge-build` (GCC CTest/build outputs),
`sdk-bridge-codegen-build`, `sdk-baseline-control/receipt.json`,
`sdk-baseline-control/minimal-longjmp.json`,
`sdk-baseline-control/instrumented/bridge-pod-runtime.json`, and
`sdk-bridge-clang-control/{receipt,correct-profile-modes}.json`. Earlier failed
build/test logs are retained beside them. The independent loader test does not
bypass or imply a pass for the broad fixture.

Integration still requires an actual combined X6 build, current-build state
round trips, rewind/input ownership checks, and a fresh two-peer default-loading
qualification. This bridge makes those experiments possible; it supplies no
runtime gameplay, cross-build or cross-platform determinism claim.
