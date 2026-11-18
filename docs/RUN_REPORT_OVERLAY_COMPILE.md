# Overlay compile results in the run report

Every run writes `psx_last_run_report.json` next to the exe when it exits.
The `overlay_compile` object in it says what the overlay compile runs of that
start did. A product has no debug server, so this is the only place where a
start says that the compiler rejected a unit.

```json
"overlay_compile": {
  "configured": 1, "consistent": 1, "state": "idle",
  "runs": 3, "runs_failed": 1, "runs_with_result": 3,
  "units_attempted": 41, "units_compiled": 38, "units_failed": 3,
  "units_skipped": 112, "fail_lines": 3, "runs_stopped_at_quit": 0,
  "failure_classes_max": 8, "failure_text_max": 239,
  "fail_lines_in_unnamed_classes": 0,
  "failure_classes": [
    {"class": "compile", "count": 3,
     "first": "SHARD FAIL [compile] 0007E000_1C02BEFB: gcc/tcc compile failed (see COMPILE ERROR above)",
     "first_error": "unit.c:412:6: error: redeclaration of 'func_8007E120' cannot add 'dllexport' attribute"}
  ],
  "output_tail_max": 2000,
  "output_tail": "..."
}
```

| Field | Meaning |
| --- | --- |
| `configured` | 1 when the product has an overlay compile command. |
| `consistent` | 0 when the report was written while the output reader held its lock (a crash at that moment). The texts can then be half updated; the numbers are still bounded. |
| `state` | `idle`, `running` or `done`. A run that was still going when the start ended on its exit timer or in a crash shows `running`. A normal quit stops the run first, so the state is `idle` and `runs_stopped_at_quit` counts it. |
| `runs`, `runs_failed` | Compile driver runs started in this start; and failures: a run that ended badly (a failed unit, a bad exit code, no result line) or a driver that could not be started. `runs_failed` can therefore be larger than `runs`. |
| `runs_with_result` | Runs that printed their `PSX_SHARD_RESULT` line. |
| `units_compiled`, `units_failed`, `units_skipped` | The numbers of those result lines, summed over the start. Skipped units were already in the cache or hold no code. |
| `units_attempted` | `units_compiled` + `units_failed`. |
| `runs_stopped_at_quit` | Runs that a normal quit stopped. A result line such a run had already printed is in the unit counts and in `runs_with_result`. |
| `fail_lines` | `SHARD FAIL` lines seen, counted as they arrive. It includes a run that the end of the start cut short, which has no result line. |
| `failure_classes` | One entry per failure class, at most `failure_classes_max`: the class, how many failures, the first `SHARD FAIL` line, and for a compile failure the compiler's first error line. |
| `fail_lines_in_unnamed_classes` | Failures in classes past that bound. They are counted, not named. |
| `output_tail` | The last `output_tail_max` bytes of the latest run's output. |

Each text is cut at `failure_text_max` bytes. A unit that fails in two runs of
one start is counted twice.

The runtime also prints one line on stderr for the first failure of each class:

```
psxrecomp: overlay compile: a unit failed and runs interpreted: SHARD FAIL [compile] ... | unit.c:412:6: error: ...
```

## What to look for

A unit that fails to compile is not an error for the game: it runs in the
interpreter. It is a fault of the build, and it costs speed.

| Reading | Meaning |
| --- | --- |
| `units_failed` 0 and `fail_lines` 0 | every unit the compiler was given was built |
| `units_failed` or `fail_lines` above 0 | units ran interpreted; `failure_classes[0].first_error` says why |
| `runs` above 0 and `runs_with_result` 0 | the driver never printed its result: read `output_tail` |
| `configured` 1 and `runs` 0 | nothing asked for a compile in this start (everything was cached, or the start was short) |

`autocompile_degraded` in the same report is another fact: it is set when the
compile command itself cannot work. A start with failed units is not degraded.
