# Bringing up a game project on PSXRecomp

This guide is for **title developers**: you keep game code and config in your
own repository, pin this framework and `recomp-ui` as **root-level submodules**,
commit the recompiled game C under `generated/`, and ship a **bundled** zip:
the compiled game, built by CI from that committed C. Players bring their own
disc and play; nothing is generated or built on their machine
(`docs/ci/BUNDLED_RELEASES.md`).

Related docs:


| Doc                                            | When                                                                     |
| ---------------------------------------------- | ------------------------------------------------------------------------ |
| `[BUILDING.md](BUILDING.md)`                   | Local build of the framework / BIOS runtime                              |
| `[LOCAL_CODEGEN_SDK.md](LOCAL_CODEGEN_SDK.md)` | CLI contract for Generate & rebuild                                      |
| `[NETPLAY.md](NETPLAY.md)`                     | **Read more about netplay** (rollback, SFU/ICE, dual-raster, disc gates) |
| `[ci/README.md](ci/README.md)`                 | Shared CI scripts and composite actions                                  |
| `[CONTRIBUTING.md](../CONTRIBUTING.md)`        | Framework contribution rules                                             |


---

## New Project Layout (preview)

The preferred title-repo shape is **root-level** `psxrecomp/` + `recomp-ui/`
submodules. Scaffolding scripts create that layout, pin submodule SHAs, fill
CI + packager stubs, and (with `--disc`) autofill identity, TOC fingerprint,
and a first-pass seed list from the boot EXE.


| Platform                | Script                                                                                        |
| ----------------------- | --------------------------------------------------------------------------------------------- |
| Linux / macOS           | `[tools/new_project_layout/setup_project.sh](../tools/new_project_layout/setup_project.sh)`   |
| Windows                 | `[tools/new_project_layout/setup_project.ps1](../tools/new_project_layout/setup_project.ps1)` |
| Disc probe (standalone) | `[tools/new_project_layout/probe_disc.py](../tools/new_project_layout/probe_disc.py)`         |
| Boxart fetch            | `[tools/new_project_layout/fetch_boxart.py](../tools/new_project_layout/fetch_boxart.py)`     |
| Token fill helper       | `[tools/new_project_layout/fill_tokens.py](../tools/new_project_layout/fill_tokens.py)`       |


### Scaffold flow (paths on CLI, rest prompted)

**Required path flag** (use your shell’s tab-completion): `--disc` / `-Disc`
pointing at a legal Redump `.cue`. Missing `--disc` exits with a nag — no
prompt for that path.

```bash
# Interactive (TTY): prompts for name, players, zip prefix, recomp-ui, …
sh tools/new_project_layout/setup_project.sh \
  --disc /path/to/legal/game.cue \
  --dir ~/src \
  --bios /path/to/SCPH1001.BIN   # optional; only used if you answer Y to Generate
```

```powershell
powershell -File tools\new_project_layout\setup_project.ps1 `
  -Disc C:\dumps\game.cue -Dir C:\src
```

Non-interactive / CI (`--yes` / `-Yes` or `PSXRECOMP_SETUP_YES=1`): requires
`--name` + `--disc`; Y/N options default **off** unless you pass enable flags.


| Path / CLI         | Role                                                          |
| ------------------ | ------------------------------------------------------------- |
| `--disc` / `-Disc` | **Required.** Stage cue+bins, probe identity / seeds / TOC fp |
| `--dir` / `-Dir`   | Parent directory for the new repo (default `.`)               |
| `--bios` / `-Bios` | Optional `SCPH1001.BIN` when Generate runs                    |
| `--boot-exe`       | Optional until probe overwrites from `SYSTEM.CNF`             |



| Prompted (or flag override) | Notes |
| --------------------------- | ----- |
| Project name | Required; `--name` / `-Name` skips prompt |
| Players | Default **2**, max **8** |
| Zip / CI prefix | Default acronym from name (e.g. MotK → `motk`) |
| Description / publisher / year / region | Optional marketing → `catalog_identity.json` + README |
| Include **recomp-ui**? | **N** → no `recomp-ui` submodule, `PSX_RECOMP_UI=OFF`, skip wizard/netplay |
| Setup wizard? / Netplay? | Only asked if recomp-ui = Y; **netplay auto-off (no prompts) when players = 1** |
| Netplay lobby URL? | Only if netplay = Y (players ≥ 2); default host `netplay.retcomm.net` → `ws://…:8765` |
| GitHub Actions release workflow? | Y → `.github/workflows/release.yml` (logs submodule SHAs via `record_pins.sh`) |
| Fetch libretro boxart? | Needs network |
| Run Generate now? | Emitters + OpenBIOS + game C; `generated/` is **committed** (release CI builds from it) |
| Configure & build after Generate? | Only if Generate = Y |
| Create GitHub repo (`gh`)? | Opt-in; needs `gh` auth |

