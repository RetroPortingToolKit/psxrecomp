# Checkpoint capture and resume

A debug feature for replays. A run saves a checkpoint at a chosen frame, and a later run starts from it and continues exactly as the first run would have. Use it to start a long replay part way through, or to show that a change does not alter a run.

It is off unless its environment variables are set, and `PSX_NO_DEBUG_TOOLS` builds leave it out. Player save states, rewind and netplay are unchanged: without the capture variables a state is byte for byte what the runtime always writes.

## What a checkpoint holds

A checkpoint is three kinds of file side by side:

- the state, `<name>.pst`: an ordinary save state plus four optional sections that a resume needs (`CPU_EXEC`, `SCHED`, `MEMCTRL`, `IRQ_TIMING`; see `runtime/include/boot_state.h`). They are written only while a capture variable is set, and older builds skip them;
- its manifest, `<name>.pst.json` (`psx-tas-stateio-v3`, `runtime/include/source_tas_stateio.h`): the frame taken and the frame requested, the cycle count, a RAM digest, the configuration and executable hashes, the input route and the state's SHA-256;
- one image per inserted memory card, `<name>.pst.card1.mcd` and `<name>.pst.card2.mcd`, each hashed in the manifest.

An existing manifest is never overwritten.

## Environment

| Variable | Meaning |
|---|---|
| `PSX_TAS_SAVE_STATE_AT=<frame>[,<frame>...]` | Capture at these frames. |
| `PSX_TAS_SAVE_STATE_PATH=<file.pst>` | Where to write. `{frame}` in the name becomes the six-digit frame taken. |
| `PSX_TAS_RESUME_STATE=<file.pst>` | Resume from this state at start. |
| `PSX_TAS_RESUME_MANIFEST=<file.json>` | Its manifest, if not `<state>.json`. |
| `PSX_TAS_RESUME_COMPATIBLE_BUILD=1` | Accept a rebuilt executable whose checkpoint compatibility id matches. |
| `PSX_TAS_PERTURB_RESTORE=<field>` | After an accepted resume, change one restored value: `ram`, `irq_timing`, `sched`, `timer2` or `memctrl`. For negative controls only. |

Frames are the debug server's frame counter, one per presented VBlank. A capture requested at a frame is taken at the next save-state boundary with a safe resume PC, which may be a frame or more later; the manifest records both. A frame requested while an earlier capture is still waiting is skipped.

## Resume checks

Before anything is loaded, a resume compares the manifest with this run: the configuration, the executable (or its compatibility id), the input route, the state file's SHA-256, and each memory card's presence and SHA-256. After loading it checks the frame, the cycle count and the RAM digest. Any difference prints `[tas-checkpoint] resume rejected: <reason>` and the process exits with status 2.

A resume never writes the memory card files bound to the run. Insert cards that already hold the checkpoint's images (copy the `.mcd` files beside the state into place first); a card that differs is refused.

When `PSX_INPUT_ROUTE_FILE` is unset the route is recorded as `none`. A resume does not seek an input route to the checkpoint's frame, so resumes of runs with recorded input are not supported yet.

## Tools

The tools in `tools/tasreplays/` run a debug build headless with its debug server on. Give the game command after `--`; the driver adds the variables.

Capture at frames 500 and 3000, then resume from the first capture:

```console
python3 tools/tasreplays/run_checkpoint.py runs/A1 --save-at 500 3000 -- build/game --headless
python3 tools/tasreplays/run_checkpoint.py runs/B --save-at 3000 --resume-from runs/A1/tas-state-000500.pst -- build/game --headless
python3 tools/tasreplays/compare_checkpoint_runs.py runs/A1 runs/B --start 500
```

`run_checkpoint.py` quits the game through the debug server once the capture at the last frame has its manifest, and writes `captures.json` and `run.json` in the output folder. `compare_checkpoint_runs.py` requires, for every capture after the resume frame, the same frame taken, cycle and RAM digest, and identical state images; `compare_boot_states.py` names the first differing section and offset. Use the frame the capture was taken at, from `captures.json`, as `--start`.

`verify_checkpoint_cohort.py` does the whole check for one game: two cold runs, resumes from frames 500, 1500 and 2500, all compared with the first cold run at 500, 501, 560, 1500, 1501, 1560, 2500, 2501, 2560 and a terminal frame, plus a resume from 2500 with `timer2` perturbed that must be caught. It writes `results.json` with every command, exit status, duration and comparison.

```console
python3 tools/tasreplays/verify_checkpoint_cohort.py runs/cohort --terminal 3000 -- build/game --headless
```

A cohort pass shows that resumes of that build reproduce its own runs without input. It does not compare the build with another emulator or with hardware.

`python3 -B tools/tasreplays/test_checkpoint_tools.py` runs the tools against a stand-in game, with no disc or BIOS.
