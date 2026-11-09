# Interpreted code in the run report

Every run writes `psx_last_run_report.json` next to the exe when it exits.
`overlay_loader.disp_interp_overlay` in it is one number: the dispatches above
the kernel window for which the overlay loader had no native unit. The
`interp_detail` object says what stands behind that number. A product has no
debug server, so this is the only place where a start says it.

```json
"interp_detail": {
    "text_guard": {"armed": 1, "lo": "0x00010000", "hi": "0x000AE800",
                   "native_blocked": 1204, "diverged_pages": 0, "exact_mismatches": 1204,
                   "last_mismatch": {"range": "0x00064D80", "len": 844, "at": "0x00065070",
                                     "live_byte": 33, "image_byte": 37}},
    "ranges": {"kernel_end": "0x00010000", "text_lo": "0x00010000", "text_hi": "0x000AE800"},
    "interpreter": {"blocks_run": 71132060, "insns_run": 912345678, "aborts": 0,
                    "guard_yields": 12, "native_handoffs": 40311},
    "interpreter_after_game_entry": {"blocks_run": 48132060, "insns_run": 612345678,
                                     "aborts": 0, "guard_yields": 12, "native_handoffs": 40311},
    "per_address": {"table_size": 262144, "addresses": 2210,
                    "by_place": {"kernel": {"addresses": 14, "hits": 3849, "entries": 3849},
                                 "text": {"addresses": 2196, "hits": 71128211, "entries": 880231},
                                 "overlay": {"addresses": 0, "hits": 0, "entries": 0}},
                    "preload": {"game_started": 1, "frame": 1841, "addresses": 1544,
                                "by_place": {"kernel": {"addresses": 5, "hits": 367000, "entries": 367000},
                                             "text": {"addresses": 1300, "hits": 23000000, "entries": 1000000},
                                             "overlay": {"addresses": 239, "hits": 76000, "entries": 76000}},
                                "interpreter": {"blocks_run": 23000000, "insns_run": 300000000,
                                                "aborts": 0, "guard_yields": 0, "native_handoffs": 0}},
                    "hottest_max": 32, "hottest": [
      {"pc": "0x80064D80", "place": "text", "hits": 2364000, "entries": 2364000, "insns": 40188000,
       "last_caller_ra": "0x80064164", "unit_crc": "0x53132191", "unit_valid": 0}
    ]},
    "loader_miss": {"above_kernel": {"miss_cached": 70100000, "no_unit": 12, "stale_bytes": 1028199,
                                     "outside_window": 0, "device_touch": 0, "native_off": 0,
                                     "diff_gate": 0, "rank": 0, "bad_entry": 0,
                                     "modified_text_backoff": 0,
                                     "modified_text_load_bound": 0},
                    "kernel": {"miss_cached": 3800, "no_unit": 49, "stale_bytes": 0,
                               "outside_window": 0, "device_touch": 0, "native_off": 0,
                               "diff_gate": 0, "rank": 0, "bad_entry": 0,
                               "modified_text_backoff": 0,
                               "modified_text_load_bound": 0}}
}
```

The numbers above show the form; they are not a measurement.

## text_guard

The text image guard compares the game's own code in RAM with the boot
executable before a statically compiled function runs.

| Field | Meaning |
| --- | --- |
| `armed` | 1 when the guard has its reference image. 0 means that no statically compiled game function runs at all: every one is refused, and the game's own code is interpreted. |
| `lo`, `hi` | The physical range of the reference image. |
| `native_blocked` | How often the guard refused a static function. |
| `diverged_pages` | Pages marked as changed for good by the page-level check. |
| `exact_mismatches` | How often a function's own byte ranges differed from the image. |
| `last_mismatch` | The last such case: the range, the first differing address, the byte in RAM and the byte in the image. |

## ranges and place

Each address has a place: `kernel` below `kernel_end`, `text` from `text_lo`
to `text_hi` (the boot executable), `overlay` for the rest of RAM. A boot
executable that loads high has overlay RAM on both sides.

## interpreter

Totals of the RAM interpreter: blocks and instructions run, aborts on an
unsupported instruction, long loops that yielded, and hand-backs to native
code.

The legacy `interpreter` object still counts from power-on. The
`interpreter_after_game_entry` object subtracts the snapshot taken at the first
game entry. `per_address.preload.interpreter` holds that snapshot. Before game
entry, all current totals are preload and every after-entry value is zero. A
later game entry does not replace the first snapshot. If a restored or damaged
counter is below its snapshot, the reported difference is zero instead of an
unsigned wraparound.

## per_address

The interpreter keeps one row per block entry address it ran. `addresses` is
the number of rows with hits; `by_place` sums them per place. `hottest` lists the
`hottest_max` rows with the most hits, the hottest first.

