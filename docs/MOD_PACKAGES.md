# PSXRecomp mod packages and features

A `.psxmod` is a versioned installation, provenance, and trust boundary. A
package may contribute any number of independently configurable **features**.
The launcher presents those features as the primary Mods list; package
installation, version selection, and removal are a secondary management view.

Feature identity is always `(package_id, feature_id)`. Enabling one feature
never enables, disables, or reconfigures another feature.

The player selects a verified stock BIN/CUE. Resolution produces guarded native
operations and sparse disc overlays without rewriting or replacing that stock
image.

## Where packages live

Two catalog roots sit beside the executable, split by who owns the files:

```text
<exe>/mods/
  bundled/     build output — the framework's mods/builtin/packages (minus any
               the title declines with EXCLUDE_BUILTIN_MODS) plus the title's
               mods/preloaded/packages. Every build WIPES and re-stages this
               tree, so nothing a player owns may live here.
  installed/   launcher-owned — .psxmod archives installed through the Mods
               manager. No build ever touches this tree.
  state.toml   user selection state (enabled features, option values).
```

Both roots use the same `<package-id>/<version>/manifest.toml` layout and are
scanned into one catalog, bundled first. An installed package with the same id
as a bundled one deliberately shadows it and records that it did so, so an
override is visible rather than decided by directory-iteration order.

A bundled package is not removable from the Mods page: deleting build output
would succeed and then be undone by the next build. Remove it from the title's
`mods/preloaded/packages` instead.

Only `bundled/` ships in a release. `installed/` and `state.toml` are the local
machine's and are excluded by every packager.

**Migration.** A pre-split install has one `mods/packages/` tree holding both.
The first scan moves each package the current build did not also stage into
`installed/`, drops the rest as redundant build output, and removes the old
tree — but only once every version directory has been dealt with. Anything that
could not be moved is left exactly where it is and reported.

A manifest that fails to parse is never skipped silently: it is reported by
`scan_errors()` and logged, naming the path and the reason.

## How `bundled/` gets staged (titles: read this)

**The framework owns the layout. A title declares a directory, never a path
shape.** Hand the title's catalog to `psxrecomp_add_runtime_target()`:

```cmake
set(MYGAME_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")

psxrecomp_add_runtime_target(psx-runtime
    ...
    PRELOADED_MODS_DIR "${MYGAME_PRELOADED_MODS}"
)
```

`PRELOADED_MODS_DIR` names a directory shaped like
`<dir>/packages/<package-id>/<version>/manifest.toml`, optionally with a
`README.md` beside `packages/`. On every build of that target the framework:

1. wipes `<exe-dir>/mods/bundled` (build output only — never `installed/` or
   `state.toml`),
2. removes exactly the ids it is about to stage from any pre-existing
   `mods/packages`, which migrates a build directory made before the split
   while leaving a player's own legacy packages for `migrate_legacy_root()`,
3. copies the framework's `mods/builtin/packages/<id>` then the title's
   `<dir>/packages/<id>` into `mods/bundled/<id>` — in that order, so a title
   may deliberately OVERRIDE a builtin at the same id and version (Tomba 2's
   Italian runtime ships localized `psx.*` manifests exactly this way),
4. copies `<dir>/README.md` to `mods/README.md`, and
5. verifies the result with `runtime/psx_check_mod_catalog.cmake`.

Pass `PRELOADED_MODS_DIR NONE` to declare that a target intentionally ships no
game catalog. A target built with `COSIM` stages nothing: it has no launcher,
and it shares an output directory with the real runtime.

### Declining a framework builtin (`EXCLUDE_BUILTIN_MODS`)

Every package under the framework's `mods/builtin/packages` targets
`game_id = "*"`, so by default every title ships all of them. A title that does
not want one — because it ships its own replacement, or because the feature is
wrong for it — names it:

```cmake
psxrecomp_add_game_runtime(psx-runtime
    ...
    PRELOADED_MODS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded"
    # WipEout 3's own team-mark bezel replaces the generic file-picker one,
    # and host-paced fast loading is not offered for it.
    EXCLUDE_BUILTIN_MODS
        psx.enhancement.fast-loading
        psx.presentation.bezel
)
```

`EXCLUDE_BUILTIN_MODS` is accepted by both `psxrecomp_add_game_runtime()` and
`psxrecomp_add_runtime_target()` (the PGXP clone inherits it). An excluded
package is **absent, not hidden**, the same rule as the developer channel:

- it is never copied into `<exe-dir>/mods/bundled`, so the launcher cannot list
  it and every release packager — which all ship the build's staged tree —
  cannot ship it either;
- the build publishes the exclusions beside the catalog manifest as
  `psx_mod_catalog_<target>.excluded.txt`. The build-time guard fails if an
  excluded id turns up in `mods/bundled` anyway, and `tools/release_stage.py
  stage-mods` both refuses such a catalog and stops counting an excluded
  builtin as *missing* when a caller passes the framework's `mods/builtin` as a
  `--mod-source`;
- the framework itself is unchanged: the package, its plugin and its C API stay
  available to every other title.

Configure fails loudly when an excluded id is not a framework builtin (a typo
would otherwise leave the package shipping while the CMakeLists reads as if it
did not), and when the title's own catalog provides the same id — that is an
override, and excluding and overriding one id at once contradicts itself. The
selection lives in `runtime/psx_mod_catalog_select.cmake`; both it and the guard
are exercised by `runtime/tests/test_mod_catalog_layout.py`.

