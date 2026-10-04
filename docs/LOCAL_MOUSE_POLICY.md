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

The preceding click-toggle candidate passed a full SDL3 Release link, focused
tests, both SDL backend input checks and a private cold-start smoke check.
The LEFT-held activation revision passes the focused policy/reducer integration,
both SDL backend input checks, native SIO checks and a full incremental SDL3
Release link with five registered game tests. Its new private candidate passes
static import/profile checks; physical startup/interruption validation remains
pending. `local_mouse_policy_test` supplies a deterministic fake backend and
covers capture, neutral acquisition, releases, host/guest gates, bad times,
stalls, conflicting hold, failure fallback and registration validation.
`mouse_binding_suppression_test` folds the real keybind code with a controlled
mouse poll, covering primary/alternate/button/axis consumption through release
and independent controller/P2 actions. `keyboard_pad_chord_test` includes
captured-Escape suppression and fresh native re-presses.
It also checks repeated Turbo=Escape polls and the independent P2 report.

Run these and the existing controller presentation, stick mapping, netplay
pad codec and SIO tests with both supported SDL backends. Compile the actual
adapter/main path with debug tools enabled and disabled. Observe relative
capture/focus/menu/removal and normal report delivery in a private game copy;
pure policy tests do not exercise the OS or game dispatcher.

The reviewed implementation base is release framework commit
065888f50f9131839bcbbc8814debf58b4624b16, also the canonical
`release/ape-v0.5.0-hle-support` branch. Current upstream master has differing
existing mod APIs and needs separate compatibility validation. The linked game
draft pins this runtime draft's reachable head through its configured framework
upstream; final-pin checkout, build and local test validation are recorded with
the drafts. Hosted CI status and physical interruption checks are reported
separately; this is not a
public-quality or merge-readiness claim.
