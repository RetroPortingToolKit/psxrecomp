# Your first PSXRecomp port

This is one Windows x64 path from your own disc image to a generated game
project. It uses a terminal and an editor. No AI assistant or account is
required. Start with the stock game and OpenBIOS; add mods after you have a
repeatable working baseline.

If you want to play an existing port, choose it from the [game list](../README.md#games)
and follow that game's download and disc-selection instructions instead.

## 1. Install the tools

Install [Git for Windows](https://git-scm.com/downloads/win) and
[Python 3](https://www.python.org/downloads/windows/), available on PATH.
Download the Windows x64 `cmake-clang-v1` toolchain ZIP from
[the toolchain releases](https://github.com/RetroPortingToolKit/RetroPorting-Toolchains/releases/latest)
and extract it to `D:\RecompDev\Toolchain`. Use the folder containing `bin`.
This supplies the compiler, CMake and Ninja for this guide.

Open PowerShell and prepare the session:

```powershell
New-Item -ItemType Directory -Force D:\RecompDev\Temp | Out-Null
$env:TEMP = 'D:\RecompDev\Temp'
$env:TMP = $env:TEMP
$env:PSXRECOMP_TOOLCHAIN_DIR = 'D:\RecompDev\Toolchain'
$env:Path = "D:\RecompDev\Toolchain\bin;$env:Path"
git --version
python --version
cmake --version
ninja --version
```

Replace `D:` with your development drive. Each version check should succeed
before you continue. Keep build output, caches and the extracted toolchain
on that drive.

## 2. Prepare a disc and clone the framework

Use your own supported CUE/BIN dump. Extract archives first and keep the
`.cue` with all of its referenced tracks. Leave the disc outside the source
repository. This example uses `D:\Discs\My Game\game.cue`.

```powershell
Set-Location D:\RecompDev
git clone --recurse-submodules https://github.com/RetroPortingToolKit/psxrecomp.git
Set-Location psxrecomp
```

Check the [existing projects](../README.md#games) before starting another
port of the same game. Their per-disc configurations and validation may give
you a better starting point for a contribution.

## 3. Generate one local game project

Run the setup command with these fixed first-project choices:

```powershell
powershell -File tools\new_project_layout\setup_project.ps1 `
  -Disc 'D:\Discs\My Game\game.cue' -Dir D:\RecompDev `
  -Name MyFirstPSXPort -GithubRepo my-first-psx-port -Players 1 -Yes `
  -EnableRecompUi -NoNetplay -NoFetchBoxart -NoGithub -Generate -EnableBuild
```

The output folder is `D:\RecompDev\my-first-psx-port`. The script probes
the disc, records its identity, creates the framework/UI submodules, generates
the game and open-source OpenBIOS code, then configures and builds. The
GitHub slug above controls the local folder; `-NoGithub` keeps the project
local. You do not need to supply a retail BIOS for this path.

Wait for generation and compilation to succeed. Read errors at the step
where they occur; a created folder alone is not a completed build. Open the
executable at the path printed by setup, select the same disc if asked, and
try a cold boot with the default presentation.

## 4. Make one short route reliable

Your first goal is boot → title menu → first gameplay scene. Compare input,
graphics, sound and transitions with the original game in a trusted emulator.
Record what works and the smallest sequence that reproduces a failure.

The new project owns `game.toml`, symbol/analysis inputs and game-specific
integration. The framework owns the shared hardware runtime. Keep original
disc data outside Git, and fix analysis/configuration or runtime behavior
instead of hand-editing generated functions to force a result.

Games can stream executable overlays from disc. Reaching a title screen does
not establish that later overlays or a full campaign work. Read the
[execution model](EXECUTION_MODEL.md) and [overlay guide](AOT_SHARDING.md)
when the next failing scene needs more code coverage.

## 5. Add enhancements after the baseline

Keep one disc revision and the original game behavior working before adding
widescreen, multiplayer, translations or alternate content. Each feature
needs its own tested contract. Additional discs and content variants need
explicit identities and integration; selecting a different image does not
make it supported.

Use the [project reference](GAME_PROJECT_SETUP.md) for regeneration and
packaging, and [manual mod authoring](../psxmod.md) for the mod system. Include
the required license notices when distributing a build. The
[full build reference](BUILDING.md) covers other host platforms and advanced
toolchain choices.
