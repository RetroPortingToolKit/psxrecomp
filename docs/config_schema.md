# PSXRecomp v4 — config TOML schema

Consumed by:
- `psxrecomp-bios.exe` and `psxrecomp-game.exe` (via `--config <toml>`)
- `tools/audit_config.py` and the audit tooling that wraps it
- The runtime cmake macro (`runtime/runtime.cmake`)

Examples:
- `bios/SCPH1001.toml` — BIOS-only config (no game; psx-runtime targets this)
- `../TombaRecomp/game.toml` — game config (tomba-runtime targets this)

## How configs combine

A PSXRecomp v4 process **always** has a BIOS config. It optionally also has
a game config. Both are TOML files in this schema:

- `bios/SCPH1001.toml` (or another BIOS .toml in `bios/`) — describes the
  BIOS. Always loaded.
- `<game>/game.toml` — describes a single game. Loaded ONLY when running
  that game.

```
psxrecomp                            # BIOS-only regen (uses default bios.toml)
psxrecomp games/tomba/game.toml      # BIOS + game regen
psx-runtime                          # boots BIOS discless
psx-runtime games/tomba/game.toml    # boots BIOS, then loads game
```

How the two configs relate:

- **Scalar keys (`debug_port`, `window_title`, `memcard_dir`, ...)**: these come
  from `game.toml` only. There is **no BIOS→game inheritance.** An earlier
  version of this document described a shallow override where the game won and
  otherwise inherited from `bios.toml`; that merge was never implemented.
  `load_bios_config` (`recompiler/src/config_loader.cpp`) is called only from the
  recompiler front-ends — `main_bios.cpp`, and `main_psx.cpp` purely to build the
  `BiosAddressModel` — and never from `runtime/src/main.cpp`. Setting a
  `[runtime]` scalar in a BIOS toml has no effect on a game run.
  Runtime precedence is: environment > CLI > `settings.toml` > `game.toml` >
  compiled-in default.
- **`[program]` (BIOS) and `[game]` blocks**: NOT merged — they describe
  different programs. Both are visible to the loader.
- **Generated dispatch tables and C output**: ADDITIVE. BIOS contributes
  `SCPH1001_*.c`; game contributes `<exe>_*.c`. No address overlap is
  expected (BIOS lives at RAM 0x500-0x8500 + ROM 0xBFC..., game at
  0x80010000+). The cmake macro `psxrecomp_v4_add_runtime_target` already
  links both.
- **`[[audit.regions]]` and `[[audit.normalize.remap]]`**: additive — game
  adds its regions on top of BIOS's.

## Top-level blocks

```toml
[program]    # in bios.toml; describes the BIOS
[game]       # in game.toml; describes the game
[prepare_disc]  # optional; data-track digests for prepare/verify
[netplay]       # optional; TOC / cue policy for online
[recompiler]
[draw_distance] # optional; opt-in far-geometry clamps (below)
[runtime]
[audit]
```

`[prepare_disc]` digests identify the main track only. See
[disc companions](DISC_COMPANIONS.md) for the separate SBI input gate,
exact revision coverage, and preparation receipts. No SBI configuration key
is required.

## Netplay disc mount (`[netplay]`)

Optional. Online play needs the same CD geometry on every peer — data-track
CRC/SHA alone cannot distinguish a Track-01-only dump from a full Redump
multi-track cue. The runtime mounts the resolved path, fingerprints the TOC
(`disc_fp`), and gates Host/Join on `netplay_ok`. Peers also exchange
`disc_fp` through the lobby (`disc_mismatch` on join).

