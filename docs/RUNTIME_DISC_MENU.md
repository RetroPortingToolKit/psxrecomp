# Changing discs during play

For games with multiple configured discs, press **Ctrl+L** or **F10**, or hold
**Select + L3** on the first controller. Choose **Disc**, change **Selected
disc**, then activate **Insert disc and resume**. Browsing does not change the
mounted image. Resume or back closes the menu without inserting anything.
Use arrows/Enter/Escape or controller D-pad/Cross/Circle.

The runtime uses its existing `PsxMenuHost` and the attached menu provider.
Maintained recomp-ui remains the default OpenGL/SDL3 provider; alternative
providers stay optional. There is no separate disc ImGui context, GL draw
callback, or competing input/pause loop when a provider is attached. If none
can attach (including software, Vulkan and the SDL2 default), one compact
framebuffer menu presents the same recomp-ui model through the normal host
overlay. Its `OpenLauncher` keyboard binding is configurable in config.ini;
the attached provider retains its own bindings.

The menu is available in release builds with recomp-ui, without a debug server.
Single-disc games retain their existing default menu behavior. Disc changes are
refused during netplay; a provider may still offer its other online-safe pages.

Every insertion, reinsert hotkey, automatic request and debug command uses
`psx_disc_tray_cycle`. The guest sees the normal open-lid/image-change/timed-close
sequence; it is not restarted. Successful changes persist the selected disc,
refresh savestate scope and the slot browser, and discard rewind history from
the old image. A failed file open leaves the drive empty and permits another
selection. The attached provider reopens with an error toast; the compact menu
stays open with its error status. Both pause paths guard closing input and
re-anchor pacing before resuming. No menu writes guest VRAM or story state.

The host's mounted-media resolver makes title plugin file/extent reads follow
the current roster entry, including sector-padded resident reads. An empty
drive returns failure rather than silently reading the boot disc. Existing
prepared-image behavior and long-read watchdog heartbeats are retained.

## Optional automatic switching

A trusted active offline plugin can register `psx_mod_set_disc_request_provider`
from its activation callback. It returns the requested roster number throughout
the guest's request episode and separately reports safe lid-wait readiness.
The title owns the addresses and instruction guards; the framework contains
no title checks. The callback runs with its owning plugin context.

The host polls at VBlank and makes one attempt per request episode. Readiness
changes do not rearm failed requests. Missing media leaves manual recovery
available. No provider is called before the game starts or during netplay.
A new plan/activation clears the provider; `psx_disc_tray_reset_session` clears
request suppression before every boot, including soft restarts.

## Diagnostics and validation

The always-on 32-entry tray ring retains successful and refused attempts across
sessions. `disc_tray_log`, `disc_select`, and `cd_reinsert` use the production
policy. `runtime_menu` automates only the compact fallback; it cannot open a
second menu behind an attached provider. `menu_host` reports the attached
provider and pause state. `host_key` and `host_button` inject SDL events through
the ordinary provider input path, without editing guest pad RAM.
`session_restart` queues an offline, unpaused teardown/commit/activation/boot;
optional `n` selects a roster disc for that next boot. It tests a real second
session without requiring a lobby. The command replies before the host acts.

`runtime_disc_menu_test` covers browse/cancel, provider/fallback exclusion,
insertion, repeated accepts, missing-media recovery, rewind invalidation,
persistence, online refusal, request restart and ring wrapping. The mod-runtime
fixture covers activation ownership, boot/online suppression, plan reset,
mounted file/extent/resident reads and empty-drive refusal. Host menu, overlay
and shortcut tests cover their shared contracts. Real title integration must
also exercise an actual disc transition, controller navigation, renderer
fallback, missing-media recovery and a soft restart on the resulting head.

Tracks `beads-eio.3.178`, `beads-eio.3.342` and `beads-eio.3.365`.
Disc images remain user-provided assets.
