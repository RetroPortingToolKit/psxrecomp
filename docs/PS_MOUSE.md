# PlayStation Mouse (SCPH-1030)

The runtime can put an emulated Sony PlayStation Mouse in a controller port and
drive it with the host (PC) mouse. Games that support the original mouse detect
it over SIO0 exactly as on hardware: it is a real SIO device with its own ID,
not a right analog stick fed from mouse motion. The existing optional
right-stick mouse policy for mod titles ([LOCAL_MOUSE_POLICY.md](LOCAL_MOUSE_POLICY.md))
is unchanged and independent.

It is off unless a port is configured as `"mouse"`; default behaviour, other
controller types, memory cards, multitap and rumble are unaffected.

## Enabling it

A title that works with the mouse declares it in its `game.toml`:

```toml
[controller]
supports_ps_mouse = true
```

The launcher then offers **PlayStation Mouse** as Player 1's input source
(see Launcher). Without the key the source list is unchanged.

Independently of that key, a port's device can be set to `mouse` in the
existing `[controller]` configuration. There is no separate configuration
system.

Per install, `settings.toml` next to the executable:

```toml
[controller]
p1_device = "mouse"      # PlayStation Mouse in console port 1
p2_device = "keyboard"   # optional: a pad in port 2
```

Or as a title default for fresh installs and launcher-less boots, `game.toml`:

```toml
[controller]
p1_device = "mouse"
```

`settings.toml` overrides `game.toml`. Accepted spellings (case-insensitive):
`mouse`, `ps_mouse`, `playstation_mouse`. `pN_mode` and `pN_deadzone` are
ignored for a mouse port.

### Launcher

For a title with `supports_ps_mouse = true`, Player 1's source list reads
None / **PlayStation Mouse** / Keyboard / gamepads. Choosing PlayStation Mouse
saves `p1_device = "mouse"` in `settings.toml`, and the next start shows it
selected again; Keyboard, None or a gamepad save that device as before.

This uses recomp-ui's mouse source (`GameInfo.has_mouse_controls` with
`mouse_source_label = "PlayStation Mouse"` and `mouse_source_only = 1`, behind
`RECOMP_LAUNCHER_HAS_MOUSE_SOURCE`): the entry is the keyboard-family source
with `Settings.mouse_enabled = 1`, and the mouse-aim card (sensitivity, invert,
button binds) other systems use is not shown, since the PlayStation Mouse has
fixed buttons and 1:1 motion. Pokemon Snap's "Keyboard + Mouse" is unchanged.

The entry exists for Player 1 only (console port 1). Port 2 is set with
`p2_device = "mouse"`.

Without the entry (a title without `supports_ps_mouse`, or a recomp-ui older
than `RECOMP_LAUNCHER_HAS_MOUSE_SOURCE`), a mouse port configured in
`settings.toml` is shown as **Keyboard**, and leaving it on Keyboard keeps the
mouse; choosing None or a gamepad replaces it.

### Port assignment

Any standalone console port (1 or 2) can hold the mouse. Ports 3-5 exist only
behind an SCPH-1070 multitap; a multitap seat always presents a digital pad
instead (see Limitations). With a single multitap, the lone pad on the opposite
console port may be the mouse.

## Capture and release

When the game window is focused, **left-click inside it** to capture the mouse
(relative mode; the cursor is hidden and confined). That first click is not
sent to the game. While captured:

* motion moves the PlayStation Mouse, left/right buttons are its two buttons;
* these events are not seen by other mouse consumers (menus, mod policy).

The capture is released, and any motion or clicks not yet delivered are
dropped, on:

* **Esc**;
* window focus loss, hide or minimize; host mouse removal (SDL3);
* any frame that is not live gameplay: in-game/launcher menu, save-state menu,
  rewind, save-state input guard, debug input injection, headless, netplay;
* save-state load and runtime shutdown.

Click in the window again to recapture.

## How it works

