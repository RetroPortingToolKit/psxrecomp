# CD read counters in the run report

The run report, `psx_last_run_report.json`, has a `cdrom` object that counts
the read streams of the process:

```json
"cdrom": {"read_starts": 16, "silent_reads": 1, "silent_run_max": 1}
```

| Field | Meaning |
| --- | --- |
| `read_starts` | ReadN and ReadS commands that began a new read stream. |
| `silent_reads` | Streams that ended (Pause, Stop, Init, a seek, the next read start) before one sector was read off the disc. |
| `silent_run_max` | The longest run of silent streams in a row. A stream that reads a sector ends the run. |

A stream that is still open when the report is written is not counted.

## What to look for

A game's CD library restarts a read that delivers nothing in time. One or two
silent reads in a row are normal: a game may cancel a read while the drive is
still seeking. A long run means the game restarts the same read again and
again and never gets a sector.

| `silent_run_max` | Reading |
| --- | --- |
| 0 to 2 | normal |
| 3 or more | look at the game: a read does not deliver in time |
| tens or hundreds | the game is stuck in a read retry loop |

## Limits

The counters belong to the process and are not part of a save state; after a
state load they can be off by one. They are diagnostics: the drive reads none
of them to decide anything, and the cost is a few integer updates where a
stream starts and ends.