A player's `mods/state.toml` written by an earlier build may still name an
excluded package. That selection is dormant, not an error: see
[State and migration](#state-and-migration).

**Do NOT write your own `copy_directory` into `<exe-dir>/mods`.** Five titles
did, and the reason it is now a build error is worth stating: a hand-written
copy names the destination as a *string*, so when framework commit `4cc04be3`
renamed the staged catalog from `mods/packages` to `mods/bundled`, all five
kept staging into a directory nothing reads. Nothing failed to configure,
compile or link — the coupling has no compile-time or link-time consumer — and
the defect surfaced only when a release packager ran, in a different
repository, on a later day. Two guards now close that window:

* **configure time** — a project with packages under
  `mods/preloaded/packages` that does not declare `PRELOADED_MODS_DIR` is a
  `FATAL_ERROR`, naming the packages and the argument to add.
* **build time** — `psx_check_mod_catalog.cmake` runs as the *last* `POST_BUILD`
  step (registered through `cmake_language(DEFER)`, so it lands after anything
  the title registered) and fails the build if a declared package did not reach
  `mods/bundled`, or if any package this build stages turned up under
  `mods/packages`. It is also registered as the ctest
  `psx_staged_mod_catalog_test`.

Both are exercised by `runtime/tests/test_mod_catalog_layout.py`.

The release packagers assert the same invariant one layer out:
`Add-ModCatalog` in `tools/release_overlay_stage.ps1` reads `mods/bundled` and
refuses to package when a package the *sources* define is missing from it. It
asserts that invariant rather than a hard-coded count, because a count
describes only one side of a catalog two repositories contribute to and goes
stale the moment either side gains a mod.

## Feature manifest

Write new manifests at the current format version, which is **7**. Older
versions stay readable so installed packages survive an update, and each
section below notes the version a field first required.

```toml
format_version = 7
id = "example.localization"
version = "1.2.0"
name = "Example Localization Pack"
author = "Example Author"
description = "Independent title and script features."
license = "MIT"
resolver = "declarative"

[[target]]
game_id = "SLUS-00000"
# Required for disc overlays. Use the digest of the supported stock image.
disc_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

[[feature]]
id = "title-screen"
name = "Title Screen"
description = "Selects the title-screen artwork."
group = "Localization"
default_enabled = false

[[feature]]
id = "retranslation"
name = "Retranslation"
description = "Uses the revised English script."
group = "Localization"

[[option]]
feature = "title-screen"
id = "variant"
label = "Title artwork"
type = "choice"
default = "rockman"

[[option.choice]]
value = "mega-man"
label = "Mega Man X6 (USA)"

[[option.choice]]
value = "rockman"
label = "Rockman X6 (Japan)"

[[patch]]
feature = "title-screen"
target = "main_exe"
address = 0x80041234
expected = "2a 00 02 24"
replace = "0e 00 02 24"
when = { variant = "rockman" }

[[overlay]]
feature = "retranslation"
target = "disc_raw"
offset = 123456
file = "assets/retranslated-script.bin"
sha256 = "..."
# Optional additional guard over the same range in the stock image.
expected_sha256 = "..."
```

Every `[[option]]`, `[[patch]]`, and `[[overlay]]` in a feature-style manifest
must name its owning feature. Ambiguous operations are rejected.

Option types are `boolean`, `choice`, and bounded `integer`. Conditions are
feature-local: `when = { option = "value", ... }` requires every listed option
to match. The legacy `when_option`/`when_value` pair remains accepted for a
single condition.

A feature with `hidden = true` is left out of the launcher's lists while it is
disabled; while enabled it is listed so the player can turn it off. A title that
sets `[runtime] hide_hidden_mod_features = true` in `game.toml` never shows it:
the launcher does not list it (enabled or not), does not name it in the lobby
summary, "Enable all" / "Disable all" leave it alone, and a package whose every
feature is hidden is not listed under "Installed packages". Either way it runs
exactly as `default_enabled` and `mods/state.toml` say, so a hidden default-on
feature is simply active. (recomp-ui `launcher_mod_visibility.h`.)

## Bounded integer patches

Package format 2 can encode a bounded integer option directly into a guarded
write:

```toml
format_version = 2

[[feature]]
id = "starting-lives"
name = "Starting Lives"

[[option]]
feature = "starting-lives"
id = "count"
label = "Lives"
type = "integer"
min = 0
max = 99
step = 1
default = 2

[[patch]]
feature = "starting-lives"
target = "main_exe"
address = 0x8001DE64
expected = "02 00 02 24"
replace_from = { option = "count", encoding = "u16le", offset = 0 }

[[patch]]
feature = "starting-lives"
target = "main_exe"
address = 0x8001DE70
expected = "03 00 02 24"
replace_from = { option = "count", encoding = "u16le", offset = 0, addend = 1 }
```

`replace_from` and literal `replace` are mutually exclusive. The referenced
option must be a bounded integer owned by the same feature. The initial
encodings are `u8`, `u16le`, and `u32le`. `offset` selects a byte field inside
the expected guard and defaults to zero. Generated replacement bytes begin as
an exact copy of the expected bytes, then the encoded value replaces only that
field. This lets a MIPS immediate, for example, retain a guard and collision
claim over its complete instruction. `addend` is the only supported transform,
and the complete declared option range after that addend must fit the unsigned
encoding.

There is deliberately no host-endian encoding, signed inference, mask, shift,
scale, expression language, or partial-field merge. A package uses multiple
guarded `[[patch]]` entries when the same value has multiple destinations.
Generated bytes enter the ordinary pre-boot write plan, collision checks, and
fingerprint. A generated value identical to the stock guard is omitted as a
no-op, so an enabled stock-valued option does not claim or conflict on bytes it
does not change.

Integer values use canonical decimal text. Leading plus signs, redundant
leading zeroes, values outside the bounds, and values not aligned to `step` are
rejected.

## Ordered values and split MIPS immediates

Package format 3 adds feature-local ordering constraints for related integer
fields:

```toml
format_version = 3

[[constraint]]
feature = "rank-thresholds"
kind = "ordered_integer"
direction = "nondecreasing"
options = ["rank-c", "rank-b", "rank-a"]
```

All listed options must be integer options on that feature. Defaults must
satisfy the constraint. While a feature is enabled, an edit that would invert
the order is rejected with the neighboring option labels. Disabled features
may retain an incomplete or invalid draft, but cannot be enabled until it is
valid. `nonincreasing` is also supported.

Format 3 also provides a narrow, typed transform for constants constructed by
a linked MIPS `LUI`/`ORI` pair:

```toml
replace_from = {
  option = "speed",
  encoding = "mips_lui_ori_u32",
  omit_when_default = true
}
```

The patch must target one aligned, fully guarded eight-byte `main_exe`
instruction pair. The loader verifies the opcodes and register linkage, then
places the raw high and low 16-bit halves into the two immediates. It does not
apply signed-`ADDIU` carry adjustment. `offset` and `addend` are not accepted
for this encoding.

`omit_when_default` suppresses the entire patch when the selected value equals
the option default. This models source tools whose declared default means
"make no writes," including cases where multiple guarded sites contain
different stock values. For any nondefault selection, every declared site
retains its collision claim even if one generated replacement happens to equal
its stock guard.

## Sparse fields and integer predicates

Package format 4 separates a patch's complete expected-byte guard from the
fields it owns and writes. This is for semantic records whose independently
configurable fields share one useful guard:

```toml
format_version = 4

[[option]]
feature = "saber-timing"
id = "frames"
label = "Frames"
type = "integer"
min = 0
max = 99
default = 2

# Positive values change only byte 0.
[[patch]]
feature = "saber-timing"
target = "main_exe"
address = 0x80077640
expected = "02 42 01 02"
fields = [
  { offset = 0, option = "frames", encoding = "u8" },
]
when_integer = { option = "frames", op = "gt", value = 0 }

# Zero has a narrow, explicitly declared compound representation.
[[patch]]
feature = "saber-timing"
target = "main_exe"
address = 0x80077640
expected = "02 42 01 02"
fields = [
  { offset = 0, replace = "01" },
  { offset = 2, replace = "00" },
]
when_integer = { option = "frames", op = "eq", value = 0 }
```

`fields` is mutually exclusive with `replace` and `replace_from`. Every field
has a nonnegative `offset` and exactly one payload form:

- `replace = "..."` supplies non-empty literal bytes; or
- `option` plus `encoding` uses a bounded integer option on the same feature,
  with the optional checked `addend`.

Fields must fit inside `expected` and their owned byte ranges may not overlap.
The supported dynamic encodings are the existing `u8`, `u16le`, `u32le`, and
linked `mips_lui_ori_u32` forms. A sparse MIPS field owns only the two
immediate halfwords while the complete linked instruction pair remains
guarded; as with format 3, that encoding does not accept an addend.

Resolution omits individual fields whose generated payload already equals the
guard. If no field changes, the patch is a no-op. Otherwise the plan retains
the complete guard and the exact remaining owned ranges. Runtime guard
validation checks every byte of `expected` before applying any writes, while
collision detection and writing use only the owned fields. Thus two features
can safely own adjacent bytes in one record without either overwriting the
other. Overlapping guards must still agree on their expected bytes; conflicting
guards make the plan unsatisfiable and are rejected.

`when_integer` is a patch-level, feature-local predicate over one bounded
integer option. `op` is exactly one of `eq`, `ne`, `lt`, `le`, `gt`, or `ge`,
and `value` is an integer constant inside the option bounds. Equality
constants must also be selectable under the option's `step`. String-valued
`when` conditions and one `when_integer` predicate may coexist and are ANDed.

Sparse fields and integer predicates are still pre-boot plan construction.
They do not provide a general expression evaluator, masks, arithmetic beyond
the checked field addend, package code execution, or per-frame dispatch.

## Channels

A feature declares how finished it is. Format 6 puts `channel` on the
**feature**, not the package:

```toml
format_version = 6
id = "example.enhancements"

[[feature]]
id = "widescreen"
channel = "experimental"     # ships, badged, default off

[[feature]]
id = "hook-trace"
channel = "developer"        # absent from any release build
```

| Channel | Ships | In the launcher |
|---|---|---|
| `stable` (default) | yes | no tag — the absence is the stable case |
| `experimental` | yes | amber `EXP` tag, and a line saying it is unvalidated |
| `developer` | **no** | secondary-accent `DEV` tag; only ever visible on a local build |

An absent `channel` means `stable`. A package may still declare one, which its
features inherit unless they state their own — which is how a format-5 manifest
carrying a package-level `channel` keeps working unchanged.

**Why the feature and not the package.** A package is the installation and
**trust** boundary; how finished one of its features is has nothing to do with
that. When the marker sat on the package, a catalog holding one player-ready
feature and one developer instrument had to declare itself entirely
developer — so neither shipped, and the documented workaround was to split the
catalog into two packages. Channels per feature remove that trade.

**"Developer does not ship" means absent, not hidden.** Two mechanisms, one per
catalog root:

- **`bundled/` is filtered when it is staged.** `tools/mod_channel_filter.py`
  emits a manifest without the developer features and without the `[[option]]`,
  `[[patch]]`, `[[overlay]]`, `[[plugin]]`, `[[resource]]`, `[[constraint]]` and
  `[[requirement]]` entries that only served them; a package whose every feature is developer has
  its directory removed. This is generation, not rewriting: the staged catalog
  is build output and the author's manifest in the repo is never touched.
- **`installed/` is refused at load.** A third-party archive is never modified,
  so the runtime declines to surface developer features from one instead.

The runtime gate is the build definition `PSX_MOD_DEVELOPER_CHANNEL`, which
`runtime.cmake` sets for a local build and clears under `$CI`. A contributor
reaches developer features by cloning the repo and building; a release build
carries neither the features nor their operations, so a stale `state.toml`
naming one cannot reach them either.

Packaging defaults the exclusion from `$CI` — on under any CI provider, off
locally. `--exclude-dev-mods` / `--include-dev-mods` (or `EXCLUDE_DEV_MODS=0|1`)
override it, and `project_studio build export --exclude-dev-mods` reproduces
what a release would contain.

## Trusted static plugins

Package format 5 can activate a game-owned plugin that is already statically
linked into the executable:

```toml
format_version = 5

[[feature]]
id = "warp-debug-menu"
name = "Warp Debug Menu"

[[plugin]]
feature = "warp-debug-menu"
id = "example.warp-debug"
```

The plugin id is a stable registry key, not a library path or symbol name. The
package archive supplies no native code. Resolution fails before launch when
an enabled plugin has no registered implementation or when two features claim
the same plugin id. Active plugin identities and owners participate in the
canonical plan fingerprint.

An implementation may register an activation callback, a deterministic
guest-VBlank callback, function-entry hooks, or any combination under the same
id. A function-entry hook (`psx_mod_register_function_entry_plugin`) runs at the
top of a generated function the game config lists in
`[recompiler] mod_function_entry_funcs`, and at every interpreted entry to the
same address (segment bits ignored), so the backend running the page does not
matter. Overlay shards compiled for any segment get the hook at the listed
function's bytes, however the config spells its segment
(docs/SEGMENT_AWARE_CODE.md §5.7). Like the other kinds it satisfies a manifest `[[plugin]]` and runs only
while the resolved plan activates its id: the active hooks are flattened into an
address table at plugin activation, and a plan change drops them until the next
activation. Activation runs after the
launcher's final mod-plan commit and before renderer/window initialization; it
is appropriate for a game-owned mod that selects a fixed display aspect or
another pre-boot host feature. VBlank callbacks run from the emulated GPU
VBlank event, independent of host presentation, pacing, turbo, or skipped
frames. Trusted callbacks receive only the narrow C services exposed by
`runtime/include/mod_plugins.h`. Games should continue to use declarative
patches and overlays when those operations are sufficient.

### Session starts and mod-owned state

What a plugin sets up should last only for a session whose resolved plan
activates it. Function-entry hooks follow that rule by construction: the commit
and the netplay clear empty the hook table, and only activation rebuilds it, so
a hook never runs in a session that did not activate its id (including a
netplay session, which clears the plan). The host state a plugin changes
through the `psx_mod_*` setters is process-wide, so the runtime resets it at
every session start instead.

An offline session ends the process when the player closes the game. The one
in-process second session is the lobby rematch: a netplay match launched from
the lobby returns to the lobby launcher when it ends, and the next launch from
there, netplay or offline, re-enters the emulator in the same process. The
session before a rematch is therefore always a netplay match, which ran with
the plan cleared, so no plugin activated in it -- except own-view plugins
(`[[plugin]] netplay = "local_view"`, docs/NETPLAY.md "Own-view mods"), whose
setters the reset below covers like any other.

The reset is one step of the session start that every session runs,
immediately before activation (see *A rematch is a full session start* below).
What it fixes today is the netplay local viewport's Fit and fixed aspect
carrying from a match into an offline rematch. The rest of the table is
defensive, for the state listed here.

| State | Setter | Reset to | When |
|---|---|---|---|
| Fit / capped adaptive aspect | `psx_mod_set_adaptive_display_aspect` | off, cap 16:9 | every session |
| World-scene predicate | `psx_mod_set_world_scene_predicate` | NULL | every session |
| Retained-scene predicate | `psx_mod_set_retained_scene_predicate` | NULL | every session |
| Adaptive backdrop preload | `psx_mod_set_adaptive_backdrop_preload` | 0 | every session |
| Draw-distance clamps | `psx_mod_set_draw_distance_clamp` | off | every session |
| Bezel artwork | `psx_mod_set_bezel_artwork` | none (see below) | every session |
| Frame-interpolation blend mode | `psx_mod_set_frame_interpolation_blend` | default | every session |
| Native VBlank pacing and its rate | `psx_mod_set_native_vblank_rate` | off, 0 | every session |
| PGXP request (`psx.enhancement.pgxp`) | the builtin `psx.pgxp` activation (`pgxp_mod_request`) | none | every session |
| Frame period, if native VBlank pacing was on | `psx_mod_set_native_vblank_rate` | first-session value | later sessions |
| Frame interpolation and its rate | `psx_mod_set_frame_interpolation` | first-session value | later sessions |
| Vsync forced off | `psx_mod_set_frame_interpolation`, `psx_mod_set_native_vblank_rate` | first-session value | later sessions |
| Automatic FMV skipping | `psx_mod_set_auto_skip_fmv` | first-session value | later sessions |
| 8 MiB main RAM request | `psx_mod_set_main_ram_8mb` | retail 2 MiB | later sessions |
| Texture-bank resolver and batching | `psx_mod_set_texture_bank_resolver`, `psx_mod_set_texture_bank_batching` | NULL, off | later sessions |

"First-session value" is what settings, environment overrides (such as
`PSX_VSYNC`) and the launcher resolved before the process's first activation.
The first call records those values and changes nothing that is not already at
its initial value, so the first session, and every run that never
soft-returns, behaves exactly as without the reset. The RAM request matters on
a rematch because `memory_init()` latches the requested geometry again at every
boot, including the rematch's.

The PGXP request is how the builtin `psx.enhancement.pgxp` package arms
geometry and texture correction, and its CPU-mode and precise-culling options.
Its activation records the request. The renderer setup runs after activation,
and its session arming (`psx_pgxp_session_arm`,
`runtime/include/pgxp_session.h`) takes the request: it reads it, clears it,
and arms PGXP from it and the `[video]` keys together. Arming the corrections
from activation directly does not work: the renderer setup applies the
`[video]` baseline afterwards and would switch them back off. A netplay session
clears the plan, default-on packages included, so it gets the `[video]` keys
alone, and nothing at all with `[video] pgxp_mod_only`. A title that ships
PGXP on by default overrides the builtin at the same id and version, with
`default_enabled = true` and, if it wants it, the `culling` option's default set
to `"true"` (ENHANCEMENTS.md G1.11/G1.12).

Bezel artwork has two parts. The reset clears the artwork path, so the next
session start loads nothing unless its own activation selects artwork again.
The OpenGL renderer drops the loaded texture itself when it shuts down, which
happens when a session ends by returning to the lobby (and at process exit),
so a rematch never draws the previous session's artwork in its new context.

Not reset, and why:

- The **fixed display aspect** (`psx_mod_set_fixed_display_aspect`) and the
  **renderer** (OpenGL, which `psx_mod_set_frame_interpolation` and
  `psx_mod_set_bezel_artwork` force) are launcher controls. A rematch takes
  both from the lobby launcher, which is seeded from the live values. For the
  aspect, the rematch path then re-applies the Settings clamp (4:3, since
  widescreen is mod-owned) before its session start, where a plugin's
  activation or the netplay local viewport can still replace it, so a match's
  16:9 or 21:9 does not carry into an offline rematch. A renderer forced by a
  plugin would be carried the same way, but no plugin activates in the session
  before a rematch.
- **Guest memory, GPU-DMA memory, texture-packet arenas and defined texture
  banks** (`psx_mod_alloc_guest_memory`, `psx_mod_alloc_gpu_dma_memory`,
  `psx_mod_alloc_texture_packet_memory`, `psx_mod_define_texture_bank`) live
  for the process. A bank ID is read only from packets in a plugin's own arena,
  which stock game code does not use.

**A rematch is a full session start.** It re-enters below the first session's
setup block, so after its commit or netplay clear it runs the same sequence as
the first session:

1. Clear controller-mode overrides and presentation policies, load
   acceleration and disc speed.
2. Reset the mod-owned state in the table above.
3. Run `mod_runtime_activate_plugins()`: the plan's activation callbacks, then
   its function-entry hook table.
4. Apply what activation chose: the netplay local viewport, controller-mode
   overrides and load acceleration. Disc speed is read when the session boots.
5. Mount the plan's derived disc image if it built one, else the stock disc.

An offline rematch with mods enabled therefore runs like a first launch with
those mods: activation callbacks, function-entry hooks, VBlank callbacks and
the main-EXE and disc patches. A netplay rematch clears the plan, so none of
them run and the match stays vanilla. Either way activation still precedes
renderer and window creation, which every session reaches only when it boots.

A plugin should establish what it needs in its activation callback and not rely
on state from an earlier session; its own static variables are its
responsibility.

`psx_mod_set_load_acceleration(multiplier, release_frames)` is the narrow
pre-boot service for a game-owned fast-loading feature. It changes host
wall-clock pacing only: guest VBlanks, CD deadlines, interrupts, callbacks, and
game logic still execute. Multipliers 2 through 16 are bounded choices; zero
selects uncapped host speed. A zero-frame release stops acceleration as soon as
the sustained-load predicate clears, which is appropriate for timing-sensitive
or speedrun-oriented packages.

`psx_mod_set_draw_distance_clamp(enabled)` switches the title's
`[[draw_distance.clamp]]` sites (docs/config_schema.md) on for the session: a
far primitive the game would drop past the end of its ordering table is kept
in the farthest slot. It returns 0 when the title lists no sites. More
primitives mean more guest work, so it belongs to an opt-in feature.

`psx_mod_set_disc_speed(divisor, instant_max_per_frame)` is the guest-visible
alternative. Divisors 2 and 4 shorten emulated CD deadlines; zero selects the
bounded instant scheduler. Because this changes interrupt timing, packages
should label it experimental and make it mutually exclusive with host-only
load acceleration (normally as choices in one default-off feature).

## Native operations

`main_exe` writes use PSX guest virtual addresses. Expected bytes are checked
after the BIOS loads the executable, then the complete write plan is applied
before its entry point. Changed executable ranges use the existing dirty-RAM
interpreter/native-overlay machinery; untouched functions remain on the static
native path.

Small `disc_raw` and `disc_user` patches are equal-length guarded writes and may
not cross a sector boundary:

- `disc_raw` offsets use `lba * 2352 + byte_in_sector`.
- `disc_user` offsets use `lba * 2048 + byte_in_sector`.

File-backed `[[overlay]]` operations are intended for large assets and may span
any number of sectors. Their paths must remain inside the archive. Payload
size and SHA-256 are verified while scanning, but disabled payloads are not
retained in memory. Enabled payloads are loaded and reverified during
resolution, then indexed by target and LBA before boot. A CD read performs a
direct indexed lookup rather than scanning every installed mod.

Feature disc overlays require an exact `disc_sha256` on every target entry.
`expected_sha256` can additionally guard the replaced stock range.

## State and migration

`mods/state.toml` format 2 stores selected package versions separately from
per-feature enabled states and values:

```toml
format_version = 2

[[package]]
id = "example.localization"
version = "1.2.0"

[[feature]]
package_id = "example.localization"
id = "title-screen"
enabled = true

[feature.values]
variant = "rockman"

[feature.resources]
artwork = "C:/Users/You/Pictures/example-bezel.png"
```

State format 1 and package-only manifests remain readable as a migration aid.
They appear through one synthetic legacy feature. New packages should use
explicit features.

**Selections for packages the catalog does not hold are dormant.** State can
outlive the package it names: the player deleted an installed archive, a
release build stripped a developer-only package, or the title now declines a
framework builtin with `EXCLUDE_BUILTIN_MODS`. Resolution only visits packages
that are present, so such a selection contributes nothing to the plan (and
does not change its fingerprint), and it is not a launch error — the Mods page
no longer lists the package, so the player would have no way to clear one.
`save_state()` keeps the entry verbatim, so the choice applies again if the
package ever returns, and the runtime names each one at startup
(`mod selection kept but inactive: <id> is not in this build's mod catalog`).
A selection pinned to a *version* that is missing while other versions of the
package are present remains an error, because the Mods page can fix that one.

The old `derived_disc` VCDIFF mechanism is legacy conversion scaffolding only.
Feature-style manifests reject it. It is not a product mod primitive, fallback,
or image-selection workflow; patched discs may be used offline as parity
oracles while converting known mods to native operations.

## Resolution and diagnostics

Before boot, the manager:

1. verifies the selected stock game and revision;
2. expands only enabled features and their selected options, plus every
   feature an active `[[requirement]]` activates (see
   [Implicit requirements](#implicit-requirements-across-packages));
3. orders active packages deterministically by dependencies;
4. verifies enabled payloads and operation bounds;
5. collision-checks the complete owned byte-range plan and guard
   compatibility;
6. coalesces only truly identical target/range/expected/replacement writes or
   identical overlays; and
7. produces a canonical SHA-256 plan fingerprint.

Incompatible overlaps fail before launch. Structured diagnostics identify both
`(package, feature)` owners and the exact contested target range. The launcher
marks both feature rows and lets the user decide what to disable. It never
silently chooses a winner.

Operation boundaries are not semantic boundaries. Legacy full-record writes
compose when both their expected and replacement bytes agree throughout the
owned intersection. Format-4 sparse patches collide only on declared owned
fields, while their complete guards must remain mutually compatible.
Partially overlapping overlays compose when their replacement payload bytes
agree. A differing owned byte or incompatible guard produces a diagnostic at
that exact location. Exact duplicate operations may be coalesced.

Package-level dependencies and conflicts are reserved for actual implementation
relationships. Mutually exclusive choices such as US versus Japanese artwork
belong inside one feature as option values. A relationship that holds only for
some selections of one feature is a `[[requirement]]`, not a dependency.

## Implicit requirements across packages

The shared PGXP plugin uses `psx_mod_set_pgxp_precision(enabled, cpu_mode)`.
This stores a session selection as well as setting live correction flags, so
later renderer initialization cannot erase mod activation with the base video
settings. Session start clears the selection before activating the new plan;
the player's persistent settings remain unchanged. Explicit validation
environment overrides still take precedence at renderer initialization.

Package format 7 lets a feature, while a condition on its own options holds,
need a feature of **another** package:

```toml
format_version = 7
id = "wipeout3.enhancement.framerate"

[[requirement]]
feature = "framerate"                 # the requiring feature (owner)
package = "psx.enhancement.8mb-ram"   # the package that provides it
requires_feature = "8mb-ram"          # its feature to activate
when = { extras = "enhanced" }        # same condition syntax as [[plugin]]

[[requirement]]
feature = "framerate"
package = "psx.enhancement.8mb-ram"
requires_feature = "8mb-ram"
when = { extras = "full" }
```

`when` (or `when_option`/`when_value`) is the same feature-local condition
plugins and overlays use; every listed option must match, so a requirement that
holds for several values is written once per value. Omit it for a requirement
that holds whenever the feature is enabled. `version` optionally restricts the
provider with the `[[dependency]]` range syntax (`"*"` by default). Unknown
fields are rejected, and so is a requirement naming its own package: a feature of
the same package is a `requires_feature` `[[constraint]]`.

The two existing mechanisms could not express this. `[[dependency]]` is
package-level and unconditional, so WipEout 3's "NTSC / PAL Mode" would need
8 MB RAM even with Extras = None; a `requires_feature` constraint cannot name
another package.

**Resolution.** While the requiring feature is enabled and its condition holds,
`resolve()` activates the required feature for the session:

- even when it is `hidden`, default-off, or explicitly disabled in
  `mods/state.toml` -- the player chose the requiring option, and the requiring
  option cannot run without it;
- recursively: a derived feature's own `[[requirement]]`s and in-package
  `requires_feature` constraints are derived too, to a fixed point;
- **without writing it to `state.toml`**. A derived activation is not a player
  choice, so `save_state()` persists only what the player selected, and turning
  the requiring option back off leaves nothing behind. `feature_enabled()` keeps
  reporting the player's choice (the launcher's checkbox, so a hidden required
  feature stays hidden); `feature_implicitly_enabled()` reports the derived one.

The plan lists each derived feature in `ModResolution::implicit_features` with
what required it, plans with and without the derivation have different
fingerprints, and a committed plan's plugins read option values from
`ModResolution::selections` -- the selection the plan was actually built from.
The runtime names every derived feature at launch:

```text
psxrecomp: mod feature psx.enhancement.8mb-ram/8mb-ram activated implicitly (required by wipeout3.enhancement.framerate/framerate)
```

**An unmet requirement fails loudly.** When the provider is absent from the
catalog (removed, stripped from a release, or declined with
`EXCLUDE_BUILTIN_MODS`), the selected version is missing, the version is out of
range, or the provider has no such feature, the plan is rejected before launch:

```text
wipeout3.enhancement.framerate/framerate requires psx.enhancement.8mb-ram/8mb-ram, but package psx.enhancement.8mb-ram is not in this build's mod catalog
```

It is also a diagnostic on the requiring feature, so the launcher marks that
row. The requiring selection never runs without what it requires -- an 8 MB
build on 2 MB RAM is exactly the failure this prevents. A package an active
requirement uses cannot be removed from the Mods manager.

AOT profiles pin active requirements the way they pin plugins:
`tools/mod_package_images.py` reports each active one as `"<package>/<feature>"`
and a `mod_packages` entry must list the exact set under `requirements`.

The launcher needs no change for a hidden required feature: it lists a hidden
feature only when the player enabled it, and the player did not. A *visible*
required feature still shows the player's own checkbox state while it is
derived; showing "on, required by X" needs a launcher field that does not exist
yet.

## Trusted adapters and archive safety

### Owner-selected resources

Format-5 packages may declare feature-owned resources that the launcher renders
with its native file/folder picker:

```toml
[[resource]]
feature = "bezel"
id = "artwork"
label = "Bezel image"
description = "Select an image to draw behind the game frame."
format = "file"
file_patterns = "*.png,*.jpg,*.jpeg,*.bmp"
file_description = "Image files"
required = false
```

Resources are paths selected by the player and persisted in `mods/state.toml`;
they are not copied into the package. Optional resources with no selected path
are omitted from the committed plan. Required resources reject launch while the
feature is enabled and unset.


Format 8 adds engine-verified donor media to the same picker and state format:

```toml
[[resource]]
feature = "arena"
id = "donor-rom"
label = "Source ROM"
format = "n64-rom"
file_patterns = "*.z64,*.v64,*.n64"
required = true
size = 8388608
sha256 = "<64 lowercase hexadecimal digits from the canonical image>"
```

`size` and `sha256` must both be present. Verification runs only for enabled
features. Missing required media, a removed selected file, an incorrect size,
or a hash mismatch rejects the launch plan before plugin activation. Disabling
the feature restores the stock launch; the selected path is preserved.

Canonical identity domains:

| `format` | Size and SHA-256 domain |
|---|---|
| `file` with identity fields | Exact file bytes, at most 512 MiB |
| `n64-rom` | Big-endian `.z64` bytes; `.v64` halfword swaps and `.n64` word swaps normalize first; 64 bytes to 64 MiB, word aligned |
| `psx-disc` | First data track's 2048-byte sector payloads, at most 512 MiB; CUE/BIN/ISO/CHD use the shared disc reader; audio tracks are excluded |

For `psx-disc`, Mode 1 payloads start at raw-sector offset 16 and Mode 2
payloads at 24. This domain retains the first 2048 bytes of Form 2 sectors;
it is intended for asset data, and is not a full XA/CD-audio identity. Mixed-mode
retail ISO headers can declare a volume size extending into audio tracks; the
reader uses the actual first-track boundary from the TOC or single-track image
length. An audio-first disc or a nonzero first-track start is rejected. Use a
CUE for a raw image containing multiple tracks. Declared volume metadata must
have matching byte orders and 2048-byte logical sectors.

The resolved plan owns an immutable canonical snapshot. During its callbacks,
a trusted plugin calls `psx_mod_current_resource_bytes(id, &bytes, &size)` to
obtain a read-only view scoped to its own package and feature. Ordinary
unverified resources cannot supply bytes through this API. The pointer remains
valid until the committed plan is replaced or cleared. Decode into host or
enhancement memory and rebuild derived data when activation changes; never
reopen the owner path as a substitute for the verified snapshot.

An activation callback can mount a slice of its verified resource for native CD
streaming with `psx_mod_append_disc_extent(id, byte_offset, sector_count, &lba)`.
The slice contains 2336-byte Mode-2 sectors beginning at the duplicated XA
subheader. The runtime appends extents after the mounted disc, synthesizes raw
sector headers and a data-track TOC/subchannel entry, and retains the original
CD-ROM/XA decoder, interrupts and timing. The returned LBAs are deterministic
for the same activation order. Registration rejects missing/unowned resources,
invalid ranges/subheaders, late calls and addresses beyond the CD MSF limit.
Plan replacement or netplay clearing removes the extents; base sectors remain
unchanged. The game plugin supplies its own file lookup, names and stream
selection. Donor bytes remain external and are covered by the resource fingerprint.

Trusted activation callbacks can also append an audio-only playlist with
`psx_mod_append_cdda_tracks(tracks, count, first_lbas, sector_counts)`.
Each `PSXModCDDATrack` either names an owned verified resource plus a byte
offset/count of raw 2352-byte stereo PCM sectors, or names an audio track on
the mounted disc. A batch succeeds completely or leaves the previous playlist
unchanged. Outputs are deterministic; late calls, non-audio disc tracks,
invalid resource ranges, more than 98 audio tracks and the CD MSF limit are
rejected. Mounted-track slices exclude INDEX00 pregaps.

The playlist supplies the CD controller's audio TOC (data placeholder track 1,
audio tracks 2 onward, track 0 lead-out). Filesystem and XA LBAs remain on
the original data timeline. The native CD controller still performs playback,
reports, volume mixing and save-state serialization; no host playback clock
is introduced. Plan replacement/reactivation clears the playlist, and a
disabled plan follows the existing disc reader unchanged. Titles own native
menu integration and must relocate fixed-size TOC/name buffers before exposing
more tracks. `cdrom_trace_dump` reports audio sectors as `cdda_sector`, with
the virtual LBA in `val` and track number in `w`.


Verified resource fingerprints include format, canonical size and SHA-256,
rather than the selected path, so moving identical media or changing N64 byte
order does not change save compatibility. Donor bytes are never package files,
and donor executable code is not dispatched by this API. N64 ZIPs must be
extracted before selecting the ROM. Format 8 prevents older runtimes from
silently accepting manifests whose identity checks they cannot enforce.

`resolver = "builtin:<id>"` selects a resolver statically registered by the
game. Format-5 plugin ids likewise select only statically registered
implementations. Packages cannot load arbitrary native code or select
arbitrary symbols.

The installer accepts stored or DEFLATE-compressed ZIP entries, validates CRCs,
rejects encrypted entries and unsafe or absolute paths, limits archives to 4096
files and 256 MiB expanded size, stages extraction, validates the manifest, and
publishes the version atomically.

### Guarded instruction callbacks

Trusted plugins can register
`psx_mod_register_instruction_plugin(id, address, expected_word, callback)`.
List native code locations in `[recompiler] mod_instruction_sites` and regenerate.
Callbacks run immediately before the instruction; the registered full word,
executing word and current RAM word must agree. Segment aliases share a site.
Only active resolved plugins run, with their feature's resource context.
The dirty-RAM backend uses the same checks, including nested delay slots.
Overlay ABI v27 carries the forwarder; site metadata changes the codegen hash.

Callbacks may change registers and data. They must preserve PC and must not
re-enter guest execution, because pending branch/load state is live.
`psx_mod_finish_function` is unavailable in this callback scope. Inactive plans
do not change guest behavior. Function entry replacement remains the separate
`psx_mod_register_function_entry_plugin` interface.

### Presentation bezel packages

The framework registers `psx.bezel`, a trusted presentation plugin for OpenGL
margin artwork. The built-in package declares an optional `artwork` image
resource; when the feature is enabled and the player has selected an image, the
runtime draws that image behind the game frame before presenting the normal 4:3
or widescreen content.

The built-in package `psx.presentation.bezel` targets every game but defaults to
off, so the default presentation is unchanged: letterbox and pillarbox margins
remain black. Enabling the feature without choosing artwork is also a no-op.
The package supplies only the declaration and trusted plugin selection; archives
still cannot load native code.

A title's own trusted plugin can call `psx_mod_set_bezel_artwork(path)` directly
to ship its artwork. An absolute path (such as the built-in package's selected
resource) is used as-is; a relative path like `"bezels/qirex.png"` is resolved
against the executable's directory when the artwork loads, never the current
working directory, so artwork the title stages beside its binary is found
however the game was launched.

### Retained-scene loading presentation (native-wide opt-in)

A trusted game plugin can register
`psx_mod_set_retained_scene_predicate(predicate)` from `mod_plugins.h` when the
game keeps displaying its previous framebuffer while loading. The cheap, pure
emulation-thread callback returns `PSX_MOD_SCENE_HOLD` while that same scene is
retained. It must not call presentation APIs recursively. Passing NULL removes
the opt-in. Without registration, existing presentation behavior is unchanged.

In native-wide mode this holds the previous wide/4:3 classification even if a
game-state flag or absent GTE activity would normally classify loading as 2D.
It does not force menus wide, stretch artwork, change guest rendering or memory,
or override the FMV veto. Return `PSX_MOD_SCENE_RELEASE` when a new scene replaces
the retained image, so ordinary classification resumes. For double-buffered
games whose draw-ready signal precedes the actual display flip, return
`PSX_MOD_SCENE_UNTIL_FLIP`: the prior hold ends only when the displayed VRAM
origin changes. Without a prior HOLD it behaves like RELEASE. Do not use
UNTIL_FLIP for in-place image replacement. Crash's experimental
adaptive feature uses its pending level transition and draw-skip globals for
this; these game-specific addresses do not belong in the framework.

GPU reset and savestate restore discard this host-only history. A save loaded
directly into a frozen loading frame cannot recreate wide reveal strips absent
from the canonical saved framebuffer. `ws_scene_hold_test` covers long holds,
menu release, delayed buffer flips, retained 4:3 scenes, FMV and timeline reset.


## Shared source media and automatic preparation (format 9)

A linked trusted preparer lets a title accept source discs/ROMs and prepare
verified resources before committing a launch. Manifests name registered code;
they cannot name executable commands. Set package `prepare = "provider.id"`.

A `[[resource]]` with `input_only = true` is a picker for preparation input,
not a runtime resource. `shared_source = "game.original"` stores one path in
`[sources]` in mods/state.toml; every package with that key shows that same path.
Changing either picker updates the shared binding. Earlier per-feature paths
are used as migration defaults. Source paths never affect runtime fingerprints.

Derived resources declare `hidden = true` plus their expected `size` and
`sha256`. The launcher omits their pickers. The preparer receives the selected
main-page disc, source inputs, application directory and user cache root. It
returns declared output paths; the normal resolver then verifies and snapshots
their bytes. All active preparers must succeed before new bindings are published.
Disabled features perform no preparation. Preparation also runs for implicit
requirements and the committed netplay selection. Cache files and donor media
remain user-owned and outside the bundled package directory.

Register via `mod_register_media_preparer` in mod_packages.h. Ship any worker
and its runtime dependencies with the title: players should only supply media.

Launcher preparation is synchronous by default. A title may set
`PSX_LAUNCHER_MOD_COMMIT_WORKER_SAFE=ON` only after auditing all its trusted
preparers and services for serialized worker execution without UI, SDL, GL,
or main-thread affinity. With a recomp-ui header advertising
`RECOMP_LAUNCHER_HAS_WORKER_MOD_COMMIT`, preboot offline PLAY then commits on an
owned worker while the launcher renders an exclusive progress view. The UI
does not access the provider until that worker has joined; close is queued
until completion. Verification, preparation, persistence, and failure gates
are unchanged. Older UI pins, other titles, netplay, and in-session commits
retain synchronous behavior. Plugin activation remains on the runtime's
existing post-commit path, not the preparation worker.
The `prepare_resources` method is separate from read-only `resolve`, so editing
launcher settings does not trigger conversions. Runtime activation still sees
only fully verified immutable resources.