| Field | Default | Description |
|---|---|---|
| `require_cue` | `false` | Require a `.cue` mount (reject bare `.bin` / cue→bin fallback) |
| `required_tracks` | `0` | Exact `iso_track_count` when > 0 (e.g. MotK Redump = `17`) |
| `required_leadout_lba` | unset | Exact lead-out LBA when set |
| `required_disc_fp` | `""` | Exact lowercase hex SHA-256 TOC fingerprint when non-empty |
| `required_disc_fps` | unset | Multi-disc: array parallel to `[game] discs`, the TOC fingerprint of each disc. Each disc of a set has its own TOC, so a set gated only on `required_disc_fp` (the boot disc's) refuses online play on every other disc the launcher lets the player select. A disc with no entry falls back to `required_disc_fp`. |

The whole `[netplay]` policy is resolved **per mounted disc**, not once per
build — `required_tracks` and `required_leadout_lba` are per-disc facts too (a
set may mix a CD-DA disc with a data-only one, and the lead-out LBA is the
disc's size). Only `required_disc_fps` carries per-disc data today; the
resolution point is `netplay_expect_for_disc()` in `runtime/src/main.cpp`, and
that is where the others go when a title needs them.

Offline Play may still launch with a TOC warning; first-run setup Finish and
online Create/Join require `netplay_ok` (and online also a clean verify +
non-empty `disc_fp`). Mirror `required_tracks` in the Retro catalog as
`rom_identity.track_counts` so the hub library scan rejects Track-01-only dumps.
Wizard / Retro / catalog submission accept Redump `.cue` + sibling `.bin`
tracks, or a MAME-compatible `.chd` of the same dump: `tools/psx_chd.py` reads
it through the libchdr the emitters build (CMake target `chdr`) and writes the
Redump-shaped track files back, so `[prepare_disc]` digests, SBI companions and
`required_tracks` apply unchanged. A bare `.iso` cannot expand to multi-track.

## Program / game block

- **bios.toml** has a `[program]` block describing the BIOS ROM.
- **game.toml** has a `[game]` block describing the game EXE / disc.

These are NOT alternatives; they're complementary. A runtime loading both
sees both blocks. The legacy single-file audit loader
(`tools/audit_config.py`) accepts either as the program-info source for
backwards compat, but going forward they are the canonical names for their
respective files.

### Fields

| Field | Required for | Description |
|---|---|---|
| `name` | both | display name, e.g. `"SCPH1001 BIOS"` |
| `id` | both | canonical id, e.g. `"SCPH-1001"` or `"SCUS-94236"` |
| `rom` | bios | path to raw flat binary, relative to project root |
| `exe` | game | path to PS-X EXE file, relative to project root |
| `load_address` | both | hex string, virtual address of first byte (`"0xBFC00000"` BIOS, `"0x80010000"` typical game) |
| `entry_pc` | both | hex string, first PC to execute |
| `text_size` | both | hex string, size in bytes of the static region. For games this also bounds main-EXE analysis and establishes the overlay floor. A smaller-than-header bound must be verified non-code and 4 KiB aligned. |
| `stack_base` | game | hex string, initial `$sp` value for the game |
| `disc` | game (single-disc) | path to .cue, relative to project root |
| `discs` | game (multi-disc) | array of .cue paths; `disc` is sugar for `discs = [disc]` |
| `disc_serials` | game (multi-disc, optional) | array parallel to `discs`: the serial each disc carries (`["SCUS-94163", "SCUS-94164", "SCUS-94165"]`). Without it every disc is checked against `[game] id` — the BOOT disc's serial — so selecting disc 2 reports "wrong disc". A disc with no entry here is not serial-gated; the ISO-header check still applies. |

### Multi-disc selection

Relative paths in executable-side `settings.toml` (`[disc] path`, `[bios] path`,
`[memcard] dir`, `card1`, `card2`) and in `disc.cfg` / `bios.cfg` resolve from the
executable directory, including when launched from another working directory.
Absolute paths and UNC paths keep their original location. A relative `--disc`
command-line argument still resolves from the caller's working directory.

A build whose `discs` array has more than one entry grows a **Disc Selection**
dropdown in the launcher, above the Serial/Region/ISO-header checklist. The
choice is persisted in `settings.toml`:

```toml
[disc]
path     = "/abs/path/Game (Disc 2).bin"   # the image actually mounted
selected = 2                               # 1-based index into [game] discs
```

`selected` names the disc and `path` only survives when it *is* that disc
(same file-name stem), so writing `selected` alone — from an external launcher
or by hand — switches discs even though `path` still points at the previous
one. That is what makes disc choice manageable like any other setting. Both
keys are written only for multi-disc titles.

## Recompiler block

```toml
[recompiler]
seeds       = "recompiler/seeds/phase2_ghidra_seeds.json"  # BIOS
seeds       = "seeds/ghidra_funcs.txt"                     # game (note: game seeds aren't json today)
bios_thunks = "seeds/tomba_bios_thunks.txt"                # game-only
bios_config = "psxrecomp/bios/SCPH1001.toml"               # game-only: BIOS profile whose address
                                                           # model game codegen folds RAM aliases
                                                           # through (see "BIOS profiles" below);
                                                           # defaults to the SCPH1001 profile
out_dir     = "generated"                                  # both
strict      = true                                         # both — currently always true
discovery   = "whole-image"                                # game-only: "whole-image" or "reachable"
out_stem    = "SCPH1001"                                   # optional; overrides the auto-derived stem

[[recompiler.patch]]
id          = "descriptive-policy-name"
address     = "0x80012340"
expected    = "0x24020002"
replacement = "0x24020001"
note        = "Why this game-owned instruction change is required" # optional
```

Output filenames: `<out_dir>/<out_stem>_full.c` and
`<out_dir>/<out_stem>_dispatch.c`. If `out_stem` is omitted, it's derived
from the `rom`/`exe` file basename with the trailing `.BIN` or `.EXE`
stripped (`Path.stem` is NOT used because it mishandles `SCUS_942.36`).

Game `discovery` defaults to `"whole-image"`, preserving the existing sweep and
pointer-table heuristics. Opt-in `"reachable"` starts at the executable entry
and evidence-backed seed roots, then follows callable direct `jal` targets. It
does not sweep arbitrary bytes for prologues or return-shaped words. Unresolved
`jalr`/indirect targets and unseen callbacks fail closed to runtime
interpretation; add an evidence-backed seed (or a `dispatch_root` seed for a
proven nonstandard boundary) when they should be compiled.

When `game.text_size` is smaller than the PS-X EXE header size, recompilation
uses it as a static-analysis bound. The value must be nonzero, instruction- and
4 KiB-aligned, retain `entry_pc`, and not extend past PS1 RAM. The original EXE
is still loaded from the user's disc. A generated config's canonical final-page
reservation may be slightly larger than the header; it does not widen analysis.

Each `[[recompiler.patch]]` replaces one exact 32-bit MIPS word before function
discovery, control-flow analysis, and normal translation. It is intended for
small, understood game-code changes whose addresses, opcodes, and policy remain
in the game repository. The framework does not contain title IDs or
title-specific addresses.

- `id`, `address`, `expected`, and `replacement` are required hex/string
  fields; `note` is optional.
- IDs are case-sensitive and unique within one config.
- Addresses must be four-byte aligned and are unique by the PSX 29-bit physical
  address. Thus `0x00012340`, `0x80012340`, and `0xA0012340` are aliases of one
  site and cannot define separate patches.
- Main-EXE generation fails if the word at the target site is not `expected`.
  This catches a wrong disc revision or stale patch instead of guessing.
- Captured overlays may place unrelated variants at one virtual address. In
  overlay mode, a patch is applied only to a variant whose word is `expected`;
  a nonmatching variant is translated unchanged.
- When `--config` and `--ws-config` supply the same byte-identical patch it is
  deduplicated. Reusing an ID or physical address for different patch data is
  an error.

Patches are build-time inputs, not runtime memory writes or live toggles.
Regenerate the affected main executable or captured overlays after changing
them.

### Guarded packed-coordinate rejection

Some polygon funnels combine packed SXY flags and reject on `BNE flags,zero`.
The native-wide GPU can clip horizontal overflow while the game retains its
vertical rejection bits. Declare only verified render rejection branches:

```toml
[[widescreen.cull.masked_reject]]
address = "0x80021ECC"
expected = "0x15E0FFC7"
reject_mask = "0xFFFF0000"
```

The full instruction guards each site on static, cached-overlay and interpreter
paths. At native 4:3, menus and FMV the original predicate is unchanged. During
wide gameplay only the specified flag bits participate in the predicate; guest
registers, the branch delay slot, vertical/depth/backface tests and timing remain
intact. Adapters must provide sufficient bounded primitive/capture capacity.
Changing a site, guard or mask changes the overlay configuration fingerprint.

### Guarded widescreen participation comparisons

`[[widescreen.cull.packed_x]]` handles a proven mesh screen-X predicate
expressed as `SLTU rd, (SX << 16), (width << 16)`:

```toml
[[widescreen.cull.packed_x]]
address = "0x801466A0"
expected = "0x0079C02B"
```

The complete instruction guards native overlays and the interpreter. While
wide, X must lie in `[-margin, width + margin)`; both sides widen together.
At 4:3 the original unsigned comparison is exact. Nonzero low-half operands,
zero width, and widths above 1024 retain the original comparison. It changes
only the predicate, preserving GPR operands, vertical, backface, depth and
ordering-table limits. Sites affect the overlay cache identity; regenerate
main/native overlays after changing them. Empty lists are inert.

Games may disable a proven object/model cull verdict in widened world views
without changing true 4:3 behavior:

```toml
[[widescreen.cull.keep]]
address = "0x8002B310"
expected = "0x28A21C01"
result = 1
```

- `expected` must encode `SLT`, `SLTU`, `SLTI`, or `SLTIU`.
- `result` must be 0 or 1.
- The site identity is the normalized physical address plus the complete
  32-bit instruction. A nonmatching overlay variant at the same VA is left
  unchanged.
- At true 4:3 the original comparison is evaluated. The configured result is
  forced only when `psx_ws_x_margin() > 0`.
- Native generated code and the dirty-RAM interpreter implement the same
  semantics.
- Regenerate main/overlay native code after changing the list.

Prefer an aspect-derived cone over `keep` when the original predicate is a
camera-frustum test. `keep` has no queue policy and is appropriate only for a
separately proven binary verdict.

### Aspect-aware terrain and model participation

Exact terrain-frustum angle loads can follow the live horizontal field:

```toml
[[widescreen.cull.angle]]
address = "0x8013F138"
expected = "0x24020155"
```

`expected` must be `ADDI`/`ADDIU rt,zero,imm`, with a positive 12-bit angular
half-extent below one quarter-turn. The helper widens `tan(angle)` by the live
per-side horizontal extent. It is exact at 4:3 and full-word guarded against
same-address overlay variants.

A model-list or per-child cosine rejection can use a horizontal-only envelope:

```toml
[widescreen.cull.aspect_cone]
forward_addr = "0x1F8000E8" # signed Q12 X/Z/Y halfwords
object_type_offset = 12
object_reg = 19
x_reg = 16
z_reg = 17
y_reg = 18
hysteresis_pixels = 24
queue_reserve = 4
queue_count_addrs = ["0x1F800144", "0x1F800150", "0x1F80015C"]
queue_capacities = [24, 40, 28]
queue_type_masks = ["0x00000204", "0x00000010", "0x00000020"]

[[widescreen.cull.aspect_cone.sites]]
address = "0x80077368"
expected = "0x28620358" # signed SLTI reject predicate

[[widescreen.cull.aspect_cone.sites]]
address = "0x8002B368"
expected = "0x0082202A" # signed SLT reject predicate
cosine_threshold = 856  # required for SLT; Q10
object_reg = 20         # optional per-site register overrides
x_reg = 19
z_reg = 18
y_reg = 17
queue_guard = false     # this lower-level predicate appends to no fixed queue
```

- Sites must be signed `SLTI` or `SLT` reject predicates: zero is the keep
  path and one is rejection.
- An `SLTI` site derives its Q10 cosine threshold from the immediate unless
  `cosine_threshold` is given. An `SLT` site requires it explicitly.
- A vanilla keep is always preserved. Only a vanilla rejection is retested.
- Horizontal reach follows the current client aspect. Vertical reach,
  near/far checks, type dispatch, and the game’s queue-capacity branches are
  unchanged.
- `guard_pixels` is the activation guard outside the visible field.
  `hysteresis_pixels` moves deactivation farther out.
- For `queue_guard = true`, non-visible guard/hysteresis candidates are
  rejected at `capacity - queue_reserve`, preserving headroom for candidates
  intersecting the visible wide field.
- Use `queue_guard = false` only after proving that the exact predicate does
  not append to those queues.
- Site address, instruction, threshold, registers, queue policy, guard size,
  and enclosing cone/queue metadata all contribute to overlay cache identity.
- Generated/native overlay code and the dirty-RAM interpreter use the same
  live helper. Dynamic resizing therefore needs no recompilation.

The debug server’s `ws_aspect_cone_site` command accepts an `address` string
and reports exact-site identity/keep/reject counters.

Signed horizontal bounds can be widened at the constant-load site:

```toml
[[widescreen.signed_x_bound]]
address = "0x800BD290"
expected = "0x2402FF00" # addiu v0,zero,-256
```

- `LUI rt,imm` sites are signed Q16 gameplay bounds and use the gameplay-field
  scaler.
- `ADDIU rt,zero,imm` and `ORI rt,zero,imm` sites are screen-pixel bounds and
  move by the live horizontal margin. ADDIU sign-extends the constant; ORI
  zero-extends it. The destination and immediate must both be nonzero.
- Site identity is the normalized physical address plus the complete
  instruction word. The helper is identity at 4:3.

Signed per-vertex screen-X culls can be listed site by site
(`slti_sites`, `slti_lower_sites`, `bltz_sites`, `bgez_sites`,
`clip_edge_x_load_sites` with `clip_edge_width`, `branch_keep_sites`). All are
identity at 4:3, empty by default, and need a regen; `docs/WIDESCREEN.md`
("Explicit screen-X cull sites") has the semantics of each kind:

```toml
[widescreen.cull]
bgez_sites = ["0x80013F40"]              # bgez SX,keep: keep while SX >= -margin
bltz_sites = ["0x80013F58"]              # bltz SX,reject: reject while SX < -margin
clip_edge_x_load_sites = ["0x8005F5F4"]  # lh of a clip bound: 0 -> -m, W -> W+m
clip_edge_width = 320                    # 1..1024; default screen_w_imms[0]
```

- Main-EXE generation fails (exit 1) when a listed address does not hold the
  expected instruction: `slti` for both slti kinds, exactly `bltz` / `bgez`,
  `lh`/`lhu`/`lw` to a nonzero register for clip-edge loads, and a
  conditional branch (`beq`, `bne`, `blez`, `bgtz` or REGIMM) for
  `branch_keep_sites`.
- `bgez_sites` and `clip_edge_x_load_sites` (with the width) contribute to the
  overlay-cache identity only when non-empty.

Explicit `bias_sites` / `bias_lower_sites` / `range_sites` may opt into an additional resident
object lead without widening terrain or render queues:

```toml
[widescreen.cull]
guard_pixels = 16
activation_guard_pixels = 256
bias_sites = ["0x80069BA8"]
range_sites = ["0x80069BB0"]
```

`activation_guard_pixels` is added only to the live margin emitted at those
three explicit site families, and only while widescreen reveals extra world.
At true 4:3 it is exactly zero. `guard_pixels` remains the shared
render/terrain participation guard; keep it small when terrain producers or
model queues have fixed capacity. Both values are restricted to `[0, 256]`
and contribute to native-overlay cache identity. Changing the activation
guard requires regenerating the game and overlay code.

`bias_lower_sites` is the lower-endpoint counterpart to `bias_sites`:
an `ADDI`/`ADDIU` camera-relative bound subtracts the activation margin from
its original immediate. Both native code and the dirty-RAM path apply it.
For strip-based enemy spawning, expand the outer strip edge, initial and
vertical scan X bounds, and any associated respawn-reset interval together.
Keep authored placement flags and vertical bounds intact. Empty is inert;
configured sites require regeneration.

## Draw-distance clamps (`[[draw_distance.clamp]]`)

Many PS1 renderers drop a primitive whose depth index falls past the end of
their ordering table (`sltiu t, z, N; beqz t, reject`). The limit protects the
OT, so raising it can write past the table. A title can list such guards and
let a trusted mod keep the far primitive in the farthest slot instead:

```toml
[[draw_distance.clamp]]
address  = "0x80061230"
expected = "0x2C4A01C0"  # sltiu t2, v0, 0x1C0
reg      = 2             # v0: the depth index the guard tests
max      = 0x1BF         # last safe index

[[draw_distance.clamp]]
address  = "0x80066FF8"
expected = "0x2441FFFF"  # addiu at, v0, -1 (then sltiu at, at, 0x1BF)
reg      = 2
max      = 0x1BF
```

- While a mod has called `psx_mod_set_draw_distance_clamp(1)`, `reg` is
  clamped to `max` (signed: `if ((int32_t)reg > max) reg = max`) immediately
  before the instruction at `address` runs. The original guard then keeps
  the primitive and the code that follows indexes the table with the clamped
  value. A negative (wrapped) value is left alone, so a guard that also
  rejects too-near primitives keeps doing so.
- Off, the default and the state at every session start, the sites run the
  original code. Netplay never activates a mod, so it stays vanilla.
- `expected` is the complete instruction word and must be an ALU
  instruction (`ADDI`/`ADDIU`/`SLTI`/`SLTIU`/`ANDI`/`ORI`/`XORI`, or a
  SPECIAL shift/arithmetic/logic/`SLT`/`SLTU`) that reads `reg`. `reg` is
  1..31; `max` is a signed 32-bit integer. Addresses are unique by physical
  address.
- Main executable only. Generation fails when the listed word is not
  `expected`, or when the previous instruction loads `reg` (its value would
  still be in the load-delay slot). Captured overlay code at a listed address
  keeps its own code, so the sites do not enter the overlay-cache identity.
  The dirty-RAM interpreter applies a clamp where the game's text image holds
  the listed word.
- Pick `max` so the clamped index reaches no further than the farthest index
  the unclamped code can already produce. Choose the register the following
  code really indexes with: in the `addiu` form above, the guard tests `at`
  but the slot is computed from `v0`.
- Regenerate after changing the list. With no sites the generated code is
  unchanged.

## Runtime block

Consumed by the cmake macro `psxrecomp_v4_add_runtime_target` (eventually)
and by `runtime/src/main.cpp` as the source of compiled-in defaults.

```toml
[runtime]
debug_port    = 4370            # TCP port for the debug server
window_title  = "..."           # SDL window title
controller    = "digital"       # "digital" or "dualshock"
memcard_dir   = "."             # memcard files location, relative to project root
```

### Opt-in warm CD routes

Per-game read acceleration is disabled unless the game explicitly declares
one or more strict routes. Each route arms on one `SetLoc`, then requires every
later file start to match `lbas` in order. A mismatch immediately restores the
normal configured disc timing. Only data-read cadence is accelerated; XA/CDDA,
seek, and motor timing remain authentic.

```toml
[[runtime.warm_cd_routes]]
arm_lba = 95947
lbas = [298, 299, 306]
instant_max_per_frame = 32
```

Up to 16 routes may be declared, with 1–64 LBAs each. The old singular
`[runtime.warm_cd_route]` table is deprecated and emits a warning when loaded;
it remains readable for compatibility. This enhancement is intentionally
opt-in and must not acquire a global default.

The other load-time accelerators are likewise opt-in:

```toml
[runtime]
idle_skip = true
turbo_audio_sink = true
overlay_region_floor = "0x10000"   # optional: lowest RAM address treated as overlay region.
                                    # Default = boot EXE text end. Lower it for titles whose gameplay
                                    # code loads at/inside the boot text range (GT1 secondary EXEs at
                                    # 0x80010000, Driver 2 mission pages) so it is overlay-cache
                                    # eligible. Clamped >= 0x10000; PSX_OVERLAY_REGION_FLOOR overrides.
```

### `turbo_loads` / `offer_turbo_loads` — deprecated and ignored

**Do not use these keys.** Load acceleration is owned by the Mods catalog:
`psx.enhancement.fast-loading` ("Fast Loading (host pacing)") and
`psx.enhancement.cd-speed`. Both target `game_id = "*"`, so they ship with every
title, both default to off, and both expose the multiplier and instant-scheduler
detail that a single opaque boolean never could. recomp-ui correspondingly draws
no generic Turbo loads row.

Both keys are still parsed so existing configs load without error, but neither is
honoured — the runtime logs a deprecation line naming the Fast Loading mod and
leaves acceleration off. Remove them from `game.toml`.

The same applies to `[video] turbo_loads` in a user's `settings.toml`: it is no
longer restored at startup, and it is no longer written back out, so the stale
row disappears on the first save after updating. This is deliberate. Because the
launcher stopped drawing a control for it, a persisted `true` was simultaneously
authoritative and unreachable: one run of a build whose `game.toml` said `true`
latched turbo on permanently, and no later config change could undo it. That
shipped to players in MegaManX6Recomp v1.0.4/v1.0.5 (MegaManX6Recomp#14). Never
restore this row without also restoring a UI control for it.

For development, the `turbo_loads` TCP debug command still toggles acceleration
at runtime.

`turbo_audio_sink` is meaningful only while load acceleration is active. It keeps
the guest SPU timeline advancing but discards accelerated samples before host
playback, then fades normal output back in.

## Audio Block

Game projects may choose the host playback cushion after validating their
audio production cadence:

```toml
[audio]
buffer_ms = 60
```

`buffer_ms` accepts 30–500 milliseconds and defaults to 180. Lower values
reduce audible input-to-sound delay, but leave less reserve for frames where a
game temporarily produces no audio and can therefore crackle on affected
titles. This is deliberately a per-game developer choice; it is not read from
the player's `settings.toml`.

## Video Block

Runtime video defaults live in `[video]`:

```toml
[video]
renderer = "opengl"       # "software", "opengl", or "vulkan"
offer_vulkan = false      # show Vulkan in the launcher only after game validation
auto_skip_fmv = false     # legacy Settings/runtime default
offer_skip_fmv = true     # false when the game exposes this through Mods
```

`renderer = "vulkan"` remains an experimental runtime choice and still requires
a build compiled with Vulkan support. `offer_vulkan` controls launcher
visibility only; it defaults to false so game projects must explicitly expose
Vulkan after validating their visuals and stability.

### Internal resolution (`internal_resolution`, `supersampling`)

The player picks a preset in Settings → Display → **Internal resolution**:
Native, 720p, 1080p, 1440p, 4K, 5K, 8K or Match display. It is stored in the
player's `settings.toml`, and a game may ship a default the same way:

```toml
[video]
internal_resolution = "4k"          # native | 720p | 1080p | 1440p | 4k | 5k | 8k | display, or a number of lines
resolution_reference_lines = 240    # game.toml only: the title's usual display height (120..1024)
```

A preset is a target height. The runtime renders at the integer scale
`S = ceil(target / resolution_reference_lines)`: at the default 240 lines,
720p is 3x, 1080p is 5x (1200 lines, area-resolved to 1080), 1440p 6x, 4K 9x,
5K 12x and 8K 18x. A 480-line interlaced screen renders at twice the target
and is resolved down. **Match display** takes the monitor's pixel height,
measured from the game window's display when it opens. The backend clamps S
to what it can allocate (below); `video_info` over TCP reports both numbers.

Precedence: the game's `internal_resolution` is the default; a player's legacy
`supersampling` in `settings.toml` outranks it; the player's own
`internal_resolution` outranks both. The launcher writes the preset as a stable
id (`"4k"`) beside `supersampling = min(S, 4)`, so an older runtime reading the
same file still gets the nearest factor it supports. A legacy factor with no
matching preset appears in the launcher as its own entry, for example
"2x (480 lines)". `settings.toml` `window_width` accepts 640 to 7680.

A host built against a recomp-ui without the Internal resolution row keeps
the legacy Supersampling row (1x to 4x) and that row stays in charge: with no
preset configured the launcher round trip is exactly the historical one, and
no `internal_resolution` key is written. A preset from `game.toml` or a
hand-edited `settings.toml` starts the row on the nearest factor it can show
and survives if the row is left alone; picking a different factor there
drops the preset.

`PSX_INTERNAL_RESOLUTION=<preset or lines>` overrides every layer for one
run (validation, and two local netplay peers that share one `settings.toml`).
It is never written to `settings.toml`: the launcher shows and saves the
configured preset.

On OpenGL, any choice above native opens the game window with a
high-pixel-density drawable (macOS Retina, Wayland scaling), so the window has
the pixels to show it. Native keeps the window exactly as before.

`supersampling = N` renders at N times the native resolution per axis and
downsamples to the window. It accepts 1 to 32 in both `game.toml` and the
player's `settings.toml`; 1 (the default) is native and unchanged.

The runtime clamps N per backend:

- Software and Vulkan stop at 4.
- OpenGL keeps VRAM as one `1024*N x 512*N` surface and clamps N at context
  init to the driver's `GL_MAX_TEXTURE_SIZE`, `GL_MAX_RENDERBUFFER_SIZE` and
  `GL_MAX_VIEWPORT_DIMS`, and to a memory budget of 2 GiB for that surface
  (`PSX_GL_VRAM_BUDGET_MB` overrides it; `0` removes it). Apple's OpenGL
  reports 16384, so 16 is the largest full-VRAM scale there. A surface that
  still fails to allocate is retried one scale lower; the backend never drops
  to software because of the scale. The log line
  `GL internal scale Nx clamped to Mx (...)` names the limit that applied.

**Past the full-VRAM limit** (8K is 18x, over Apple's 16384), OpenGL keeps
the whole VRAM at 1x — the native renderer, so everything the game reads back
is exactly native — and renders only the columns it displays at the full
scale, in a separate high-resolution window (`5760x9216` for a 320-wide
game at 18x, about 405 MiB). The window grows to cover each displayed
rectangle the first time it is shown; when the displayed rectangles are too
far apart for one surface (side-by-side 512-wide buffers), it splits into up
to four tiles so every buffer still presents at the full scale. A copy whose
source lies outside it is taken from the 1x surface. `PSX_GL_HIRES_WINDOW=0`
turns the mode off (the scale is then clamped as above) and `=1` forces it at
any scale above 1. `PSX_GL_MAX_DIM=N` lowers the GPU limit the backend plans
with, to check a layout on a smaller GPU.

Above 1x the OpenGL present averages the whole footprint of each output pixel
when the internal image is more than 1.25 times larger than the window (for
example 1200 internal lines into a 1080-line window), and lines are drawn one
native pixel thick at any scale.

`offer_skip_fmv` defaults to true for compatibility with the shared PSX
Settings surface. A game migrating Skip FMVs into its built-in mod catalog sets
it to false. The runtime then hides the Settings row, ignores stale persisted
values, and leaves activation to the selected trusted plugin.

### Texture-window batching (`texture_window_batching`, OpenGL)

```toml
[video]
texture_window_batching = true   # game.toml only; default false
```

The OpenGL renderer draws consecutive textured primitives in one batch while
their blend, mask and filter state match. By default a GP0(E2h) texture-window
change also ends the batch, although each vertex carries its primitive's
texture window. With this key on, primitives with different windows share a
batch. The image is the same either way; only the number of draws changes.
While mask checking (GP0(E6h) bit 1) is on, a window change still ends the
batch, because an opaque batch updates the mask bits of its own texels only
after its colour pass.

It is for games that tile textures through per-primitive windows. Ridge Racer
Type 4's split screen changes the window about 515 times a frame, which drew
about 180 batches a frame instead of 17, and every batch that reaches the
native-wide margins is drawn again into the wide surface. The software and
Vulkan renderers ignore the key.
`PSX_GL_TEXWIN_BATCH=0|1` overrides it for one run, and the TCP command
`gl_texwin_batch on=<0|1>` switches it live.

### Local rewind (`settings.toml`)

Rewind is a player setting, not a game one: it lives in the user's
`settings.toml` beside the runtime executable, under `[video]`.

```toml
[video]
rewind          = false   # off by default — see below
rewind_depth    = 50      # snapshots kept: 50 / 100 / 150 / 200
rewind_interval = 15      # frames between snapshots: 1 / 4 / 8 / 12 / 15
```

**`rewind` defaults to `false`.** The ring holds whole *machine* snapshots —
2 MB main RAM + 1 MB VRAM + 512 KB SPU RAM, stored uncompressed — and captures
one every `rewind_interval` frames (denser, toward 4, while an FMV runs). At
the default depth that is a few hundred MB of resident memory plus a periodic
multi-megabyte copy, which is not a cost to charge every host for a feature a
session may never open. Turning it on is one click in the launcher's Display
card; nothing is allocated until it is.

`PSX_REWIND=1` / `PSX_REWIND=0` override the setting either way, and
`PSX_REWIND_DEPTH` / `PSX_REWIND_INTERVAL` / `PSX_REWIND_FMV_INTERVAL` override
the tuning. A build configured with `-DPSX_REWIND=OFF` has no rewind at all and
ignores all of these.

Netplay rollback is a separate subsystem with its own ring and is unaffected by
this setting; rewind is in fact suppressed while a netplay session is active.

Bezel artwork is intentionally not a `[video]` key. It is exposed as the
disabled-by-default `psx.presentation.bezel` mod package, which draws a
user-selected image resource behind the game image in OpenGL letterbox or
pillarbox margins. With the mod disabled, or with no bezel image selected,
margins remain the historical black clear.

Reserved future fields:
- `default_disc_path` — game runtimes can pre-mount a disc
- `default_game_root` — for sibling-junction setups

## Audit block

See `docs/internal/audit_inventory.md` for the audit pipeline. The schema here is
the input side: regions to walk, address-normalisation rules.

```toml
[audit]
function_starts = "generated/ghidra_function_starts.json"   # optional

[[audit.regions]]
name        = "..."             # e.g. "Boot", "Kernel", "Shell", "Text"
rom_start   = "0x..."           # byte offset in rom/exe file
rom_end     = "0x..."           # exclusive
vaddr_base  = "0x..."           # virtual address corresponding to rom_start

[audit.normalize]
kseg_mask = "0x1FFFFFFF"

[[audit.normalize.remap]]
description = "..."             # human-readable; not consumed by tooling
from_lo     = "0x..."           # inclusive
from_hi     = "0x..."           # exclusive
to_lo       = "0x..."           # target start offset (phys = phys - from_lo + to_lo)
```

## What's NOT in the schema yet (Phase B+)

These are noted here so future work knows where to slot them:

- Game `discs` field (Phase D). For now Tomba uses `disc = "..."`.
- `[runtime] disc_swap_command` — runtime-side disc swap (Phase D).
- `[recompiler] seeds` as an array of paths (currently single file;
  Phase A might allow multiple).
- `[program] type` explicit discriminator (currently inferred from
  `rom` vs `exe` field presence).

## BIOS profiles (`bios/<STEM>.toml`)

One profile per BIOS image; the profile is the single source of truth for the
image identity, the relocation windows, and the runtime anchors. Two ship:
`bios/SCPH1001.toml` (retail; user supplies the dump) and `bios/OpenBIOS.toml`
(MIT, redistributable, shipped with the build). Normal runtimes link both
generated backends (`PSXRECOMP_BIOS_STEMS=OpenBIOS;SCPH1001`) and select one at
launch. The recompiler's `[recompiler] bios_config` identifies the profile used
for game code generation; it does not choose the player's runtime BIOS.

```toml
[program.image]              # identity; recompiler refuses a mismatched ROM
sha256          = "..."      # pins the exact image (empty = unchecked)
redistributable = false      # true: BIOS ships with the game; runtime hides
                             # any requirement to provide that image

[recompiler.address_model]   # boot-time bulk code copies out of ROM
normalize_mask = "0x1FFFFFFF"
[[recompiler.address_model.copy]]
name         = "Kernel Part 2"   # comment label in the emitted C
rom_lo       = "0x1FC10000"      # [lo, hi) physical, hi EXCLUSIVE
rom_hi       = "0x1FC18000"
ram_lo       = "0x00000500"      # physical RAM destination
runtime_base = "0x00000500"      # vaddr the CPU executes the copy at
dispatch_key = "ram"             # "ram": functions keyed by RAM address;
                                 # "rom": RAM alias folds back to ROM
kernel_bless = true              # runtime may byte-verify + run native

[[recompiler.install_slots]] # kernel-RAM RANGES patched at runtime
ram_addr = "0x00000CF0"          # legacy form: len 0x10, resume "jalr"
[[recompiler.install_slots]]
ram_addr = "0x00000C88"
len      = "0x30"                # bytes; the patched range is [addr, addr+len)
resume   = "fallthrough"         # "jalr" (default) | "fallthrough" | "none"

[recompiler.runtime_exports] # per-image HLE anchors (omit = unavailable)
shell_entry_phys  = "0x00030000"
deliver_event_ret = "0x80001720"
```

An `install_slots` entry declares kernel-RAM words the guest is EXPECTED to
overwrite at runtime — the BIOS's own install stubs and, far more often, the
Psy-Q libapi patchers every SDK title runs (`_patch_gte`, `_patch_card`,
`_patch_card2`, `_patch_pad`). The emitter plants a compare-against-ROM hook
at the range start, so a live patch dispatches into the interpreter and the
guest's own instructions execute; the runtime excludes the range from the
kernel-bless memcmp and resumes native at the range end. Without the
declaration the patched body fails verification forever and interprets for
the life of the process.

`resume` says how the compiled body picks up again:

| `resume` | Continuation PC | Use for |
|---|---|---|
| `"jalr"` (default) | `ram_addr + 0x10` | the classic 4-word `lui/addiu/jalr/nop` stub, whose call returns there |
| `"fallthrough"` | `ram_addr + len` | the patched words ARE the function (a prologue rewrite, a NOP'd routine) |
| `"none"` | — | the patch jumps out and never returns to this body (a `jr` into game text) |

`ram_addr` and `len` must be 4-aligned, `len` non-zero, ranges must not
overlap, and every range must lie inside the `kernel_bless` window; the
loader refuses the profile otherwise and sorts the list for the runtime.
Finding the ranges for a new image is described in
[`dynamic_handler_install.md`](dynamic_handler_install.md).

Every `copy` entry is a claim that the boot copy is byte-verbatim; the
runtime kernel-bless memcmp enforces it, minus the declared install-slot
ranges. A BIOS with no copies (runs
entirely from ROM) is valid: normalization degenerates to the KSEG mask.
Semantic invariants (disjoint windows, no fold-output/input intersection,
single bless window) are enforced at load; violations refuse to build.
