# Releasing a title

One framework command cuts every player release, for every title, on Windows
and Linux. A title keeps only its release config, `packaging/release.toml`.
Release fixes land here once instead of in a forked packager per title.

## Commands

From the title repository root, with submodules checked out at their pins:

```powershell
# Windows host: Windows ZIP, Linux AppImage (through WSL), or both
psxrecomp/tools/package_title_release.ps1 --channel public   --platform all
psxrecomp/tools/package_title_release.ps1 --channel playtest --platform all
```

```sh
# Native Linux host: the AppImage
psxrecomp/tools/package_title_release.sh --channel public
psxrecomp/tools/package_title_release.sh --channel playtest
```

`--channel` is required and has no default. Without it the command fails at
once with `PACKAGING FAILED`. Artifacts land in `dist/`:

| Channel | Diagnostics default | Windows | Linux |
|---|---|---|---|
| `public` | off | `<Name>-v<ver>-windows-x64.zip` | `<Name>-v<ver>-linux-x86_64.AppImage` |
| `playtest` | on | `<Name>-v<ver>-playtest-windows-x64.zip` | `<Name>-v<ver>-playtest-linux-x86_64.AppImage` |

Each artifact has a `.sha256` file beside it. `RELEASE_MANIFEST.json` inside
the package records the channel, version, title/framework/recomp-ui commits,
mod list, imports or glibc, and a hash for every file. The command never tags,
pushes or uploads.

Other options: `--jobs N` (default `min(6, cores-2)`), `--out-dir`,
`--work-dir` (default `build-package/<platform>`), `--toolchain-bin`
(Windows MinGW, default `C:\msys64\mingw64\bin`), and `--allow-dirty`.
Without `--allow-dirty`, uncommitted tracked changes, including a submodule
moved off its pin, are refused. With it they are recorded in the manifest, so
that package must not be published.

`--recompiler-build-dir PATH` lets serial title builds reuse one native emitter
build directory. Each invocation still configures it against the current
framework and builds both emitter targets with the selected absolute tools.
Use a separate directory per host/platform/toolchain. This option never shares
title runtime objects, PGO profiles or per-title `--work-dir` data. A WSL build
uses its own native emitter directory.

`verify --channel C ARTIFACT...` extracts finished artifacts and re-runs every
check. The command already does this after each package.

## What the command does

Any failure ends with `PACKAGING FAILED: <reason>` and a non-zero exit code.
No step warns and then continues.

1. **Provenance.** Reads the title, framework and recomp-ui commits and checks
   for tracked changes.
2. **Emitters.** Builds `psxrecomp-game` and `psxrecomp-bios` before any slow
   step. The overlay toolchain needs both.
3. **Sources.** Regenerates the recompiled OpenBIOS if it is missing, then
   regenerates the game C from the disc. For AOT mode `static`, it also builds
   the static AOT units from the original disc. On Windows, `--platform linux`
   or `all` prepares the sources once there. The WSL build then compiles those
   same audited sources.
4. **Build.** Release, `PSX_DEBUG_TOOLS=OFF`, OpenBIOS-only backend
   (`PSXRECOMP_BIOS_STEMS=OpenBIOS`), `PSX_RELEASE_CHANNEL=<channel>`, and
   `PSX_GAME_VERSION` from the title's version file. Every flag the command
   owns is read back from `CMakeCache.txt`. A title's `cmake_args` may not set
   any of them. Native GNU C/C++ with Ninja defaults to the qualified selective
   PGO workflow: baseline, generate, one muted hidden boot, then strict use.
   Only the three shared runtime helpers are instrumented; generate/use rebuild
   those helpers and relink. Other toolchains and cross builds retain an
   explicitly recorded portable unprofiled build.
