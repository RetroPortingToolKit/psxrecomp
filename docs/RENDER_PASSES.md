# Stereo and own-view render transactions

Temporal replay for frame interpolation is retired. `psx_mod_render_pass_plan`
returns no phases, `psx_mod_render_pass` refuses the request, and the old
presenter cannot allocate or publish temporal image generations. Use the
[source-geometry presentation API](FRAME_GENERATION.md) for interpolation.

The reversible transaction machinery remains for two independent features:

- [Stereo rendering](STEREO_RENDERING.md), including [OpenXR](OPENXR_RENDERING.md),
  retains a completed pair of eye images.
- `psx_mod_render_local_view` retains the local player's view in a presentation
  surface separate from canonical VRAM; `psx_mod_render_local_pair` produces an
  offline pair of independent views.

These calls preserve the simulation through the existing guest-state checkpoint,
frozen-time scope, bounded store policy and VRAM journal. Device writes that
cannot be restored are refused. A kept local view retains only its presentation
colour; canonical CPU VRAM, raw pixels, stencil and device state are restored.
Stereo captures each eye before restoring the transaction. Neither path uses
interpolation history, temporal phases or an image-blending scheduler.

Backend, session, reentrancy and scene-resource guards remain mandatory. Refusal
must leave the ordinary native presentation available. Save/load and watchdog
recovery must restore nesting and discard incomplete view pairs.

Validation includes the store-policy/stereo-freshness test, sandbox guard checks
and the hidden real-GL readback fixture, which verifies stereo backup/journal
restoration independently of the retired temporal controls.
