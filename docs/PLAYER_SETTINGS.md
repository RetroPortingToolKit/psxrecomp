# Player Settings

The launcher exposes General, Display and PGXP preferences through the existing mod
provider interface. The reserved `psx.runtime.settings` entries are preferences,
not installed mods. Values are validated by `recompiler/src/player_settings.h`,
saved in executable-side `settings.toml`, and applied on startup and after Play.
The provider merges a fresh file before each write and rejects malformed files;
a stale launcher snapshot must not overwrite newer preference edits.

The typed provider covers 36 values: seven existing preferences plus 29 additional
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
heading, which the provider reports as the option's group. The PGXP page holds
PGXP vertex correction and perspective-correct textures. These preferences
override the PGXP Precision mod's matching visual choices independently; the
mod's CPU and culling choices remain separate. The Display page's Automatic
resolution section holds automatic resolution on/off, its floor
(`dynamic_resolution_min`), its ceiling (`match_display_max_lines`, the display
height Match display and Auto resolve against), the supersample multiplier and
`supersample_disable_at_lines`. They replace the title's baseline that the
Internal Resolution mod resolves over: they apply while that mod is enabled and
its matching option is Game default (a fixed resolution choice turns automatic
resolution off, Auto turns it on). The ceiling and the supersample height cut
apply whatever the mod chooses. Netplay and environment restrictions still take
precedence.

The paired UI offers Faithful, Best and derived Custom states. Faithful uses
native 1x resolution, sharp presentation, nearest texture/FMV filtering, normal
dithering, and no PGXP vertex or perspective correction. Best uses 4x resolution,
smooth presentation, bilinear textures, bicubic FMV filtering, fine dithering,
FXAA HQ, mipmaps, 16x anisotropy, chroma smoothing, and explicit PGXP vertex and
perspective correction. Both retain 4:3, disable interpolation/frame generation,
use accurate blending, no bloom/post shader, and a 1x supersample multiplier.
Both enable the built-in resolution/filter features and disable the separate
PGXP and CRT features. Saved PGXP CPU/culling options, title-specific tuning,
third-party features, disc-speed choices, audio, bindings and cards are retained.
Best can increase GPU demand; every value remains adjustable. Presets make no
claim of higher simulation speed, widescreen support or compatibility for every
title. The exact map is `launcher_psx_settings.inc` in the paired UI.

Focused checks are `player_settings_provider_test`,
`player_settings_composition`, `pgxp_player_preferences_test` and, with the
paired UI, `player_settings_presets_test`. The last check uses real built-in
manifests, persistence/reload and software-rendered ImGui draw data; it does not
test a retail game's rendering or gameplay.
