# Native PS1 pointer devices and neGcon

The runtime can present a PS1 Mouse or GunCon to the guest through SIO. This is
separate from a game's mouse-to-stick mod. Configure a seat in `settings.toml`:

```toml
[controller]
p1_device = "mouse"
p2_device = "keyboard"
mouse_sensitivity = 1.0
mouse_capture = true
```

Mouse sensitivity accepts values from 0.05 through 10. Invalid values keep the
default. Ctrl+F10 releases or captures the pointer; change `MouseCapture` in
`config.ini`'s `[KeyMap]` section to rebind it. F10 keeps its existing launcher
action. Losing focus or opening a host menu releases capture and clears input.
Short clicks that end between host input samples reach the next guest poll.
Unpolled clicks expire after a quarter second of guest time.

### Rescue Shot

Build the title with the shared runtime and select **PS1 Mouse** for Player 1
in the launcher. Keep `p1_device = "mouse"` if the launcher already saved that
choice. The title can drop the original mouse patches and the
`supports_ps_mouse` entry from `game.toml` when switching to the shared runtime.
In the guest menus, check pointer movement and clicks. In a stage, check
aiming in both directions, single shots, repeated left clicks, and release.

Use `p1_device = "guncon"` for absolute pointer aiming. Its coordinates follow
the guest display ranges and the displayed image's letterbox. The default
`keybinds.ini` `[guncon]` actions are trigger = Mouse1, A = Mouse3 or A,
B = Mouse2 or D, no_light = Mouse4, and offscreen_shot = W. Both bindings of
each action persist. Outside the image, during a host menu, or without focus,
the guest sees no light. Offscreen shot keeps the trigger pressed for reload.
Native pointer input is disabled during netplay.

Use `p1_mode = "negcon"` with a keyboard or gamepad source for a neGcon. Twist
uses the left stick or its keyboard bindings. I/II use the right/left triggers
or Cross/Square; L uses L1. Existing title locks and controller mod policies
keep precedence. `game.toml`'s mode vocabulary is unchanged.

The paired launcher advertises these choices only when the runtime explicitly
provides their capabilities. A one-player Mouse title can keep a keyboard or
gamepad in the other port and swap the Mouse between ports. Existing public
automatic controller mode and NeGcon/JogCon protocol behavior are retained.

Validation covers native polling, ACK edges, unsupported commands, multitap
payloads, focus/menu/netplay guards, production host sampling, settings reload,
and binding persistence. It does not establish compatibility with every title
or replace game-specific play checks.
