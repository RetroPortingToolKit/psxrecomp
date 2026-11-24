# Stable world texture filtering

Stable minification on OpenGL is texture filter mode 2. A title selects it as
its baseline with `[video] texture_filtering = "stable"` (see
`config_schema.md`), a player with the Stable 3D choice of the Texture
filtering setting, and a single run with `PSX_TEXTURE_FILTER=2`. Trusted title
plugins can also call `psx_mod_set_texture_filter(2)`. Modes 0 and 1 select
nearest and bilinear. A mod-session reset restores the player's Display
setting; the override is not saved there.

The stable shader uses screen-space UV derivatives and at most sixteen
bilinear samples over a footprint bounded to eight texels per axis. It decodes
4-bit/8-bit palettes or direct 15-bit colors before averaging. Each sample reads
the current CLUT, honors texture windows, and clamps primitive bounds before
UV wrapping. This avoids stale palette mipmaps and sampling across atlas edges.

The center texel owns the cutout and semi-transparency classification. Neighbors
of a different STP class cannot contribute, and surviving sample weights are
renormalized. Only polygons with proven projected geometry receive the stable
filter. Sprites and untracked UI packets retain nearest sampling. Other backends
fall back to their existing bilinear mode.

This is a bounded minification filter, not an asset mipmap or texture replacement
system. Extreme minification can still alias; filtering also adds texture fetch
cost. The title catalog offers nearest, bilinear and stable choices.

`gl_texture_filter_test` runs a hidden real GL context over source-owned fixtures:
325 checks cover all three texture depths, minification, live palettes, cutouts,
STP separation, sharp UI, texture windows and wrap/atlas edges. On the checkerboard
fixture, red/blue chroma variation falls from 1116 to 22 for every depth.

For a running debug build, compare the same scene and resolution with:

```text
python tools/measure_render_quality.py --port 4370 --counter-prefix medievil.fr --output measurement.json
```

The resulting JSON records guest cadence, original and extra draw counters,
renderer settings, timing and statistical execution phases. The phases are host
wall-time samples, including waits and render replay; they are not a percentage
of guest instructions compiled. Extra draw counts also do not establish how
many unique images reached the monitor. Keep workload and other system activity
consistent, and report the measured rates separately from the selected target.
