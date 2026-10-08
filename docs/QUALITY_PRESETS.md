# Graphics presets and hardware detection

A title can offer named graphics presets, Low, Medium, High and Ultra, the way
PC games do. On first launch psxrecomp detects the machine and picks one, saves
the pick, and shows it in Settings → Display → **Graphics preset** together with
what it detected and a **Re-detect** button. Change any setting a preset sets and
the preset becomes **Custom**. psxrecomp never overrides a Custom setup on its own.
Dynamic resolution stays on in every preset as the safety net under the preset.

A title with no `[quality.*]` tables gets none of this, and nothing changes.

## Declaring presets (game.toml)

```toml
[video]                      # the shipped look; usually Ultra
supersample = 1.5
frame_generation = true
dynamic_resolution_min = "display"

[quality.ultra]              # empty: exactly [video]

[quality.high]
supersample = 1.25

[quality.medium]
supersample = 1.0
dynamic_resolution_min = "720p"

[quality.low]
supersample = 1.0
dynamic_resolution_min = "native"
frame_generation = false
```

Each `[quality.<name>]` table holds `[video]` keys. The preset is the whole
game.toml re-parsed with those keys laid over `[video]`, so every `[video]` key
works in a preset, with the same validation, and no other code needs to know
about presets. The names are `low`, `medium`, `high` and `ultra`. Any subset
may be offered. Any other name, a table value, or an invalid value is a load
error. The keys a title's presets set, taken together, are the keys presets
**govern**.

## Detection

`quality_probe.c` reads OS facts only on every launch: the CPU brand, logical
thread count and RAM, the GPU name on Windows (primary display device) and
macOS (Apple silicon names the GPU after the chip), the GPU's PCI id on Linux,
and Steam Deck DMI (`Valve` / `Jupiter` or `Galileo`) or `SteamDeck=1`. These
facts hash to the **hardware fingerprint**. When a detection is due, a hidden
1×1 OpenGL window reads `GL_RENDERER`: the GPU OpenGL actually renders on,
which is not always the adapter the OS names (a desktop whose display hangs off
the CPU's iGPU while a discrete card sits idle, or a hybrid laptop). The OS
name is the fallback when the probe fails, and the probe is skipped under the
`dummy`/`offscreen` SDL video drivers. The context-creation log line names the
renderer too.

`quality_tier.c` maps the facts to a tier. It is a pure function, unit-tested in
`recompiler/tests/quality_presets_test.cpp`:

| GPU | Tier |
| --- | --- |
| Software GL (llvmpipe, SwiftShader, Microsoft Basic Render), Steam Deck (Van Gogh, `AMD Custom GPU 0405/0932`), mobile GPUs, Intel HD/UHD, AMD Vega / "Radeon Graphics" APUs, Apple M1 | Low |
| Apple M2, Intel Iris Xe / Arc iGPU, Radeon 680M–890M class, GeForce MX / GT | Medium |
| Apple M3, M1 Pro, GTX 9xx/10xx, RX 4xx/5xx, unknown GPU | High |
| Apple M4 and later, M2+ Pro, any Max/Ultra, RTX, GTX 16, RX 6000+, Intel Arc A | Ultra |

Two caps apply after the GPU class. Fewer than 4 threads, or under 6 GB of
memory, caps the tier at Low. Exactly 4 threads, or 8 GB of memory, caps it at
Medium. If the tier is not offered, the highest offered tier below it is used,
or failing that the lowest one above it. Unknown hardware gets High, because
dynamic resolution catches an optimistic guess.

The reference points come from R4 measurements (RidgeRacerType4Recomp
`analysis/lowend-perf/PLAN.md`). An Apple M4 holds every enhancement at 60 Hz.
An M1 has about a third of the M4's GPU and about 70 % of its per-core speed. A
Steam Deck has about 40 % of the GPU and half the per-core speed.

## When detection runs, and what is saved

settings.toml `[video]` keeps the following:

| Key | Meaning |
| --- | --- |
| `quality_preset` | `low`..`ultra`, or `custom` |
| `quality_base` | the preset a `custom` started from (its values for keys with no launcher row, such as `supersample`) |
| `quality_hardware` | the fingerprint the last detection ran on |
| `quality_detected` | what that detection picked |

Detection runs on first launch (no `quality_preset`), whenever the fingerprint
changes while a preset (not Custom) is in force, and on **Re-detect**. A new
machine under Custom is only reported:

    psxrecomp: new hardware since your graphics settings were made; your Custom settings are kept

While a preset is in force it owns its keys. The player's saved settings.toml
values for those keys are ignored, so a launcher save of the preset's own values
cannot pin them. Under Custom the player's saved values apply on top of
`quality_base`. Keys a preset does not govern always stay the player's.

`PSX_QUALITY=low|medium|high|ultra` picks a preset for one run and is never
saved. `PSX_QUALITY_REDETECT=1` forces a detection.

## Launcher

`RecompLauncherCGameInfo.quality_*` gives the launcher the offered presets, the
detected preset, a hardware summary (for example "Apple M1 · 8 threads · 8 GB"),
and two callbacks:

- `quality_apply` fills the Settings rows a preset sets. The host runs the
  preset's values through the same conversions it uses to seed the launcher.
- `quality_redetect` probes again, GL included, and returns the preset.

`Settings.quality_preset` is 1–4 for a preset and 5 for Custom. The launcher
snapshots the rows after a preset is applied. As soon as a governed row differs
from the snapshot, it sets Custom and keeps the preset it came from in
`quality_base`. On launch the host saves the state. A newly picked preset's
keys that have no row, such as `supersample` and the PGXP extras, apply at once
on the boot launcher and at the next launch from the in-game settings.
