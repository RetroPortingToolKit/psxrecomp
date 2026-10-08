# Optional local mouse policy

The SDL-free `PSXModMousePolicy` interface provides one trusted local P1 policy.
Register it from an opt-in activation plugin. Registration copies the policy,
rejects incompatible struct sizes, missing callbacks and a second policy,
and returns success/failure. Existing const-input controller presentation is
unchanged. The generic runtime has no game addresses or gesture recognizer.

`eligible(native_buttons)` supplies the game's current persistent-state
predicate. `event()` receives ordered reset/acquired/motion/hold edges with
nanosecond event times and fractional displacement. `sample(now, output)` is
a non-consuming query and may override only the right axes. The output is
initialized to neutral; its size, override flag and byte bounds are checked.

SDL event delivery and relative capture remain on the existing SDL owner
thread. SDL3 timestamps share SDL_GetTicksNS's epoch. SDL2 millisecond event
timestamps are lifted into the nearest current 64-bit era; older SDL2 versions
use a private owner-thread tick extension. No SDL public types enter the game
interface. No secondary event queue or generalized arbitration is added.

Only a fresh game-window LEFT mouse-button down can acquire capture, with focused,
visible, eligible, connected analog P1 local gameplay. The acquisition source
and captured Escape are suppressed through release before keyboard/mouse
binding folding. LEFT release immediately releases capture and resets the
policy, including a separately held direction; it is never timestamp-gated.
Both normal edges and emergency Escape are silent. Only genuine capture/config
failures produce bounded diagnostics. Suppression is per source/player, so a controller holding
the same PSX action remains intact. Primary and alternate P1 bindings are
inspected for hold-control conflicts without rewriting them. Ordinary bindings
remain live; a conflict disables only hold interpretation with a notice.
Physical down-state and accepted hold-state are separate. Only an accepted
hold receives HOLD_RELEASE; conflicting or acquisition-blocked releases leave
a free flick intact. A polled host-hotkey keyboard view also excludes consumed
P1 sources, so captured Escape cannot activate a configured Turbo=Escape.
The original SDL state, controller shortcuts and P2 folds remain available.

Local P1 RX/RY are applied after all native folding and presentation, before
the existing PsxNetPad/SIO publication. Whole-vector native right-stick
activity releases mouse capture and retains the native bytes. Other seats,
buttons, left axes and report type/order are not rewritten. With no policy,
the existing post-pacer SDL_PumpEvents timing is retained.

Focus loss, hide/minimize, host or game menus, reset, state load, rewind,
routing/removal, invalid data, capture failure and >250 ms host stalls release
capture and reset the policy. Stale, backwards and future motion cannot
extend a pulse. Releases remain immediate even with anomalous timestamps;
their watermark cannot admit old motion after a cleared held target. A
valid ordered LEFT release while capture remains eligible uses its actual
event timestamp as the acquisition boundary. A later physical press in the
same delayed SDL drain can therefore acquire a neutral new session. Lifecycle,
stall and malformed-release resets retain the processing-time barrier; a release
after such a reset cannot lower it to admit queued input. A
stall's backlog cannot acquire capture again. Eligibility recovery needs a
release/repress of LEFT and fresh motion. Reset retains physical LEFT down-state,
so holding it across an interruption cannot recapture. Debug input injection, headless, netplay, replay/resimulation and
render-only passes inhibit this policy.

`psx_local_mouse_clear()` unregisters the policy during existing mod/session
reset. Source suppression already owed through release survives unregister,
so disabling a mod cannot leak a held Escape into a native binding. There is
one P1 policy; physical device locking and P2 mouse controls are out of scope.

## Validation status and targets

This release backport retains the original mouse policy and the maintainer's
missed-LEFT-release/focus-regain correction. It adopts the ordered external-input
layer from [psxrecomp #510](https://github.com/RetroPortingToolKit/psxrecomp/pull/510)
and only its required offline controller-source dependency from #495:

1. Capture the physical/local pad.
2. Resolve an attached offline controller source and the controller presentation
   policy. Physical and source buttons combine as active-low input.
3. Apply the mouse policy once to the resolved P1 right axes. A source cannot
   capture the mouse and then discard its result. A deflected native/source
   right stick takes over as a whole vector; a digital source or Start disables
   capture under the existing eligibility rules.

Detach/reset release frames, input guards and device absence reset capture.
Declined/invalid sources remain neutral and do not suppress physical buttons.
P2, pad type, buttons and left axes retain their ordinary delivery. Registration
is restricted to the main thread and validates the ABI structure size.
The release visibility and `HOST_KEYMAP_CAPTURE_MARK` guards are retained.
There is no master-range merge, game-specific runtime address or new gesture API.

The matched release pair passed a fresh Windows x64 matrix with MSVC 19.50,
CMake 4.1.2 and Ninja 1.12.1: 34 operations, 32 test executions, 18 unique names,
eight CTest inventories and both unchanged native BIOS fingerprints.

- Six game-focused tests cover reducer geometry/timing, guest eligibility,
  capture lifecycle, activation, stick response and the preloaded catalog.
- Three controller-source/resolver tests cover registration, neutralization,
  attach/decline/detach/reset, precedence, routing and eligibility gates.
- Host keymap, keyboard chord and mouse suppression tests pass separately on
  SDL3 3.4.10 and SDL2 2.32.10, with debug tools enabled and disabled. The real
  main, mouse adapter and game plugin translation units compile in all four
  configurations.
- Six native SIO protocol/boundary/IRQ checks pass with active assertions.
- A full offline SDL3 Release game/runtime build links and its five selected
  game tests pass. Fatal BIOS fingerprint validation remains enabled.

`local_mouse_policy_test` uses a deterministic fake capture backend; the source
bridge and host-fold tests use controlled input. Automated results do not prove
physical relative capture, controller feel, live gadget use or gameplay.
Non-Windows capture behavior has not been exercised on a physical device.
Hosted CI results must be read separately from local results.

The runtime PR targets `release/ape-v0.5.0-hle-support` at
`065888f50f9131839bcbbc8814debf58b4624b16`. The companion
[Ape Escape PR #21](https://github.com/mstan/ApeEscapeRecomp/pull/21) deliberately
updates both its `psxrecomp-v4` gitlink and `framework_pins.txt` to the same
runtime commit. Consume the runtime PR first and keep that exact commit
reachable when merging the game pin. Runtime-only changes need a rebuild,
not BIOS or game C regeneration. Release packaging metadata records the
historical v0.5.0 binary and is unchanged.
