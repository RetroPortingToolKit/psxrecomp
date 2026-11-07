# Getting started with PSXRecomp

This guide covers first-time setup on **Windows x64**, from downloading tools
to building a game project from your own disc image. You will use PowerShell
to run commands and a text editor to work on the project. No AI assistant or
account is required. For other platforms and compilers, see the
[build reference](BUILDING.md).

## Choose your starting point

- **Play an existing port:** choose it from the [game list](../README.md#games)
  and follow that game's release and disc-selection instructions.
- **Modify an existing port:** start with that game's source repository and
  build instructions. Its game-specific setup may already cover your disc.
- **Start a new port:** follow the steps below to create a separate game
  project. Start with the stock game and OpenBIOS; add mods after you have a
  repeatable working baseline.

PSXRecomp is a development framework. It translates PS1 code and builds a
native program; changing levels, characters or other game content still needs
game-specific programming, asset tools or reverse engineering.

## What the downloads contain

| Download | What you do with it |
| --- | --- |
| Git for Windows | Run its installer. Git downloads source repositories and their dependencies. It also supplies Bash, used by the build scripts. |
| Python 3 | Run its installer and enable the option to add Python to PATH. Python runs the setup and generation tools. |
| Windows x64 toolchain ZIP | Extract it into a folder you choose. It supplies the compiler, CMake and Ninja used to build programs from source. |
| `psxrecomp-master.zip` from GitHub's **Code > Download ZIP** | This is source code, with no installer or ready-to-run game executable. Use the Git clone command below to obtain the source and its required submodules. |

The toolchain ZIP and psxrecomp source ZIP are separate downloads with
different contents. A game's packaged release has its own instructions and
is different from GitHub's source-code ZIP.

## 1. Choose and create your workspace

In File Explorer, create a folder for your development files. This guide uses
`C:\RecompDev`. **You create this folder yourself**; Git, Python and psxrecomp
do not install it for you. `RecompDev` and `Toolchain` are example names,
and `C:` or `D:` can both be used.

Open **PowerShell** from the Windows Start menu. Paste commands into that
window. Set your workspace path:

```powershell
$RecompRoot = 'C:\RecompDev'
```

If you chose another location, change that value. Keep this PowerShell window
open for the steps below: `$RecompRoot` and the environment settings apply to
the current session. In a new window, repeat this command and the session
settings in step 2.

The completed layout will look like this:

```text
C:\RecompDev\
  Toolchain\             You extract the compiler tools here.
    bin\
      cmake.exe
      ninja.exe
      clang.exe
  psxrecomp\             Git creates this framework source folder in step 3.
  my-first-psx-port\      Setup creates this separate game project in step 4.
```

## 2. Install and check the tools

Install [Git for Windows](https://git-scm.com/downloads/win) and
[Python 3](https://www.python.org/downloads/windows/), making their commands
available on PATH. PATH is the list of folders Windows searches when you type
a command. If you installed them with PowerShell already open, reopen
PowerShell and set `$RecompRoot` again so it sees the updated PATH.

Download **`cmake-clang-v1-windows-x64.zip`** from
[the toolchain releases](https://github.com/RetroPortingToolKit/RetroPorting-Toolchains/releases/latest).
In File Explorer, right-click the ZIP, choose **Extract All**, and extract its
contents into `C:\RecompDev\Toolchain` (or `Toolchain` inside your chosen
workspace). There is no toolchain installer to run.

**Check the extracted folder before continuing.** The toolchain location must
be the folder containing `bin`, so the example above has
`C:\RecompDev\Toolchain\bin\cmake.exe`. If extraction added another folder
level, use the inner folder containing `bin` as the toolchain location below.
Do not point at the ZIP itself or at `bin`.

In the same PowerShell window, prepare the session and check the tools:

```powershell
$env:PSXRECOMP_TOOLCHAIN_DIR = Join-Path $RecompRoot 'Toolchain'
$env:Path = "$env:PSXRECOMP_TOOLCHAIN_DIR\bin;$env:Path"

git --version
python --version
cmake --version
ninja --version
clang --version
```

If your toolchain is elsewhere, replace the first line with its actual path,
for example `$env:PSXRECOMP_TOOLCHAIN_DIR = 'C:\Tools\MyToolchain'`.
Each version check should print a version number before you continue.

## 3. Download the framework and prepare your disc

Use Git to obtain the framework and its **submodules** (other source
repositories required by the project):

```powershell
Set-Location $RecompRoot
git clone --recurse-submodules https://github.com/RetroPortingToolKit/psxrecomp.git
Set-Location psxrecomp
```

If you already downloaded `psxrecomp-master.zip`, you can leave that extracted
copy aside and use this clone. A source ZIP does not include the submodule
contents, and running `git submodule update` inside an extracted ZIP will not
turn it into a Git checkout.

The clone creates the `psxrecomp` folder; it does not install an application.
You should now be in that folder and see `README.md`, `docs` and `tools`.
The next step uses its setup script to generate and build your game project.

Use your own supported CUE/BIN dump. Extract archives first and keep the
`.cue` with all of its referenced `.bin` tracks. Keep the disc outside the
source repository. The next command uses `C:\Discs\My Game\game.cue` as a
placeholder: replace it with your actual `.cue` path, keeping the quotes if
the path contains spaces.

Check the [existing projects](../README.md#games) before starting another
port of the same game. Their per-disc configurations and validation may give
you a better starting point for a contribution.

## 4. Generate and build a local game project

Run the setup command with these fixed first-project choices:

```powershell
powershell -File tools\new_project_layout\setup_project.ps1 `
  -Disc 'C:\Discs\My Game\game.cue' -Dir $RecompRoot `
  -Name MyFirstPSXPort -GithubRepo my-first-psx-port -Players 1 -Yes `
  -EnableRecompUi -NoNetplay -NoFetchBoxart -NoGithub -Generate -EnableBuild
```

The output folder is `my-first-psx-port` inside your workspace (for example,
`C:\RecompDev\my-first-psx-port`). Keep it separate from the framework source
folder. The script probes the disc, records its identity, creates the
framework/UI submodules, generates
the game and open-source OpenBIOS code, then configures and builds. The
`-GithubRepo` value controls the local folder name. `-NoGithub` keeps the
project local; no GitHub account is needed to clone these public repositories
or run this command. You do not need to supply a retail BIOS for this path.

Wait for generation and compilation to succeed. Read errors at the step
where they occur; a created folder alone is not a completed build. After a
successful build, the game executable is under your game project's
`build-release` folder. Use the build output for its filename, open it, select
the same disc if asked, and try a cold boot with the default presentation.

## 5. Make one short route reliable

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

## 6. Add enhancements after the baseline

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

## Common first-time setup questions

| Symptom or question | What to check |
| --- | --- |
| "Where is `RecompDev` or `Toolchain`?" | These are folders you choose and create in step 1 and when extracting the toolchain in step 2. They are not installed by Git or Python. |
| "Does `psxrecomp-master.zip` have an installer or `.exe`?" | The source ZIP has neither. Clone the source in step 3, then generate and build a game in step 4 to produce its executable. |
| "I only have a C: drive." | Set `$RecompRoot` to a folder on C:. No D: drive or particular folder name is required. |
| A tool command is "not recognized" | For Git or Python, check their installation and reopen PowerShell. For CMake, Ninja or Clang, check the extracted `bin` folder and repeat the session settings in step 2. |
| `python --version` opens the Microsoft Store | Check that Python 3 is installed and on PATH. If Windows' App execution aliases intercept the command, disable the Store aliases for `python.exe` and `python3.exe`, then reopen PowerShell. |
| `setup_project.ps1` cannot be found | Run the command from the cloned `psxrecomp` folder. It contains `tools\new_project_layout\setup_project.ps1`. |
| `-Disc not found` | Replace the example disc path with your own extracted `.cue` file's full path. Keep all referenced tracks beside it as described by the CUE. |

If a step fails, keep the exact command and its error text, which step you
reached, and the game/disc revision you are using. Those details make a support
report much easier to reproduce than a general "setup did not work."