```
SDL mouse events ──► ps_mouse_sdl.cpp (capture, focus, Esc)
                      │ xrel/yrel, buttons
                      ▼
                    ps_mouse_input.c  host accumulator (float, per-frame latch)
                      │ once per sampled frame: dx,dy (int8), buttons
                      ▼
  main.cpp capture_pad_slot_view ──► PsxNetPad{type=SIO_PAD_MOUSE, lx=dx, ly=dy}
                      │ pad_ext_resolve (title transform / stick mouse skipped)
                      ▼
                    psx_pad_apply_to_sio ──► sio_request_pad_type + sio_set_pad_mouse
                      ▼
                    sio.c  device accumulator ──► 0x42 poll on SIO0
```

### SIO protocol

Verified against PSX-SPX (Controllers and Memory Cards, "Mouse") and Mednafen
`psx/input/mouse.cpp` (the Beetle PSX core); DuckStation's
`playstation_mouse.cpp` agrees on ID, order and button bits.

| TX   | RX (mouse)                  | ACK |
|------|-----------------------------|-----|
| 01h  | HiZ (FFh)                   | yes |
| 42h  | 12h (ID low)                | yes |
| 00h  | 5Ah (ID high)               | yes |
| xx   | buttons bits 0-7 = FFh      | yes |
| xx   | buttons bits 8-15           | yes |
| xx   | X motion, signed, +right    | yes |
| xx   | Y motion, signed, +down     | no  |

Button halfword, active low: bit 10 = right, bit 11 = left; bits 0-7 and 12-15
read 1, bits 8-9 read 0, so the idle high byte is FCh (left F4h, right F8h,
both F0h).

* Any command other than 42h (including the DualShock config commands
  43h-4Fh): the mouse has already shifted out 12h with the command byte, does
  not ACK, and the transaction ends. It never enters config mode, has no
  motors and ignores the 42h data bytes.
* Motion accumulates in the device between polls. Each poll reports at most
  -128..+127 per axis and keeps the remainder for the following polls; the
  accumulator saturates at 30 polls' worth (±30·128), as in Mednafen.
* Reported motion is consumed by the poll that sends it; it is never sent
  twice. The frame is built when 42h arrives.
* A button pressed at any time since the previous poll is reported by the next
  poll, even if already released; a held button is reported every poll and a
  release appears on the next poll. (Mednafen reports releases one poll late;
  this implementation does not.)
* Without a poll for a quarter second of guest time (33868800/4 cycles) the
  device drops owed motion and latched clicks (Mednafen's `clear_timeout`), so
  motion made while a game is not polling (loading) is not replayed later.

### Device switching

The mouse is its own device class, like the NeGcon. A type change requested
per frame is applied only at an idle SIO bus boundary (never mid-transaction or
mid config handshake). Switching to or from the mouse is a device swap: it
applies even over a game's 0x44 mode lock and clears the config latch, the
lock, the rumble map and all owed mouse motion and clicks. Unplugging a port
also clears them.

### Host side

`ps_mouse_input.c` sums SDL relative motion as floats (fractional parts are
carried, which matters for high-DPI and scaled SDL3 deltas), and latches once
per sampled frame at most one signed byte per axis into the frame, keeping the
rest. Owed host motion is bounded at the same 30·128. A click shorter than a
frame still reaches the guest. Every mouse port reads the same latched frame.

### Save states, reset, rewind

The device state (owed motion, current and latched buttons, clear-timer
reference) is an appended section of the SIO snapshot, so rewind and save
states restore it; snapshots from before this section still load and restore
a mouse that owes nothing. `sio_init` (reset) clears it. Host-side owed motion
is dropped on save-state load and on the save-state input guard.

### Netplay

The mouse is **offline only**. Netplay capture treats a mouse port as having no
device, and `SIO_PAD_MOUSE` stays above `PSX_NETPAD_TYPE_MAX`, so it is never
put on the wire and a blob carrying it decodes as digital. Relative motion is
deterministic once it is in a `PsxNetPad` (it is a pure per-frame input), so
netplay support would mean raising the wire type limit for peers that all
understand it plus lobby negotiation; that is not done here.

## Supported platforms

Windows, macOS and Linux through the runtime's SDL2 or SDL3 backend (relative
mouse mode: `SDL_SetRelativeMouseMode` / `SDL_SetWindowRelativeMouseMode`).
Touch and pen pseudo-mice are ignored. On Wayland, relative mode requires the
compositor's pointer-constraints support, as for any SDL game.

