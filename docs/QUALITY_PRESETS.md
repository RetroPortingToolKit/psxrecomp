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
pgxp_depth_buffer = false        # PGXP itself stays on (a mod switch)
pgxp_color_correction = false
pgxp_seam = "off"
```

Each `[quality.<name>]` table holds `[video]` keys from the supported set
below. The preset is the whole game.toml re-parsed with those keys laid over
`[video]`, so each key gets its usual `[video]` validation. The names are
`low`, `medium`, `high` and `ultra`. Any subset may be offered. Any other name,
a table value, an invalid value, or a key outside the supported set is a load
error. The keys a title's presets set, taken together, are the keys presets
**govern**.

### Supported keys

Every key in the set is fully owned by the preset system. A preset in force
masks the player's saved value. A one-run override, or an edit that touches no
governed key, keeps the value on disk. A launcher row or live in-game change
to a governed key is a governed edit and makes the state Custom. Picking a
preset applies the key at once on the boot launcher, and at the next launch
from the soft-return launcher.

| Kind | Keys |
|---|---|
| Player-editable (settings.toml form, launcher row) | `texture_filtering`, `fmv_filter`, `antialiasing`, `dynamic_resolution`, `dynamic_resolution_min`, `frame_generation`, `render_thread`, `present_thread`, `geometry_correction`, `perspective_texturing`, `frame_interpolation`, `frame_interpolation_fps` |
| Preset-only (no settings.toml form, no row) | `supersample`, `texture_lod`, `anisotropic_filtering`, `fmv_chroma_smoothing`, `bloom`, `dithering`, `pgxp_depth_buffer`, `pgxp_color_correction`, `pgxp_seam` |

Other `[video]` keys (`vsync`, `renderer`, `scanlines`, `internal_resolution`,
`supersampling`, ...) are rejected in a preset: they stay the player's.
`PSXRecompV4::quality_key_supported` is the list; extending it means adding
masking, copying, edit classification and apply-on-select for the key.

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
`recompiler/tests/quality_presets_test.cpp` against a table of real
`GL_RENDERER` strings from Windows drivers, Mesa and macOS.

**What it is, and what it is not.** Autodetect is a static heuristic over the
GPU name, the CPU thread count and the memory size. It measures nothing:
there is no benchmark, and a name says nothing about clocks, thermals or the
driver. It is a first-launch starting point, and the dynamic systems
(dynamic resolution down to the title's floor, Smooth motion's in-between
frames made from surplus only, the adaptive aspect) are what actually keep
the frame rate.

| Class | Detected |
| --- | --- |
| Software GL, Steam Deck (Van Gogh), mobile GPUs, every integrated GPU up to the Radeon 890M / Iris Xe / Arc iGPU class, Apple M1 and M2 base chips, GeForce MX/GT | Low |
| A GPU the rules do not recognise (including Linux with only an AMD PCI id, and unnamed nouveau chips) | Medium |
| Recognised capable GPUs: discrete RX/RTX/GTX/Arc A/B cards, Apple M3 and later, any Pro/Max/Ultra chip, Strix Halo (8050S/8060S) | Ultra |

Fewer than 5 CPU threads, or under 6 GB of memory, also gives Low. High is a
manual choice. If the detected tier is not offered, the highest offered tier
below it is used, or failing that the lowest one above it.

**Why unknown is Medium.** Nothing about an unrecognised GPU is known, so the
fallback must not assume Ultra's extra work (supersampling above the display,
and whatever a title adds there, such as R4's mipmaps, 16x aniso and bloom).
Low would also be wrong for what is usually new hardware: it gives up
Smooth motion and the PGXP extras on machines that can almost always run
them. Medium keeps the display's own resolution with dynamic resolution
under it, which is the safe middle. A player can pick any preset at any
time.

On Linux, when the GL probe fails, the sysfs PCI id is used instead (Steam
Deck parts, Intel integrated vs Arc, NVIDIA).

## When detection runs, and what is saved

settings.toml `[video]` keeps the following:

| Key | Meaning |
| --- | --- |
| `quality_preset` | `low`..`ultra`, or `custom` |
| `quality_base` | the preset a `custom` started from (its values for keys with no launcher row, such as `supersample`) |
| `quality_hardware` | the fingerprint the last detection ran on |
| `quality_detected` | what that detection picked |

Detection runs on first launch (no `quality_preset`), whenever the fingerprint
changes while a preset (not Custom) is in force, and on **Re-detect**. It never
runs under `--headless` or `--hidden-window` (CI and scripted runs): a saved
preset applies there, or else the title's `[video]` block, and nothing is
written. A detection is saved only when the GL probe succeeded; a result from
the OS name alone is used for that run and the next launch probes again. A new
machine under Custom is only reported:

    psxrecomp: new hardware since your graphics settings were made; your Custom settings are kept

While a preset is in force it owns its keys. The player's saved settings.toml
values for those keys are ignored, so a launcher save of the preset's own values
cannot pin them. Under Custom the player's saved values apply on top of
`quality_base`. Keys a preset does not govern always stay the player's.

`PSX_QUALITY=low|medium|high|ultra` picks a preset for one run and is never
saved. `PSX_QUALITY_REDETECT=1` forces a detection.

### Who owns a saved key: one transaction

Every settings.toml save that can carry governed keys goes through
`psxq::settle_save` (`quality_presets.cpp`): the boot launcher (Play and Quit),
the soft-return launcher, and the in-game menu's live video settings. The
frontend only reports what the player did (below); the host decides what is
written, against settings.toml as it is on disk at that moment:

| This run | Player did | Saved |
|---|---|---|
| `PSX_QUALITY` override | anything | the governed keys and `quality_*` keys exactly as on disk: the previous preset, or Custom and its values, survive. A governed edit applies to this run only. |
| a preset in force | picked another preset | the picked preset; its keys stay as on disk (it owns them) |
| a preset in force | changed a governed value (launcher row, or a live in-game change) | Custom, from the preset in force, with the edited values; the next launch keeps them |
| Custom | anything | Custom, with the edited values |
| a preset in force | changed only keys no preset governs | the preset, unchanged; those keys are saved |

This holds when a launcher builds its save from an empty seed (the boot
launcher does): governed values the player did not choose are never written.
`quality_presets_test` covers each row through a real settings.toml round
trip, including the next launch's masking.

## Launcher

`RecompLauncherCGameInfo.quality_*` gives the launcher the offered presets, the
detected preset, a hardware summary (for example "Apple M1 · 8 threads · 8 GB"),
and two callbacks:

- `quality_apply` fills the Settings rows a preset sets. The host runs the
  preset's values through the same conversions it uses to seed the launcher.
- `quality_redetect` probes again, GL included, and returns the preset.

`Settings.quality_preset` is 1–4 for a preset and 5 for Custom. The launcher
snapshots the rows after a preset is applied. As soon as a tracked row differs
from the snapshot, it sets Custom and keeps the preset it came from in
`quality_base`. recomp-ui and recomp-launcher report the same way. The
frontends track every quality row, so the host classifies the report itself
(`psxq::launcher_custom_edit`): it fills a copy of the rows from the reported
base preset and counts only rows of keys this title governs. A Custom flag
raised by an ungoverned row (for example Antialiasing when no preset sets it)
keeps the preset, or records the pick it followed. The host then saves the
state through the transaction above. (The launcher may still label such an
edit Custom until it is reopened.) A newly picked
preset's keys that have no row, such as `supersample` and the PGXP extras,
apply at once on the boot launcher and at the next launch from the soft-return
launcher.

The in-game menu's live video settings (`psx_video_apply_live`) have no preset
row. The host compares the governed values the menu changes (texture filter,
FMV filter, smooth scaling, dynamic resolution, Smooth motion, render and
present threads, internal resolution) with the ones in force; a change to any
of them is a governed edit and makes the state Custom.
