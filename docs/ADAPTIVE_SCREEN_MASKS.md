# Owned screen-space masks and HUD

Adaptive world projection needs separate layout rules for cutscene overlays.
Game adapters identify the original producers, wait for their packet writes
to finish, and tag completed packets before GPU submission. The renderer
keeps guest packet bytes unchanged.

The public functions in `runtime/include/mod_plugins.h` are:

| API | Contract |
| --- | --- |
| `psx_mod_tag_screen_mask_quad(prim)` | Extend a flat quad's full-width top/bottom panel or existing vertical side band into the visible reveal. |
| `psx_mod_tag_reveal_edge_quad(prim, edges)` | Grow an owned axis-aligned flat quad, or a textured quad whose four UVs name one texel, from a left/right boundary that already reaches the canonical edge to the visible reveal edge. |
| `psx_mod_tag_radial_screen_mask_quad(prim, scale)` | Scale an owned flat/Gouraud quad about the display centre; tag every piece of the transition. |
| `psx_mod_anchor_hud_primitive(prim, anchor)` | Translate left/right HUD by the visible side margin, or preserve centered text with `anchor == 0`. |
| `psx_mod_widescreen_view_x_margin()` | Return the configured visible per-side reveal, excluding culling safety guards. |

`prim` addresses the packet word immediately before the GP0 color/command.
For a compound draw-mode-plus-SPRT glyph packet, pass the word before the
SPRT command, rather than the compound packet's initial P_TAG. Tags bind the
complete command words, retain expanded-RAM identity, expire after two
frames and clear on reset/state load. Reused or modified packets do not
inherit the layout rule.

Centered HUD tags bypass automatic UI and backdrop transforms. They retain
glyph coordinates, UVs and dimensions, while left/right tags translate the
whole widget. This guarded anchor API differs from the older role-tagging
API where role zero clears the tag.

Panel reveal bands are disjoint from the authored quad, preserving animated
height and avoiding repeated blending. Full-frame panels use the existing
single expansion. Slanted quads and interior strips do not acquire panel
coverage. Adapters must establish ownership; screen position alone is not
an ownership test.

Reveal-edge quads cover menu frames that screen masks do not: bars, divider
lines and tint strips at any height, including one-texel textured bars.
`edges` is `PSX_MOD_REVEAL_EDGE_LEFT`, `PSX_MOD_REVEAL_EDGE_RIGHT` or both
OR-ed together.
Only a requested side whose boundary lies on or beyond the canonical edge
(and inside the reveal) grows, so a divider's interior end stays joined to
its artwork. The band is drawn when the packet executes, keeping its
ordering-table position, colour, modulation, CLUT, texture page, texture
window and blend, and never overlaps the quad, so translucent tints do not
blend twice. Accepted packets are GP0 0x28-0x2B (5 words) and 0x2C-0x2F
(9 words) with four identical UVs; full-frame quads keep the renderer's
single full-screen expansion. An explicit HUD anchor on the same packet wins.
A tagged quad bypasses automatic UI layout even when no band is drawn; the
tags only act in native-wide mode, never in squash mode, where automatic UI
grouping runs. Tests: `ws_reveal_edge_test` (geometry, admission, tag
storage) and `ws_reveal_edge_exec_test` (real GP0 handlers, guard, expiry,
4:3, reset and state load, render-pass checkpoints, squash mode).

Radial scale must be finite and between 1 and 64. Derive it from the
producer's authored geometry and pixel aspect; scale both feathered and
solid rings together. The original transition timer remains authoritative.
At 4:3 there is no reveal transform. The older
`psx_mod_widescreen_x_margin()` returns a culling envelope and includes
safety guards, so it is unsuitable for deriving the visible fade radius.

`runtime/tests/test_ws_screen_mask.c` covers bands, whole-frame handling,
expanded-RAM identity, radial transforms, expiration and packet reuse.
The MediEvil II integration independently checked full-width museum bars
and identical subtitle glyph packets/bounds at 4:3, 16:9, 21:9 and 32:9.
That live evidence does not qualify every game's transitions or HUD.