5. **Stage.** Stages these items:
   - the executable and its version stamp
   - launcher assets
   - OpenBIOS and its MIT notice
   - `game.toml` and `game_options.toml`
   - the mod catalog, checked against the build's own catalog list.
     Developer-channel features are removed from public packages.
   - for AOT mode `cache`, the original-disc AOT cache with
     `AOT_CACHE_AUDIT.json`
   - the overlay toolchain, if the title ships it
   - framework, recomp-ui and third-party notices
   - the title's files
   - `START_HERE.txt` with `@VERSION@` filled in. A template without the
     token is an error.
   - the execution contract
6. **Gates.** Refuses the package if it contains any of these: disc images,
   retail BIOS images, `.bin` files other than `bios/openbios.bin`, saves,
   memory cards, save states, personal settings or mod state, generated or
   other C sources outside `overlay_toolchain/`, or build-kit trees. It also
   checks that the Playtest Diagnostics defaults match the channel, and runs
   `tools/ci/audit_playtest_diagnostics_package.py --mode <channel>`. On
   Windows the executable must import only system DLLs. For AOT mode `static`,
   every audited unit must match its receipt and be linked into the executable.
   A diagnostics package that the config does not declare is an error.
7. **Package and verify.** Writes a Windows ZIP with portable `/` names, or
   builds a Linux AppImage with `tools/package_appimage.sh`. The AppImage uses
   fixed linuxdeploy `1-alpha-20251107-1` and appimagetool `1.9.1`, checked by
   SHA-256. The command writes the `.sha256` file, then extracts the artifact
   and verifies it.

Linux needs `cmake`, `ninja` (or make), `gcc`, Python 3.11+, ImageMagick,
`curl`, `file` and the SDL3/OpenGL development packages. On a Windows host,
the Linux half runs inside the default WSL distribution, using the same
checkout through `/mnt/<drive>`. The build tree stays on that drive. Only the
payload, AppDir and verification extraction use `/tmp`, and they are removed
afterwards.

## The backstop: no package without a channel

`runtime/psx_release_channel.cmake` makes `PSX_RELEASE_CHANNEL`
(`public` | `playtest`) the only release input. `PSX_PUBLIC_RELEASE` is now
derived from it. Configure fails in these cases:

- a channel other than `public` or `playtest`
- `-DPSX_PUBLIC_RELEASE=ON` without a channel (the old way to ask for a public
  build)
- a `PSX_PUBLIC_RELEASE` value that contradicts the channel

A tree configured with no channel is a dev build. The packager configures its
own tree with the channel, then reads it back before staging.

## `packaging/release.toml`

Unknown keys are errors, so a typo cannot silently drop a step.

```toml
schema = 1

[product]
name = "JerseyDevilRecomp"            # artifact name prefix (and AppImage data dir token)
display_name = "Jersey Devil Recompiled"
exe_name = "JerseyDevil_Recompiled"   # staged executable; default = CMake OUTPUT_NAME
version_file = "packaging/release/VERSION"   # raw text = PSX_GAME_VERSION and @VERSION@
# game_toml = "game.toml"             # codegen config (default)
# player_game_toml = "game.toml"      # shipped config (default = game_toml)
# runtime_target = "psx-runtime"
# zip_folder = "MediEvilIIRecomp"     # optional single top-level ZIP folder
launcher_boxart = "launcher_assets/img/boxart.tga"   # staged copy must match

[build]
cmake_args = ["-DPSX_SDL_BACKEND=SDL3", "-DPSX_PGXP_VARIANT=OFF"]
# windows_cmake_args / linux_cmake_args: per-platform extras
# regenerate = true                   # psxrecomp-game --config game.toml first
# binary_identity = "{version}+{commit12}"   # bytes the executable must contain
# selective_pgo = true                # default for supported native GNU/Ninja
# pgo_train_seconds = 10               # one real boot, integer 1..300

[aot]
mode = "cache"        # cache: ship the original-disc AOT cache
                      # static: compile audited units into the exe
                      # none
profile = "aot/overlays.json"
# cps = true          # default: true for static, false for cache

[overlay_toolchain]
ship = true           # bundle the runtime overlay compiler

[diagnostics]         # required if the catalog has a *.diagnostics package
game_id = "SCUS-94907"
package_id = "jerseydevil.diagnostics"
# replay_helper = false   # title records with its own tool (no replay-diagnostics.py)

[payload]
start_here = "packaging/release/START_HERE.txt"   # must contain @VERSION@
files = ["README.md", { from = "packaging/release/input.ini", to = "input.ini" }]
# build_files = ["..."]   # extra files from beside the built executable

[linux]
app_run = "packaging/linux/AppRun"    # @VERSION@ @APP_NAME@ @EXE_NAME@ @PAYLOAD_DIR@
                                      # @ENV_PREFIX@ @ARTIFACT_NAME@ are filled in
desktop_file = "packaging/linux/io.github.mstan.JerseyDevilRecomp.desktop"
icon = "launcher_assets/img/boxart.tga"   # PNG, or converted with ImageMagick
payload_dir = "jerseydevilrecomp"         # usr/share/<payload_dir>
env_prefix = "JERSEYDEVIL_RECOMP"
# prune_unreferenced_libs = true      # drop usr/lib entries the exe does not load

# Optional title steps, run at prepare | post_build | post_stage. Tokens:
# {python} {root} {framework} {channel} {platform} {version} {build_dir}
# {work_dir}, plus {exe} (post_build) and {stage} (post_stage). A non-zero
# exit fails the release.
# [[hooks.post_stage]]
# command = ["{python}", "tools/check_my_payload.py", "{stage}"]
# platforms = ["windows", "linux"]
```

