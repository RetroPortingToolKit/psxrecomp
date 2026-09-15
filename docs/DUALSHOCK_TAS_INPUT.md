# DualShock TAS controller encoding

PSXRTI2 preserves one Nymashock2.9.1 P1 controller input per frontend
return. It is separate from the existing PSXRTI1 digital route. The exporter
rejects console/tray events, other ports, anchored movies, malformed rows,
extra payload members and streams outside the bounded route capacity.

The little-endian 24-byte header is `8sIIII`: magic `PSXRTI2\0`, version2,
record size12, frame count, reserved0. Every `IH6B` record contains the
one-based sequential frame, active-low button word, LY/LX/RY/RX bytes,
physical Analog button0/1, and reserved0. Limits are1,000,000 records and
4,096 distinct consecutive complete states. The parser stages the entire
stream and publishes counts only after EOF validation.

The canonical digest covers seven bytes per input: little-endian button
word followed by LY/LX/RY/RX/physical Analog. It is a lossless input digest,
not a claim about guest-visible controller bytes or execution equivalence.
Physical Analog is a button, not the guest-owned digital/analog mode.

The exact source bridge needs separate delivery qualification. In BizHawk
2.9.1 (`745efb1d`), Nymashock.AddAxis writes the log byte into the high byte
of a16-bit field. The pinned Nyma Mednafen submodule (`ddf225cf`) then maps
that16-bit value with `(value *255 +32767)/65535` in DualShock.UpdateInput.
Consequently raw values129..255 deliver128..254;0..128 remain unchanged.
The older Octoshock source tree is not evidence for this Nymashock bridge.
Power starts the DualShock in digital mode with mode locking disabled.

Authored Python format/negative cases and production C parsing at O0/O2
are registered tests. The private Bio Hazard export additionally reproduced
all227,202 original inputs against an independently expanded canonical
record:2,824 segments and28 noncenter rows. Original one-based noncenter
frames are44930..44952 and45232..45236. No physical Analog press occurs.
These codec checks do not qualify native SIO, core timing or a game ending.

Native PSXRTI2 preload rejects physical Analog presses, experimental input
retiming and the older Octoshock digital ACK profile. Once the whole file
and observer configuration pass, it establishes one cold P1 DualShock,
digital mode, neutral sticks and no multitap. Card contents remain separately
bound peripheral inputs. The ordinary input boundary consumes every record
once, then supplies fully neutral buttons and axes for the declared tail.

Delivery updates only buttons and sticks. It bypasses the interactive
D-pad/stick folding and host-driven mode requests. The observer reads back
the SIO button word, all four sticks, connectivity, config capability and
reported mode after delivery. Guest mode may change through the protocol.
Missing samples, wrong axes, a disconnected or plain pad, unexpected other
devices and non-neutral tail input fail the observation.

PSXRTI2 evidence has separately named original-controller, expected-protocol
and applied-controller digests in LY/LX/RY/RX order with the admitted neutral
physical Analog byte. The expected and delivered protocol digests must match;
the original digest may differ because of the source axis conversion. Legacy
PSXRTI1 word hashes and its digital-only delivery guard retain their meaning.
Full source protocol/ACK, return-clock and retail playback qualification are
still separate gates; codec or delivery tests do not satisfy them.

## Nymashock controller ACK profile

`PSX_INPUT_ROUTE_PAD_ACK_MODEL=nymashock-1.29.0-dualshock` selects a64-cycle
ACK delay and32-cycle visible pulse for a standalone config-capable pad,
in digital, analog and config modes. It is separate from the existing
Octoshock digital profile; default behavior and card timing stay unchanged.

The authored source fixture compiled the unmodified DualShock translation
unit from Mednafen `ddf225cf63b7b355cb2ac7772450cf473f4b53ac`, with the exact
InputDevice base-method region from FrontIO. It uses ordinary controller
input and serial configuration commands, with state serialization guarded
by aborting link stubs. It does not execute a full core or a game.

The260 source transactions cover cold digital polling, entering config,
guest selection and locking of analog mode, leaving config, and all256
axis values with four asymmetric sticks. Every reply byte already matched
native before the profile;2,072 ACK requests differed (170 versus64 cycles).
With the explicit profile, every complete transaction and delay matches
at O0/O2. The default170-cycle negative control remains covered. The
registered deadline test also checks both source profiles' read-independent
32-cycle pulse, IRQ enable behavior and preservation of unrelated IRQ bits.
FrontIO's pinned source Update method supplies the32-cycle pulse contract;
the extracted base-method fixture does not independently execute FrontIO's
scheduler. Full source/native guest timing remains a separate acceptance gate.

The checked-in transaction golden is authored data. Its SHA256 after newline
normalization is `169e53e7a555e5f7f3d5ec4f755011c881412d14fc9a424da3660ce91618bd40`;
the original Windows capture hash is
`b03eef7659af34c0ff99a6493d7257c2e606285a7876723dccef47a4c50576e6`.
`tools/tasreplays/collect_nymashock_dualshock.py` reproduces that normalized
capture at O0/O2 from the pinned clean source checkout, recording compiler,
source, extracted base-region and binary hashes in a fresh output directory.

## Native launch and initial cards

`run_native.py` accepts PSXRTI2 and retains separate original and expected
protocol identities. It rejects physical Analog presses, retiming, excess
complete-state transitions and the older digital ACK profile. Completion
must match both original input and applied protocol hashes, counts and tail.
The launcher freezes PSXRTI2 bytes in its new private run directory.

An explicit `--card1 RAW_CARD --card-model nymashock-1.29.0`
stages a fresh writable 128 KiB card copy and enables
only that slot. Before guest execution the observer hashes every loaded card
buffer byte through a read-only accessor, verifies the expected hash and
absence of card2, and records `initial-cards.json`. Without an explicit card,
a PSXRTI2 run verifies both slots absent. Wrong size, presence, contents or a
short buffer read rejects the run; a staged file hash alone is insufficient.
The retired game-specific card repair and its launcher option do not exist;
no opt-out argument is required for ordinary card correctness.
See [the card profile](NYMASHOCK_CARD_PROFILE.md) for its separate device scope.

These changes admit and observe the input contract. They do not make an
unqualified core timing profile source-equivalent or establish a retail pass.
RAM and CPU capture validation remains mandatory whenever enabled.