`addresses`, `by_place` and `hottest` count what ran after game entry
(PS1B-513). Before that, with a real BIOS intro (`fast_boot` false and the
BIOS run as recompiled code, not by the HLE boot), the BIOS shell runs from RAM
0x30000 and up. Its addresses lie inside the game's text or overlay range, so a
place decided from the game's ranges called them the game's: on the EU BIOS
(SCPH-5552) 0x000597E8, 0x0005981C and 0x00059774 were the hottest "text"
addresses of every title. At game entry (the latch that also clears the text
image baseline, `fntrace_mark_game_started`) each row's counts are moved to
`preload` and cleared. The row keeps its address, so the table's lookups are
unchanged, and an address that runs again after entry counts again from 0.
`table_size` is still the size of the whole table: rows moved at entry stay in
it. So `addresses` no longer counts every row in use: the rows in use are at
most `addresses` plus `preload.addresses` (a row that ran on both sides of game
entry is in both). A reader that judges how full the table is uses that sum.

| Field | Meaning |
| --- | --- |
| `hits` | Times the interpreter started a block at this address. |
| `entries` | Of those, the times it arrived from native code or the dispatch loop, not from the interpreter's own block chaining. |
| `insns` | Instructions run from this address. |
| `last_caller_ra` | The guest return address at the last such arrival: who called it. |
| `unit_crc`, `unit_valid` | The compiled overlay unit whose ranges cover the address at that time, and whether its code matched. 0 and 0: no unit covers it. A CRC with `unit_valid` 0: a unit exists, but its code is not the code in RAM. |

### per_address.preload

What the interpreter ran before game entry, by the same places. It sits next
to `by_place`, so a reader that sums the keys of `by_place` still reads three
places.

| Field | Meaning |
| --- | --- |
| `game_started` | 1 when game entry moved the rows. 0: the game was never entered in this start (it stopped in the BIOS), and `by_place` and `hottest` hold everything that ran. |
| `frame` | The frame of the move: the frame of game entry. A start that enters the game twice (the game range armed again) shows the second. |
| `addresses` | The rows moved, summed over the places. A row moved at two entries counts twice. |
| `by_place` | Per place: rows moved, their hits and their entries. |

`by_place` plus `preload.by_place`, place by place, is what `by_place` held
before this change: everything the interpreter ran since power-on. A reader
that wants the old meaning adds the two. A resume of a state taken in the
game (a TAS or replay resume that restores the game-start latch) moves the
rows the same way, with that frame.

The `dirty_ram_stats` debug command reads the same table, so its `per_pc`
rows also start again at game entry.

## loader_miss

`overlay_loader.disp_interp` by reason, above the kernel window and inside it.
The reasons of one place add up to that place's share of `disp_interp`: a miss is
counted once, under its reason, and `disp_interp` and `disp_interp_kernel` are the
sums of the reasons, taken when the report is written.

| Reason | Meaning |
| --- | --- |
| `miss_cached` | The loader already knew that it has nothing valid for this address since the last change of code. The first miss is counted under its own reason. |
| `no_unit` | No compiled unit covers the address. |
| `stale_bytes` | A unit covers it, but none matches the code bytes in RAM. |
| `outside_window` | Clean boot text, or another address the loader does not serve. |
| `device_touch` | The unit matches but touches a device; such a unit never runs natively. |
| `native_off` | Native execution is off, or this function is blocked. |
| `diff_gate` | Held back by the native-against-interpreter comparison (development). |
| `rank` | Held back by the native rank filter (debug tools only). |
| `bad_entry` | The unit ran and refused a foreign interior entry. |
| `mod_entry_hook` | A configured function-entry hook owns the address, so dispatch preserves that hook through the interpreter. |

`disp_interp` counts loader lookups that found no native unit. The interpreter
then may still decline the target, so `per_address` is the record of what was
interpreted.

The legacy `loader_miss` object and `overlay_loader.disp_interp` still count
from power-on. `loader_miss_preload` holds the first game-entry snapshot, and
`loader_miss_after_game_entry` subtracts it from each reason. The overlay
loader also writes `disp_interp_preload` and `disp_interp_after_game_entry`.
Thus old readers keep their totals, while new readers can compare game work
without the BIOS start-up share. Before game entry, `disp_interp_preload` and
`loader_miss_preload` equal the legacy totals, and their after-entry values are
zero.

## Cost

The object is built when the report is written. During play the added work
is one counter increment at each exit of the loader's dispatch that ends in
the interpreter. A native
dispatch passes none of the increments. Game entry walks the per-address
table once to move the pre-load counts.

## BIOS phase line

The runtime prints `psxrecomp: bios phase:` once, before the first game-entry
snapshot, or at exit if the game was never entered. It classifies interpreted
block entries as kernel, shell, shell tail and other; reports the frame and
shell address count; and identifies whether the shell ran from ROM. These are
interpreter-table counts, not elapsed-time or CPU-instruction measurements.
The line diagnoses BIOS startup work and makes no BIOS speed improvement claim.
