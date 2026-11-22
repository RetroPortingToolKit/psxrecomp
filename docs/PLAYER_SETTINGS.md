# Player Settings

The launcher exposes General, Display and PGXP preferences through the existing mod
provider interface. The reserved `psx.runtime.settings` entries are preferences,
not installed mods. Values are validated by `recompiler/src/player_settings.h`,
saved in executable-side `settings.toml`, and applied on startup and after Play.
The provider merges a fresh file before each write and rejects malformed files;
a stale launcher snapshot must not overwrite newer preference edits.

The typed provider covers 49 values: seven existing preferences plus 42 additional
runtime preferences. Existing launcher controls supply renderer, aspect, VSync,
interpolation, interpolation rate and widescreen. Environment overrides remain
authoritative. A title that disables fallback Select shortcuts cannot have them
enabled by the player preference. These preferences do not change the emulated
CPU clock or enable gameplay timing hacks.

The additional values include host audio buffering, input anti-deadzone, mouse
capture and sensitivity, BIOS intro policy, render presentation, and visual
filters. Exact accepted ranges and choices live in `kExtraPlayerSettings`.
Missing values retain the title/runtime baseline. Each spec names its page
(one provider feature per page: `general`, `display`, `pgxp`) and an optional
heading, which the provider reports as the option's group. Rows that share a
heading are adjacent in `kExtraPlayerSettings`; the launcher draws them in that
order.

Internal resolution, the CRT filter, image filtering, enhanced audio,
post-processing, PGXP and rewind are player settings, not mods. Where they sit:

| Page | Heading | Settings (`settings.toml` key) |
|---|---|---|
| Display | Display | Internal resolution (`video.internal_resolution`: 1x..32x, default 4x), then the post-processing and other display rows |
| Display | CRT filter | Screen model (`video.crt_filter`), Scanlines (`video.scanlines`), Scanline strength (`video.scanline_strength`) |
| Display | Image filtering | Smoothing (`video.antialiasing`), Texture filtering (`video.texture_filtering`), FMV filtering (`video.fmv_filter`) |
| Display | Automatic resolution | `video.dynamic_resolution`, `dynamic_resolution_min`, `match_display_max_lines`, `supersample`, `supersample_disable_at_lines` |
| PGXP | PGXP | Vertex correction (`video.geometry_correction`), perspective-correct textures (`video.perspective_texturing`), CPU propagation (`video.pgxp_cpu_mode`), precise culling (`video.pgxp_culling`) |
| General | General | Enhanced audio (`audio.spu_hq`), after the other General rows |
| General | Rewind | Rewind (`video.rewind`), buffer (`video.rewind_depth`), interval (`video.rewind_interval`) |

Each session start copies these into the session's requests
(`host_enhancements.h`, `pgxp_session.h`) and resolves them over the title's
`game.toml` baseline; a setting left unset keeps the baseline. A fixed
Internal resolution turns automatic resolution off; the Automatic resolution
rows replace the title's baseline and apply while Internal resolution is left
at the title's choice. The ceiling and the supersample height cut apply
whatever the choice. Precise culling needs vertex correction and never arms in
netplay. Rewind needs a build with rewind support; without it the row says so.
Netplay and environment restrictions still take precedence.

A `mods/state.toml` that still selects one of the former mod packages
(`psx.enhancement.internal-resolution`, `crt-filter`, `image-filtering`,
`enhanced-audio`, `post-processing`, `pgxp`, `psx.feature.rewind`) does not
fail: startup preserves its effective choices once into missing Settings keys.
Existing Settings keys always win. Disabled presentation/audio/PGXP packages
keep the title baseline; Internal Resolution Off keeps native 1x with automatic
resolution off and supersampling at 1x, while its saved Game default keeps the
title baseline. Legacy height presets, Match display and Auto remain valid
resolution choices. Rewind Off remains off. The saved migration marker survives
ordinary Settings writes, and later launches do not reapply dormant choices.
The mod state, resources and native memory cards are left untouched; the retired
packages stay inactive even when old copies are installed.

The paired UI offers Faithful, Best and derived Custom states. Faithful uses
native 1x resolution, sharp presentation, nearest texture/FMV filtering, normal
dithering, and no PGXP vertex or perspective correction. Best uses 4x resolution,
smooth presentation, bilinear textures, bicubic FMV filtering, fine dithering,
FXAA HQ, mipmaps, 16x anisotropy, chroma smoothing, and explicit PGXP vertex and
perspective correction. Both retain 4:3, disable interpolation/frame generation,
use accurate blending, no bloom/post shader, and a 1x supersample multiplier.
Both set the matching player settings above, except the CRT filter, PGXP CPU
propagation and precise culling, which keep the player's saved values.
Title-specific tuning,
third-party features, disc-speed choices, audio, bindings and cards are retained.
Best can increase GPU demand; every value remains adjustable. Presets make no
claim of higher simulation speed, widescreen support or compatibility for every
title. The exact map is `launcher_psx_settings.inc` in the paired UI.

Focused checks are `player_settings_provider_test`,
`legacy_player_settings_test`, `player_settings_composition`, `pgxp_player_preferences_test` and, with the
paired UI, `player_settings_presets_test`. The last check uses real built-in
manifests, persistence/reload and software-rendered ImGui draw data; it does not
test a retail game's rendering or gameplay.
