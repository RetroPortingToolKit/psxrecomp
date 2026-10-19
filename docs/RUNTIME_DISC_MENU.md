# Changing discs during play

For games with multiple configured discs, press **Ctrl+L** or **F10**, or hold
**Select + L3** on the first controller. The shared recomp-ui game menu opens
over the running game. Choose **Disc**, change **Selected disc**, and activate
**Insert disc and resume**. Browsing the disc numbers does not change media.
**Game > Resume**, the opening hotkey, or backing out closes the menu without
inserting anything. Arrows and Enter, controller D-pad/Cross/Circle, and mouse
navigation are available in the OpenGL presentation.

The launcher Hotkeys page can rebind `OpenLauncher` (`config.ini [KeyMap]`).
A custom binding replaces both keyboard defaults; `None` disables them.
The menu is included in release builds with recomp-ui and needs no debug server.
Single-disc games keep their existing shortcuts and presentation. Disc changes
are unavailable during netplay.

Insertion uses the existing CD lid cycle: stop old transfers, show the open lid,
replace the mounted image, then let the guest clock close the lid. It does not
restart the guest. A successful change persists the selected disc and path,
updates the active session's media, changes savestate scope, refreshes its slot
browser, and drops rewind history for the previous disc. A failed file open
leaves the menu open with an error and the drive empty, allowing another choice.

The host drains the OpenGL render/present workers while the menu owns the
context, draws recomp-ui's ImGui presentation, then restores the workers and
guards held closing inputs. Software and Vulkan use the shared model's compact
framebuffer presentation. The UI owns no game state and edits no guest VRAM.

Every tray attempt is retained in an always-on 32-entry ring, including invalid
indices, missing images and netplay refusals. The debug `runtime_menu` command
operates on the same model (`open`, `close`, `select` with `n`, `insert`, or
semantic `input` with `button` 0..6) for isolated automation; `disc_select` and `cd_reinsert`
use the same production tray policy. Ordinary headless runs remain unchanged.
An explicit `--headless-opengl` run can capture an open menu with `present_shot`.

`runtime_disc_menu_test` exercises the real UI model and tray against a fake
drive: browse/cancel, insertion, repeated accepts, recovery from missing media,
rewind invalidation, persistence notification, netplay refusal and ring wrapping.
`host_keymap_test` covers shared defaults, rebinding and explicit unbinding.

Tracked by `beads-eio.3.178`. Disc images remain user-provided assets.

Validated on LoD NTSC-U: real OpenGL menu at 640x480, cancel without mounting,
Disc 1 -> 4 -> 1, repeated open/close, and a real Disc 4 load prompt completing
into the Moon field through the shared tray. UI model/keymap tests pass; shared
UI desktop/touch layout tests pass. Physical-controller and Vulkan/software
visual checks remain for the staged playtest.

## Optional automatic switching

A title plugin can register `psx_mod_set_disc_request_provider`. It returns the
requested roster number throughout the guest's disc-request mode, separately
reporting whether the guest has reached a safe lid-wait state. The framework
polls this at VBlank and uses the same tray as manual insertion. The title owns
all guest addresses and code guards; the framework contains no title checks.

A request gets one automatic attempt, even if its readiness changes later.
A failed image open displays an error and leaves manual selection available.
No request, an already-mounted disc, an inactive plugin, boot, and netplay do
not trigger a change. Providers clear when the mod session resets or activates.
`disc_tray_log` reads the always-on ring, including `origin: auto` events.

Tracked by `beads-eio.3.342`.

Automatic request suppression is session-local. The host calls
`psx_disc_tray_reset_session()` before each boot, including a soft restart; the
process-lifetime tray event ring remains available for diagnosis.
