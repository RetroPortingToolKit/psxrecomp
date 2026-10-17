# Plain digital command-byte response

The protocol behavior was checked against the public
[BizHawk Octoshock gamepad implementation at c9de9a8bff0f](https://github.com/TASEmulators/BizHawk/blob/c9de9a8bff0f95114df7e1d13d4dab820388dc60/psx/octoshock/psx/input/gamepad.cpp).
It preloads the digital ID after the address byte; an unsupported command
terminates the reply without another ACK. The command does not retroactively
replace the ID already shifted out.

This is a new implementation in the existing
[upstream SIO state machine](https://github.com/RetroPortingToolKit/psxrecomp/blob/7cddab8237e1f73b7067b8401af602d90a96b80d/runtime/src/sio.c),
authored by Alexandros Mandravillis (Alexbeav). No emulator implementation,
reference fixture or retail input is copied. The test's inert link collaborators
follow the existing upstream DualShock test.

Only a connected, standalone, non-config-capable digital pad's unsupported-command
receive byte changes. DTR session lifetime, delayed ACK eligibility, source-specific
timing, memory cards, analog devices and multitap command policy are excluded.
The normal 42h poll is unchanged.

Validation on the current upstream base uses all 256 command bytes on both ports,
normal button replies, absent devices, config-capable digital/analog exclusions,
and five/eight-player multitap builds with a positive connected-pad poll. The base fails the 510 unsupported-command
receive-byte checks; the fixed source passes. Existing DualShock rumble, JogCon
and NeGcon regressions are also run. These are synthetic protocol checks; no
game boot, save/load or gameplay qualification is claimed.
