# Player Settings

The launcher exposes General and Display preferences through the existing mod
provider interface. The reserved `psx.runtime.settings` entries are preferences,
not installed mods. Values are validated by `recompiler/src/player_settings.h`,
saved in executable-side `settings.toml`, and applied on startup and after Play.
The provider merges a fresh file before each write and rejects malformed files;
a stale launcher snapshot must not overwrite newer preference edits.

The typed provider covers 35 values: seven existing preferences plus 28 additional
runtime preferences. Existing launcher controls supply renderer, aspect, VSync,
interpolation, interpolation rate and widescreen. Environment overrides remain
authoritative. A title that disables fallback Select shortcuts cannot have them
enabled by the player preference. These preferences do not change the emulated
CPU clock or enable gameplay timing hacks.

The additional values include host audio buffering, input anti-deadzone, mouse
capture and sensitivity, BIOS intro policy, render presentation, and visual
filters. Exact accepted ranges and choices live in `kExtraPlayerSettings`.
Missing values retain the title/runtime baseline. PGXP vertex and texture
preferences override those visual choices independently; the mod's CPU and
culling choices remain separate. Netplay and environment restrictions still
take precedence.

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