Flag overrides (non-interactive): `--enable-recomp-ui` / `--no-recomp-ui`,
`--enable-wizard` / `--no-wizard`, `--enable-netplay` / `--no-netplay`,
`--lobby-url`, `--description` / `--publisher` / `--year` / `--region`,
`--enable-ci` / `--no-ci`, `--fetch-boxart` / `--no-fetch-boxart`,
`--generate` / `--no-generate`, `--enable-build` / `--no-build`,
`--create-github` / `--no-github` / `--github-visibility`, … (PowerShell:
`-EnableRecompUi`, `-LobbyUrl`, `-EnableBuild`, `-CreateGithub`, …).

What runs after answers:

1. Create repo stubs (`CMakeLists.txt`, `game.toml`, codegen, rich `.gitignore`,
   `symbols.toml`, `README`, empty `mods/preloaded/packages/`, …) + copy
   `tools/sync_symbols.py`
2. Submodule `psxrecomp` (+ `recomp-ui` only if accepted); detach at fetched
   SHAs; optional snapshot → `framework_pins.txt`
3. Packager stub; optional CI workflow (submodule gitlinks are the pin;
   `record_pins.sh` logs SHAs)
4. Stage disc → `probe_disc.py` (marketing + seeds) → `psx_symbols.h` →
   optional boxart → initial commit → optional `gh repo create` (no push) →
   optional Generate → optional cmake/ninja build → single `git push`. If
   Actions has not indexed `release.yml` (common after a private first push),
   the scaffold force-publishes when visibility is public and nudges with a
   tiny follow-up commit that retouches the workflow file.

Standalone helpers: `probe_disc.py`, `fetch_boxart.py`, `fill_tokens.py`,
`sync_symbols.py`.

### Disc autofill (`probe_disc.py`)

With a Redump-style `**.cue**`:


