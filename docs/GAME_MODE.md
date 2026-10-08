# Game Mode (Steam Deck, gamescope)

On a Steam Deck in Game Mode, or any gamescope session, there is no desktop.
The launcher is an extra screen built for a mouse, and a windowed game sits
inside a fullscreen compositor anyway. Game Mode therefore boots straight into
the game, borderless fullscreen.

| Source | Effect |
| --- | --- |
| `--game-mode` / `--no-game-mode` | on / off (wins over everything) |
| `PSX_GAME_MODE=1` / `=0` | on / off |
| `XDG_CURRENT_DESKTOP` contains `gamescope`, or `GAMESCOPE_WAYLAND_DISPLAY` is set | on (detected) |
| `SteamGamepadUI=1` (started from Big Picture / Game Mode) | on (detected) |
| none of the above | off: desktop behaviour, unchanged |

When it is on:

- With a disc already set up (settings.toml `[disc] path`, or `--disc`), the
  launcher is skipped and the game opens borderless fullscreen. The saved
  Fullscreen choice is not changed, so Desktop Mode keeps its own.
- On the first run, with no disc yet, the launcher opens so the player can pick
  their disc image. The next start goes straight in.
- `--launcher` still opens the launcher. In Game Mode, add it to the Steam
  shortcut's launch options to reach the settings.
- `--headless` and `--hidden-window` runs are never affected.

The runtime logs the decision, for example
`psxrecomp: game mode (gamescope session): fullscreen, no launcher`.
