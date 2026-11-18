# Captured replay observations

These standalone Python tools compare captured observations. They do not run a game, select an oracle producer or qualify gameplay.

`compare_ram_pages.py SOURCE NATIVE REPORT.json` compares complete RAM-page indices and cumulative clocks. Indices use 512 FNV-1a64 hashes for the PS1's 2 MiB RAM. Shared raw snapshots are checked against their indices and compared byte for byte. Missing returns, invalid hashes, discontinuous clocks and incomplete requested coverage are errors. No pages are ignored.

`stream_compare.py` provides an optional watcher for an existing capture writer. It can write a stop-request file at a divergence. Its result is observation evidence; a complete post-run comparison remains the verdict authority. It does not kill or launch a process.

`observation_evidence.py` checks CPU/RAM capture completeness and compares recorded returns. Original RAM-only data remains RAM-only: missing CPU values or source clocks are not invented. Segmented original references retain explicit bounds and content hashes.

The original CPU watcher compares PC, SR, Cause, EPC, HI, LO and GPR1–31. Raw GPR0 is retained but is outside that comparison. Sparse-image passivity checks require the caller's actual input endpoint; no title-specific length is assumed.

The checkpoint tools drive and check checkpoint capture and resume in a debug build (`docs/TAS_CHECKPOINTS.md`). `run_checkpoint.py` runs a game command once with capture and an optional resume. `compare_boot_states.py` compares two state images section by section. `compare_checkpoint_runs.py` compares a resumed run with an uninterrupted one at every later capture. `verify_checkpoint_cohort.py` records cold runs, three resumes and a timer-2 negative control, and checks them all. `test_checkpoint_tools.py` runs them against a stand-in game.

Run the synthetic controls without retail files or optional Python packages:

```console
python3 -B tools/tasreplays/test_stream_compare.py
python3 -B tools/tasreplays/test_checkpoint_tools.py
```

The controls exercise complete comparisons, missing/truncated records, delayed writers, CPU/register and RAM divergences, original RAM-only scope, and segment gaps/overlaps/content changes. A passing fixture does not establish a title result, a compatible emulator build, hardware accuracy or snapshot restore qualification.