| Output                   | Contents                                                                                                                                               |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `game.toml`              | Serial / boot EXE / EXE header; `[prepare_disc]` Track 01 digests; `[netplay] require_cue`, `required_tracks`, `required_disc_fp` (`psxrecomp-toc-v1`) |
| `seeds/ghidra_funcs.txt` | Entry PC + direct **JAL targets** inside the boot EXE text (first-pass; MotK-class ~850 addrs)                                                         |
| `catalog_identity.json`  | Digests + `track_counts` + `disc_fp` + optional `marketing` (description / publisher / year / region / players) |
| `disc_probe.json`        | Full probe dump |
| `.recomp.json`           | Community metadata ([recomp.fyi spec](https://recomp.fyi/spec)): game, system, type, toolchain, `status: exploring`, and `original` region + serial. Written with or without `--disc`; region comes from the serial prefix when known. `write_recomp_json.py` only fills missing fields, so re-running it never overwrites your edits |
| `symbols.toml` + `psx_symbols.h` | Boot entry stub + `PSX_FN_*` header (`tools/sync_symbols.py`; see [`SYMBOLS.md`](SYMBOLS.md)) |


Re-run without scaffolding:

```bash
python3 tools/new_project_layout/probe_disc.py disc/game.cue \
  --write-game-toml game.toml \
  --write-catalog catalog_identity.json \
  --write-seeds seeds/ghidra_funcs.txt \
  --disc-rel disc/game.cue --out-dir disc --players 2
```

Prefer a **full multi-track** Redump cue. A single-TRACK dump will warn and will
fail online multi-track gates (see `[NETPLAY.md](NETPLAY.md)`).

### EXE-only autofill (`psxrecomp-toml`)

When you have a PS-X EXE but no cue — or you just want a `game.toml` skeleton
without running the full disc probe — `psxrecomp-toml` (built alongside the
other CLI tools from `recompiler/`, source `recompiler/src/main_toml.cpp`)
reads the EXE header and writes the config for you. It saves opening a hex
editor or Ghidra just to recover the entry point, load address and text size.

What it produces:

- **`game.toml`** — `load_address`, `entry_pc` and `text_size` taken from the
  actual PS-X EXE header, plus the game name and ID.
- **Seeds** — the destinations of every direct jump-and-link (`JAL`) it finds
  in the code segment, optionally widened with the addresses immediately
  following each `jr $ra`.

```
Usage: psxrecomp-toml <PS1-EXE> [options]

Options:
  --output <path>   Write game.toml to <path> (default: stdout)
  --seeds <path>    Also write JAL-target seed file to <path>
  --name <str>      Game name in TOML (default: derived from EXE)
  --id <str>        Game ID (default: auto-detect or empty)
  --stdout          Force output to stdout even with --output
  --include-after-return
                    Add addresses after jr $ra to seeds (more coverage,
                    may include some data addresses)
  -h, --help        Show this help
```

Example:

```bash
psxrecomp-toml ./isos/SCES_028.34 \
  --output ./projects/CrashBash/game.toml \
  --seeds ./projects/CrashBash/recompiler/seeds/seeds.txt \
  --name "Crash-Bash-EUR"
```

It also prints the EXE, game ID, entry PC, load address, text size, stack base
and seed count to stderr, so you can sanity-check the header it read.

`--include-after-return` trades precision for coverage: an address just past a
`jr $ra` is usually the next function, but sometimes it is data, so those seeds
can introduce junk entries.

**This is a first pass, not full discovery.** Static `JAL` scanning cannot
resolve dynamic function tables or indirect register dispatches. When the
recompiler later reports discovery gaps or unknown dispatches at runtime, those
newly-found addresses still have to be fed back into the seeds file. Prefer
`probe_disc.py` above when you have a cue — it does the same header work plus
disc identity, digests and netplay gates.

### Project Studio — migrate / update existing titles

Older titles (e.g. `psxrecomp-v4` submodule, `psxrecomp_add_runtime_target`,
`packaging/` prebuilt zips) can be audited and migrated onto this layout with
**Project Studio**. Releases are **bundled**: the tool un-ignores and expects
committed `generated/`, and emits the packager + CI that ship the compiled game.

| Entry | Role |
| ----- | ---- |
| `tools/new_project_layout/migrate_project.py` | CLI: `audit` / `plan` / `apply` / `gui` / `ops` |
| `tools/new_project_layout/project_studio_gui.py` | Tkinter GUI |
| `tools/new_project_layout/project_studio/` | Shared detect / plan / ops library |
| `tools/new_project_layout/README.md` | Short usage |

```bash
# From a psxrecomp checkout (or any title that vendors this toolkit path):
python3 tools/new_project_layout/migrate_project.py audit --root ~/src/ApeEscapeRecomp
python3 tools/new_project_layout/migrate_project.py apply --root ~/src/ApeEscapeRecomp --dry-run
python3 tools/new_project_layout/migrate_project.py gui
```

Typical apply order: rename `psxrecomp-v4` → emit `codegen_setup` → rewrite
CMake (`psxrecomp_add_game_runtime` + wizard) → bundled packager + CI →
optional `probe_disc` / pins. CMake rewrites keep
`CMakeLists.txt.pre_migrate.bak` plus `CMakeLists.migrate_extras.txt` when the
old file had tests / `EXTRAS_SOURCES` / mod `POST_BUILD` hooks.

`apply` always enables **recomp-ui + setup wizard** (the shipped game's
first-run disc picker). Netplay stays opt-in (`--enable-netplay` / GUI).

### After scaffold (still not automatic)

If you answered Y to boxart + Generate (+ optional build), you get a local
playable tree (OpenBIOS / optional retail BIOS C). You still must:

1. **Boot / soak** — fix missing seeds, overlays, FMV/runtime quirks in `game.toml`
2. **Netplay QA** — LAN then lobby; confirm digests + TOC fp; pin `VERSION`
3. **Polish** — more symbols in `symbols.toml`, boxart name mismatches
4. **Ship** — commit `generated/` (the scaffold does this after a successful
   Generate), push, enable Actions, tag `vX.Y.Z`. CI builds the committed C and
   attaches the compiled game for every platform.

**Legacy layout** (CLI `psxrecomp build`, nested UI under older trees,
`tools/setup_dev.sh` for framework-only boots) remains supported. Prefer the
New Project Layout for new titles until the project fully adopts it.

---

## Opt-in launcher features (build flags)

Both surfaces are **OFF by default**. The New Project Layout can enable them
when you accept **recomp-ui** and answer Y to wizard/netplay (or pass
`--enable-wizard` / `--enable-netplay`). Declining recomp-ui sets
`PSX_RECOMP_UI=OFF` and skips those prompts. You can also set the flags in
`CMakeLists.txt` before `include(runtime.cmake)`, or pass `-D…=ON`. Do not
enable them until tested — other recomp-ui consumers stay dark when `GameInfo`
is zero-init.


| CMake option       | Default | Effect                                                                                        |
| ------------------ | ------- | --------------------------------------------------------------------------------------------- |
| `PSX_NETPLAY`      | OFF     | Link `recomp-net` + lobby client; advertise **full netplay UI** when the title has 2+ players. Also defaults `RNET_ENABLE_ICE=ON` (libjuice via URL FetchContent, or `third_party/libjuice` / `RNET_LIBJUICE_ROOT`) unless `-DRNET_ENABLE_ICE=OFF`. |
| `PSX_SETUP_WIZARD` | OFF     | Advertise **first-run setup wizard + Generate & rebuild** (`GameInfo.setup_wizard_supported`) |


`psxrecomp_add_game_runtime` helpers:

- `ENABLE_NETPLAY_IF_PRESENT` — *supplemental* only; still set `PSX_NETPLAY ON`
  **before** `include(runtime.cmake)` when `lib/recomp-net` exists (otherwise
  `option(PSX_NETPLAY)` stays OFF and `psx_netplay_sched.c` cannot find
  `recomp_net/session.h`)
- `NETPLAY_LOBBY_URL "ws://host:port"` — compile-time default lobby URL
(`PSX_NET_LOBBY_DEFAULT_URL`; env `PSX_NET_LOBBY_URL` still wins at runtime)
- `ENABLE_SETUP_WIZARD` — force `PSX_SETUP_WIZARD=ON` for that target
- `PGXP` — build the title's one runtime as the PGXP hook flavor
  (`-DPSX_PGXP=1`, overlay cache flavor `_f2`, no `_pgxp` suffix). The generated
  C already carries the hook sites, so `generated/` does not change. PGXP stays
  off at runtime until `psx.enhancement.pgxp` or the `[video]` keys arm it; with
  it off the guest runs exactly as in the base flavor. Savestates of the two
  flavors are not interchangeable. A title that ships PGXP on by default does so
  through a default-on override of `psx.enhancement.pgxp` plus
  `[video] pgxp_mod_only = true`. See `ENHANCEMENTS.md` G1.11/G1.12.

Minimal opt-in example:

```cmake
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp")
if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
    set(PSX_NETPLAY ON CACHE BOOL "…" FORCE)
endif()
set(PSX_SETUP_WIZARD ON CACHE BOOL "…" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")
psxrecomp_add_game_runtime(psx-runtime
  ENABLE_NETPLAY_IF_PRESENT
  NETPLAY_LOBBY_URL "ws://netplay.retcomm.net:8765"
  ENABLE_SETUP_WIZARD
  …
)
```

Without `PSX_SETUP_WIZARD`, recomp-ui never opens the first-run modal and never
shows Generate & rebuild — even if prepare callbacks are present. Without
`PSX_NETPLAY`, the NETPLAY button stays hidden.

**Read more about netplay:** `[NETPLAY.md](NETPLAY.md)` — rollback, hybrid
OpenGL+1× SW authority, ICE/TURN, SFU online vs LAN star, multitap, and
multi-track `.cue`/`.bin` requirements.

---

## Recommended repository layout

Everything game-specific lives at the **root** of *your* title repo. Framework
and UI are submodules next to that code — not nested under each other.

The Retro / Generate & rebuild CLI (`psxrecomp_cli.py`, `tools/prepare_disc.py`,
pack helpers) ships **inside** the `psxrecomp` submodule. There is no separate
`psxrecomp-sdk/` tree.

```text
YourGameRecomp/                 # your git repo
├── .gitmodules
├── CMakeLists.txt              # thin: psxrecomp_add_game_runtime(...)
├── game.toml                   # probe autofills identity / netplay gates
├── catalog_identity.json       # Retro / catalog digests + track_counts + disc_fp
├── .recomp.json                # community metadata (recomp.fyi); keep `status` current
├── framework_pins.txt          # optional scaffold snapshot (gitlinks are authoritative)
├── README.md                   # scaffold stub (legal + quick start)
├── VERSION                     # release pin (e.g. 0.1.0)
├── assets/
│   ├── psxrecomp.ico           # default Windows app icon (Retro-themed pad)
│   ├── psxrecomp.png           # staged beside exe / packaging
│   └── psxrecomp.svg           # source mark (see psxrecomp/assets/)
├── seeds/ghidra_funcs.txt      # probe: boot-EXE JAL seeds (grow over time)
├── codegen_setup.c / .h        # title PsxrecompCodegenHostConfig (+ apply hooks)
├── launcher_assets/img/
│   ├── boxart.tga              # optional: --fetch-boxart (libretro Named_Boxarts)
│   └── BOXART_SOURCE.txt       # attribution URL
├── mods/preloaded/             # scaffold: empty catalog for shipped .psxmod packages
│   ├── README.md               # staged to <exe>/mods/README.md
│   └── packages/               # packages/<id>/<version>/manifest.toml …
│                               # declared to the framework as
│                               #   PRELOADED_MODS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded"
│                               # which stages it into <exe>/mods/bundled beside
│                               # the framework's builtins. NEVER copy it with
│                               # your own POST_BUILD command — see
│                               # docs/MOD_PACKAGES.md ("How bundled/ gets staged").
├── scripts/
│   └── package_release.sh      # scaffold fills from package_release.sh.in
├── .github/workflows/
│   └── release.yml             # scaffold fills from docs/ci/templates/game-release.yml
├── psxrecomp/                  # submodule → framework + CLI + codegen host + CI tools
│   ├── host/psxrecomp_codegen_host.*
│   └── lib/recomp-net/         # nested submodule (netplay)
├── recomp-ui/                  # submodule → launcher UI
├── generated/                  # COMMITTED recompiled game C — release CI builds it
└── disc/                       # local disc working tree — gitignore
```

Minimal `CMakeLists.txt` shape. The wizard is the shipped game's first-run disc
picker; `GEN_MARKER` / `GEN_FULL_GLOB` name the committed `generated/` C, which
release CI requires (`-DPSXRECOMP_REQUIRE_GAME_C=ON`).

```cmake
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp")
# set(PSX_NETPLAY ON CACHE BOOL "…" FORCE)        # multiplayer titles only
set(PSX_SETUP_WIZARD ON CACHE BOOL
    "Advertise first-run setup wizard + Generate & rebuild in recomp-ui" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")
psxrecomp_add_game_runtime(psx-runtime
  # ENABLE_NETPLAY_IF_PRESENT
  ENABLE_SETUP_WIZARD
  WINDOW_TITLE "My Game Recompiled"
  GEN_MARKER "generated/SLUS_01234_dispatch.c"
  GEN_FULL_GLOB "generated/SLUS_01234_full_*.c"
  CODEGEN_SETUP_SOURCES codegen_setup.c
  DEFAULT_GAME_CONFIG_PATH "game.toml"
  LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"
  APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"
)
```

Or use the scaffolding scripts under **New Project Layout (preview)** above.

### Add the submodules

```bash
cd YourGameRecomp
git submodule add -b master https://github.com/mstan/psxrecomp.git psxrecomp
git submodule add -b master https://github.com/RetroPortingToolKit/recomp-ui.git recomp-ui
git submodule update --init --recursive
```

The New Project Layout scaffold **pins** via detached submodule gitlinks
(optional `framework_pins.txt` snapshot for humans/logs). Do not float on
`main`/`master` in release CI — bump submodules deliberately when you want a
newer framework or UI.

Clone for contributors:

```bash
git clone --recurse-submodules https://github.com/you/YourGameRecomp.git
```

---

## Local development loop

After scaffold (or a manual clone):

1. **Legal disc** — under `disc/` or the path in `game.toml` (never commit dumps
  or retail BIOS).
2. **Seeds** — scaffold/`probe_disc.py` writes a first-pass `seeds/ghidra_funcs.txt`
  from the boot EXE. Grow it as overlays / runtime paths miss.
3. **Build emitters** (once per machine / after recompiler changes):
  ```bash
   ./psxrecomp/tools/ci/build_emitters.sh
  ```
4. **Generate game (+ OpenBIOS) C** — setup wizard (if enabled) or:
  ```bash
   python3 psxrecomp/psxrecomp_cli.py generate \
     --config game.toml --project-root . --disc disc/game.cue
  ```
5. **Commit the generated C** — it is the release's input:
  ```bash
   git add generated && git commit -m "Regenerate game C"
  ```
6. **Build the playable runtime**:
  ```bash
   cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build-release --target psx-runtime -j"$(nproc)"
  ```
7. **Soak** — offline boot → LAN netplay → lobby. Tune `[runtime]` quirks only
  after you have evidence (see MotK `game.toml` comments for the pattern).

Details: `[LOCAL_CODEGEN_SDK.md](LOCAL_CODEGEN_SDK.md)`, `[BUILDING.md](BUILDING.md)`,
`[NETPLAY.md](NETPLAY.md)`.

---

## Bundled releases (compiled game zip)

CI builds the committed `generated/` C and ships a zip that is the game:

- The game executable (signed on Windows when a certificate is configured)
- `assets/`, `mods/bundled/` (catalog verified against the build's manifest),
  `game.toml` / `game_options.toml`
- `bios/openbios.bin` + its MIT notice — the release runs on OpenBIOS
- `overlay_toolchain/` so overlays outside the compiled static shard can be
  turned native from the player's disc at runtime
- `licenses/` (third-party notices), `README.txt`

It contains **no** sources, emitters at the root, CLI, `psxrecomp/` or
`recomp-ui/` tree, generated C, disc, or BIOS dump. Players (or
[Retro](https://github.com/RetroPortingToolKit/Retro-Launcher)) extract it,
pick their legal disc on first run, and play; an update is a raw zip extract
over the previous one. Details and the exact CI steps:
[`ci/BUNDLED_RELEASES.md`](ci/BUNDLED_RELEASES.md). CI tip: leave the
`psxrecomp` gitlink pinned and use `reuse_cached_emitters` so release jobs skip
rebuilding emitters when only host sources moved.

Do **not** set `PSX_PGO` in CI. PGO stays user-local when `[pgo] enabled = true`.

---

## Shared tools (use these; do not reimplement)


| Tool                            | Role                                                     |
| ------------------------------- | -------------------------------------------------------- |
| `tools/ci/normalize_version.sh` | `vX.Y.Z` → `VERSION` / `TAG`                             |
| `tools/ci/check_generated.sh`   | Committed game C + framework BIOS backends present       |
| `tools/ci/record_pins.sh`       | Log submodule SHAs                                       |
| `tools/ci/build_emitters.sh`    | Build `psxrecomp-game` + `psxrecomp-bios`                |
| `tools/fetch_toolchain.sh`      | Windows emitter build on the CI machine (never shipped)  |
| `tools/bundle_mingw_dlls.sh`    | Windows runtime DLL copy                                 |
| `tools/package_game_release.sh` | Bundled zip: compiled game + runtime data (title args)   |
| `tools/release_stage.py`        | Mod catalog / overlay toolchain / overlay cache staging  |


Composite actions (from the game repo after checkout):

```yaml
- uses: ./psxrecomp/.github/actions/build-emitters
# Optional — only when packaging with --embed-toolchain:
# - uses: ./psxrecomp/.github/actions/fetch-toolchain
#   with:
#     artifact: ${{ matrix.artifact }}
- uses: ./psxrecomp/.github/actions/stage-setup-sdk
  with:
    stage: dist/stage-setup-${{ matrix.artifact }}
    recompiler-build: build-recompiler
    allow-no-toolchain: 'true'
```

---

## Bundled release checklist

Use this before tagging a release.

### Repository

- [ ] `psxrecomp/` and `recomp-ui/` are root-level submodules on pinned commits
- [ ] CLI lives in the submodule (`psxrecomp/psxrecomp_cli.py`) — no sibling sdk
- [ ] `game.toml` has disc identity (`[prepare_disc]` hashes/sizes) and boot EXE
  ```
  (scaffold `--disc` / `probe_disc.py` autofills these)
  ```
- [ ] Multi-track titles that ship netplay: `[netplay] require_cue` +
  ```
  `required_tracks` + `required_disc_fp` so Track-01-only dumps cannot go
  online against full Redump cues
  ```
- [ ] Optional: commit `catalog_identity.json` (+ `framework_pins.txt` snapshot)
- [ ] `.recomp.json` `status` matches reality (`exploring` → `in-progress` →
  `playable` → `released`, or `paused`) — community lists read it daily, and a
  stale status is worse than none
- [ ] `seeds/ghidra_funcs.txt` covers the boot path (probe JAL pass + discoveries);
  ```
  `VERSION` matches the release you will tag
  ```
- [ ] Disc images, BIOS, logs, and runtime dumps are gitignored; `generated/`
  is **not** (scaffold writes a rich `.gitignore`; see also
  `docs/ci/templates/game.gitignore`)
- [ ] `generated/<boot>_dispatch.c` + `_full_*.c` (+ `overlays_static*.c` when
  the AOT profile declares it) are committed and current for the pinned
  framework (`tools/ci/check_generated.sh` passes locally)
- [ ] The pinned `psxrecomp` commits its BIOS backends (`psxrecomp/generated/
  {OpenBIOS,SCPH1001}_*.c`) with stamps current for its emitter
- [ ] Submodule gitlinks (`psxrecomp` / `recomp-ui` / nested `recomp-net`) are
  ```
  the SHAs you intend to ship (CI builds those; `record_pins.sh` only logs)
  ```
- [ ] Full build configures from the committed C: CI passes
  `-DPSXRECOMP_REQUIRE_GAME_C=ON -DPSX_SETUP_WIZARD=ON` and asserts the
  configure log says `linking generated game C (full runtime)` and
  `BIOS backends linked: OpenBIOS;SCPH1001`
- [ ] `[runtime] overlay_cache = true` in `game.toml` (the packager refuses
  otherwise)
- [ ] Thin `codegen_setup.c` + `psxrecomp_add_game_runtime` (codegen host is in
  `psxrecomp/host/`). Must export `psx_game_codegen_forward_if_built` (see
  `codegen_setup.c.in`) — `main.cpp` always calls it; in a full build it is a
  no-op

### Packaging (shared helpers)

- [ ] CI uses `./psxrecomp/.github/actions/build-emitters` (OpenBIOS emission
  and the shipped overlay toolchain need them)
- [ ] `scripts/package_release.sh` calls `psxrecomp/tools/package_game_release.sh`
- [ ] Staged tree has the game exe, `psx_game_version.txt`, `assets/`,
  `mods/bundled/`, `game.toml`, `bios/openbios.bin` + `OpenBIOS.LICENSE`,
  `overlay_toolchain/`, `licenses/` — and **no** `psxrecomp/`, `recomp-ui/`,
  CLI, root-level emitters, sources, generated C, retail `.BIN`
- [ ] Windows zip has the runtime DLLs the exe imports beside it
  (`bundle_mingw_dlls.sh`), signed when a certificate is configured

### Zip contents smoke test

- [ ] Linux / macOS / Windows artifacts named consistently
  (`YOUR_PREFIX-<ver>-<linux-x64|windows-x64|macos-arm64|macos-x64>.zip`)
- [ ] Extract → run → pick a legal disc → the game boots; saves/settings land
  beside the exe
- [ ] Retro install/update (if you publish a catalog entry) uses this **same**
  zip as a prebuilt Play binary (raw zip extract; no build step)

### Publish

- [ ] Tag `vX.Y.Z`; GitHub Release attaches all four platform zips
- [ ] Release notes say the zip is the compiled game: bring your own disc,
  no BIOS, no build
- [ ] Catalog / Retro entry points at the release assets when ready