# Why a start ended before the first frame, in the run report

Every run writes `psx_last_run_report.json` when it exits. A start that ends
before the game runs has `frame` 0. Three fields say why it ended.

```json
"exit_origin": "start_refused",
"start_refused": {"kind": "no_bios", "title": "BIOS Mismatch",
                  "message": "That BIOS (524288 bytes, CRC32 318178BF) is not an image this build was compiled from. ..."},
"launcher_status": {"bios": "CRC32 318178BF (this build expects SCPH-5552, CRC32 D786F0B9).",
                    "disc": "verdict=ok serial=SLES-01156 expected=SLES-01156 tracks=1"}
```

| Field | Meaning |
| --- | --- |
| `exit_origin` | `start_refused`: the program refused to start the game. `launcher_closed`: the player closed the launcher without Play. `setup_relaunch`: setup finished and starts the game. |
| `start_refused` | `null`, or the refusal: `kind` for tools, and the `title` and `message` of the box the player was shown. |
| `launcher_status` | `null`, or what the launcher's BIOS row and disc row said last. A red row blocks Play, so a start that ends as `launcher_closed` has its reason here. |

Once the game runs, `start_refused` and `launcher_status` are `null`.

`disc_warning` is `null`, or a nonfatal known-disc missing-SBI sentence. It uses
the mounted disc's detected serial and basename, not the expected config ID.
It persists while the game runs, so a later black screen or crash report still
contains the warning. It is set when the launch mounts the disc and again after a
successful disc change while the game runs. A loaded SBI removes it; this field does not qualify the
disc pressing, companion identity or protection/gameplay result.

## File names

The report names a file by its **base name** only, never by its folder.
Players paste this file into a chat, and it held no path before. The box,
which only the player sees, may show the full path. Text that is not UTF-8 (a
Windows file name in the system code page) is written as `\u00XX`, so the
report always loads as UTF-8 JSON.

## Kinds

| `kind` | When |
| --- | --- |
| `config_unreadable` | `game.toml` cannot be read. |
| `memcard_dir` | The folder given with `--memcard-dir` cannot be created. |
| `overlay_cache` | The cache of compiled game code cannot be set up. |
| `no_disc` | No usable disc: the image does not open, or the picker was cancelled. |
| `mods` | The selected mods cannot be applied, or cannot be switched off for a netplay match. |
| `no_bios` | No usable BIOS: the file is not an image this build was compiled from, the bundled image is missing, or the picker was cancelled. |
| `setup_program` | The program is a setup program: no game and no BIOS code is linked into it. Run Generate and rebuild; the game is in `build-release`. |
| `bios_mismatch` | The BIOS could not be made active when the session started. |
| `disc_not_mounted` | The image verified but the drive cannot mount it. |
| `video_init` | The video system, the window or the picture output cannot be created. |
| `netplay_session_bios` | A match from the launcher needs a retail BIOS that is not on this computer. |
| `netplay_disc` | The disc image is not valid for online play, or none is verified. |
| `netplay_address` | A match was started with no address to listen on. |

Not recorded: a failed netplay start (it has its own sentence) and the
developer gates that exit with code 2 (input routes, TAS state files).

## Boxes

The refusals that printed only a line (no disc or BIOS after a cancelled
picker, mods, the cache, the window, the netplay refusals
above) open a box when a person started the game. A scripted start gets no
new box: `--headless`, `PSX_HEADLESS`, `--no-launcher`, `PSX_NO_LAUNCHER`, a
replay launch, or `[launcher] skip_launcher = true` in `settings.toml`. It
gets the old line on stderr and one more: `psxrecomp: start refused (<kind>)`.