Selective PGO is a release **build step**, separate from gameplay regression
tests. Each title gets fresh profiles from its own instrumented production
binary and normal shutdown. Boot-only training does not establish gameplay
correctness, representative coverage or a performance gain for that title.
The strict [shared workflow](performance/runtime-hot-pgo.md) verifies the exact
three source/object commands, actual headers, compiler and configuration, and
rejects missing or incompatible profiles. It keeps DEBUG and IPO off and global
`PSX_PGO` empty. Failed training never silently becomes an unprofiled release.

Training uses a hidden normal SDL presenter, dummy host audio and private
memory cards, data/cache and temporary paths. On Windows it sends `WM_CLOSE`
only to the owned SDL window; on POSIX the enabled SDL signal handler converts
the owned process's `SIGTERM` to a quit event. Only a normal exit and all three
valid profiles permit the use build. No owner saves or desktop input are used.
Run packaging under the appropriate shared capacity reservation when automated.

Use a fresh, short `--work-dir` for each profile recipe; Windows GCC profile
paths must stay below its existing path limit. Phase binaries, profiles,
training logs/private data and receipts remain under that build workspace for
review and cleanup. The player archive contains only the final use binary;
`RELEASE_MANIFEST.json` records phase receipt hashes and profile hashes/sizes,
without private build paths or profile blobs. Payload checks reject gcov and
other compiler training files. `selective_pgo = false` explicitly selects the
ordinary build and records that choice. Unsupported compiler/generator or
cross-target combinations record `portable-unprofiled`; this is not a PGO claim.

`aot.mode = "static"` checks `generated/AOT_STATIC_AUDIT.json` in these ways:
every pair is valid, the recipe count matches `expected_records`, the
`game.toml` and profile hashes match, each unit's hash matches its receipt,
and each unit appears in the executable's link line.

## Migrating a title

Rules for every migrated title:

- Delete the forked packagers (`tools/package_release.ps1`,
  `tools/package_appimage.sh`, `tools/package_release.py`, prepare scripts,
  `scripts/package_setup_release.sh`) and their tests.
- Write `packaging/release.toml`.
- Put `@VERSION@` in `START_HERE.txt`.
- Remove any title CMake that forces `PSX_PUBLIC_RELEASE`.
- Pin the latest psxrecomp and recomp-ui.

Prove the migration with both channels on both platforms, then verify the
artifacts. Titles migrated so far: Jersey Devil and MediEvil II. The backlog
is tracked in bead `beads-eio.3.369`.