## Limitations

* Multitap seats never present a mouse (no verified reference for the mouse in
  a multitap bulk 80h frame); they fall back to a digital pad.
* Offline only (see Netplay). Self-check record/replay also stores the pad type
  through `PSX_NETPAD_TYPE_MAX` and so does not replay mouse input.
* All mouse ports share the one host mouse; there is no per-device routing.
* No sensitivity setting yet: one host relative unit (SDL `xrel`/`yrel`) is one
  PlayStation Mouse unit, like Mednafen's raw mickeys.
* The launcher entry covers Player 1 only; port 2 is configuration-only.
* When the optional mod mouse policy is installed and another port uses it,
  the PlayStation Mouse's capture takes the acquiring left click first.

## Tests

* `sio_mouse_protocol_test` (+ `_multitap_test`, `PSX_MAX_PLAYERS=5`): ID,
  status, frame length and ACK pattern, neutral frame, left/right press and
  release, click latching, ±X/±Y, summing between polls, no double reporting,
  large motion spread over polls and the accumulator bound, the stale clear,
  unsupported commands (no ACK, no config, motion kept), no rumble, swaps with
  DualShock (incl. a locked one), digital, NeGcon and JogCon, unplugging,
  snapshot round trip and pre-mouse snapshots, multitap seats.
* `ps_mouse_input_test`: host accumulator (sum, fractions, clipping and carry,
  bound, NaN, clicks, reset).
* `psx_pad_apply_test`: the shared PsxNetPad -> SIO path for the mouse and that
  netplay normalization never yields it.
* `pad_external_input_test`: a mouse port skips the title transform and resets
  the right-stick mouse policy.
* `launcher_device_roundtrip_test`: the `mouse` device survives the launcher,
  and the PlayStation Mouse entry seeds and writes back `p1_device`.
* `recompiler_patch_test`: `[controller] supports_ps_mouse` is parsed and off
  by default.
* recomp-ui `launcher_mouse_source_test`: Snap's "Keyboard + Mouse" unchanged;
  the host-labelled, source-only entry keeps the host's choice and seeds no
  mouse-aim defaults.
* Existing controller tests (digital, DualShock rumble, JogCon, NeGcon, card,
  multitap) are regression coverage; their legacy-snapshot offsets account for
  the appended mouse section.

## Checking a title

1. Pick **PlayStation Mouse** in the launcher (title with
   `supports_ps_mouse = true`), or configure `p1_device = "mouse"` (or
   `p2_device`, per the game's manual).
2. Start the game; the OSD says "click in the window to capture".
3. Detection: most mouse games show a cursor or a mouse option only when a
   mouse is found. With a debug-tools build (`-DPSX_DEBUG_TOOLS=ON`), the
   `pad_status` command on port 4370 reports `"type":4` for the port and
   `mouse_owed` (motion not yet polled); `emu_sio_trace` shows `01 42` answered
   with `FF 12 5A FF FC ..`.
4. Capture, move and click; check that the cursor follows in both axes with the
   right sign and that left/right buttons act.

### Example: Rescue Shot

Rescue Shot supports the PlayStation Mouse. To try it with this runtime:

1. Build the title against this framework checkout (the title's `psxrecomp`
   submodule pointing at this branch), e.g. from the title root:
   `cmake --build build-release --target RescueShot_Recompiled`.
2. Add `supports_ps_mouse = true` to the title's `game.toml` `[controller]`
   section and pick **PlayStation Mouse** for Player 1 in the launcher, or set
   `p1_device = "mouse"` in `settings.toml` next to the executable.
3. Start the game, click into the window, then check that the game reports a
   mouse, that the crosshair follows the mouse, and that left/right buttons
   shoot and perform the secondary action.

Whether the game recognises the mouse and plays correctly has to be confirmed
in an actual play test.
