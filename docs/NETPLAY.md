# Netplay with recomp-net (psxrecomp)

This is the **feature overview** for rollback-capable multiplayer in PSX
title projects that opt into `-DPSX_NETPLAY=ON` and ship `lib/recomp-net`
(plus lobby client). It describes what we are introducing for players and
title developers — not the internal soak checklist.

Deeper / implementation docs:

| Doc | Role |
|-----|------|
| [`NETPLAY_TOPOLOGY.md`](NETPLAY_TOPOLOGY.md) | Locked online vs LAN topology |
| [`ROLLBACK_MOTK_HOOKUP.md`](ROLLBACK_MOTK_HOOKUP.md) | MotK rollback integration notes |
| [`config_schema.md`](config_schema.md) `[netplay]` | Disc gate fields (`require_cue`, tracks, fingerprint) |
| [`lib/recomp-net/README.md`](../lib/recomp-net/README.md) | Transport / session library |
| Lobby server `WS_LOBBY.md` (recomp-net-server) | WebSocket lobby + SFU policy |

Opt-in at configure time: see [`GAME_PROJECT_SETUP.md`](GAME_PROJECT_SETUP.md)
(`PSX_NETPLAY`, `ENABLE_NETPLAY_IF_PRESENT`). The launcher NETPLAY button only
appears when the host advertises `GameInfo.netplay_supported`.

---

## Rollback netcode

Titles that opt in with `[netplay] content_negotiation = true` (game.toml,
default false) get content negotiation; every other title keeps vanilla
netplay (mods cleared), the MOTK1 LAN protocol and plan-free lobby caps.

Online rooms can publish a host-selected mod plan: package versions, enabled
features/options and a portable plan fingerprint. Every peer resolves and
verifies that plan locally before launch. External media is allowed only when
the manifest declares its exact size and SHA-256 and resolution creates a
verified immutable byte snapshot; path-only resources and folders are refused.
Paths and donor bytes are never exchanged. Local bindings may differ, but the
verified content must match. The guest's offline selection is restored on return
to the launcher and is not overwritten by the session. LAN / Direct IP retains
the vanilla-content policy.

The read-only debug command `netplay_status` reports active mode, player count,
simulation tick, matched-state watermark, admission stall and input desync state.
Headless peers accept the usual injected local pad inputs through the same
session-slot routing as windowed peers.

Default match mode for opted-in titles is **rollback** (GGPO-style tip
prediction + resimulation), with **delay-sync** still available as an opt-out
(“Disable Rollback” in lobby settings, or `PSX_NET_MODE=delay`).

- Peers exchange pad tips over the session; missing remote input is
  predicted for a short horizon, then corrected by resim when the real tip
  arrives.
- Input delay (D) and prediction depth (P) can be auto-derived from RTT or
  set manually in the lobby.
- Savestate / snapshot rings and AV digests keep peers aligned; FMV / media
  paths use lockstep-friendly rules so digests stay meaningful.

Rollback is the product default for titles that ship it (e.g. MotK). Delay-sync
remains useful for debugging and for hosts that prefer fixed lag.

- With three or more seats, a tick counts as confirmed (hash confirm, the
  agreed watermark, a silently promoted button correction) only when every
  other peer has committed the same digest for it
  (`rnet_hc_set_peer_mask`). Peers that made the same wrong prediction for a
  slow seat agree with each other, so confirming against whichever peer spoke
  last let them skip a rollback the slow seat's real input needed.
- A peer that falls behind the others' inputs sheds its own view first
  (`psx_netplay_local_view_shed`, presentation only); its rollbacks then stay
  short for everyone. `PSX_NET_LOCAL_VIEW_SHED=0` keeps the own view.
- A match ends after `PSX_NET_ADMIT_STALL_MS` (default 20000) without any
  progress; a remote input tip that advances is progress, so a slow peer is
  waited for. A peer silent for `PSX_NET_LIVENESS_MS` (default 1500) counts as
  gone.

---

## Seats vs session slots

Lobby chat is masked for profanity and slurs on every path: the server
filters relayed lines, and every client runs recomp-net's
`rnet_chat_filter_apply` on each line it puts in its ring -- so LAN rooms
(host-relayed `MOTK5 CHAT`) are filtered without a server. The list is
`lib/recomp-net/data/chat_filter_words.txt`; `RNET_CHAT_FILTER=0` disables
the client pass.

The lobby browser also carries a per-game server chat (server op
`server_chat`, relayed to every client listed for the same title) and lists
only the players online for this title; both are online-only, since a LAN
room has no server. The same emoji and profanity handling applies.

Players can move themselves in the lobby and ask to swap seats (online via
the server's `seat_move` / `seat_swap_request` / `seat_swap_answer`; on LAN
via `MOTK5 SEATMOVE` / `SWAPASK` / `SWAPANS` / `SWAPRES` relayed by the host),
so the lobby host can hold any seat. The session is planned at launch
(`ae_np_plan_session_slots`): the host is always **session slot 0** — the
seat every host-only path keys on — and the other players follow in lobby-seat
order; each session slot drives the controller port of its lobby seat
(`PsxNetplayConfig.port_of_slot`), so the game sees a player where the lobby
seated them. Session slots are therefore compact (no holes) even when the
lobby is sparse; the sparse ports are what the game sees.

## Host in the spectator table

Online rooms let the host watch from the gallery and still run the match. The
host keeps session slot 0 — the seat every host-only path keys on (save
states, card sync, the start barrier, overlay host controls) — but its pad is
muted (`psx_netplay_stage_local` substitutes "no controller") and slot 0 maps
to no controller port; player seats sit at lobby seat + 1 and drive port
slot − 1. The server publishes `host_spectates` with the launch and sizes the
input relay for the extra forwarded slot; every peer derives its session slot
from that one flag. The session can therefore hold `PSX_MAX_PLAYERS + 1`
slots. LAN rooms have no gallery, so this is online-only.
Env for command-line sessions: `PSX_NET_HOST_SPECTATES=1` on every peer.

## Bring your own memory card (seat 2)

By default a match runs on the **host's** memory-card choices: at launch the
host hashes / sends both of its cards to every guest (the `SRAM` state op), and
guests play from a sandbox copy so their own cards are never written.

Some games need each player's *own* card in the machine — Yu-Gi-Oh! Forbidden
Memories duels load each duelist's deck from a separate card. For that, seat 2
(P2) can **bring its card**:

- In the lobby a memory-card glyph sits beside P2's name. P2 clicks it to
  offer its **local slot-1** card; the host can click it to disallow / allow
  guest cards. It lights up only when both agree, and every peer sees the same
  state. Default is off (host cards only).
- At launch, before the host's card broadcast, P2 uploads that card to the host
  over the session (`MEMCARD` state op, the one guest→host transfer). The host
  installs it as the match's **slot-2** card and the normal broadcast then
  carries it to everyone, P2 included.
- The host's real slot-2 card is neither read by the match nor written: the
  host rebinds slot 2 to `<memcard_dir>/netplay/guest_card2.mcd` for the
  session and restores its own file on shutdown. P2's real cards stay untouched
  too (guests already sandbox both slots); what the match writes to slot 2
  lands in P2's sandbox copy, not in P2's personal card.
- The value is **settled once by the host at start** and delivered with the
  launch caps (`guest_memcard_active`, or the trailing `MOTK1 START` line on
  LAN), so a toggle racing the start cannot leave peers disagreeing about
  whether a card is coming.

The room page also carries a lobby chat. Online it is the server's `chat` op,
echoed to everyone seated; on LAN the host relays it (`MOTK5 CHATREQ` from a
guest, `MOTK5 CHAT` to everyone). Join, leave and kick are announced as system
lines the same way (empty sender fields on the wire mark them as system).

Env override for command-line sessions: `PSX_NET_GUEST_MEMCARD=1` on **every**
peer (a host that expects a card from a seat that never sends one waits at the
`mc_guest_wait` barrier — the stall phase names it).

---

## Hybrid graphics (determinism + present quality)

Netplay must keep **guest simulation identical** across peers. Present quality
is separate.

When **OpenGL** is selected and netplay is active, psxrecomp can run a
**dual-raster** path:

| Layer | Scale | Role |
|-------|-------|------|
| Software rasterizer | **1×** (headless authority) | Deterministic VRAM / digests / rollback snaps |
| OpenGL | Player internal resolution (Native … 8K, or legacy 2×–4×) | Window present quality only |

Cost: extra CPU for the 1× SW pass. Benefit: peers can use different GL
settings without desyncing the sim. SW-only netplay forces scale 1 for the
whole path. Offline play keeps full supersampling with no dual-raster tax.

**Internal resolution is a per-peer presentation setting.** Settings → Display →
Internal resolution is not a mod, so it is not cleared for netplay: each peer
keeps its own preset, clamped to its own GPU, and one peer at 4K next to another
at Native is a supported match. Only the GL present surface changes; the 1×
software authority, the digests and the rollback snapshots are the same on every
peer. Mods are cleared for netplay, except a player's own-view mods (below),
which stay per player and never touch the shared game.

### Each peer's own view (presentation only)

Some titles draw every player's view into one frame (split screen, or a link
mode that renders all seats). Online, every peer still renders and digests
that identical frame; only what its window shows changes:

- `game.toml [netplay] local_viewport = "vertical_split"`: during netplay, seat
  0 or 1 presents its own half of a detected vertical split.
- `psx_netplay_present_local_view(x, y, w, h)` (`psx_netplay.h`): a trusted
  game plugin names this peer's rectangle of the display area (guest pixels,
  relative to the GP1(05h) display start), usually from
  `psx_netplay_local_slot()`. The present path shows that rectangle alone,
  scaled to the window at 4:3, from the GL internal-resolution surface (or the
  software frame). The request lasts a few simulation ticks
  (`PSX_NETPLAY_LOCAL_VIEW_HOLD`), so the plugin renews it every frame its
  multi-view screen is up and menus fall back to the full frame by
  themselves. It wins over `local_viewport`, never reaches the guest or a
  savestate, and is ignored offline. The headless present-image ring records
  the same rectangle, which is how a harness checks it. Vulkan, which netplay
  replaces with the software present, ignores it.
- `psx_mod_render_local_view(cpu, rect, fn, user)` (`mod_plugins.h`): this
  peer draws its *own* image of a display rect with the game's code, for
  example its seat's single full-screen view, inside the render-pass sandbox
  (docs/RENDER_PASSES.md, "Netplay local view"). The machine is restored
  afterwards, so the canonical frame stays in the authoritative VRAM, rollback
  snapshots and digests; only the OpenGL presenter's surface keeps the image.
  A committed image cancels any `psx_netplay_present_local_view` crop
  (`psx_netplay_local_view_clear`). Forward netplay frames only; a title falls
  back to the crop when `psx_mod_render_local_view_status()` is not ready
  (software present, Vulkan, resimulation, or `FAST_FORWARD` while this peer
  is behind the match and sheds its own view).

---

## Connectivity: ICE, TURN, SFU, LAN

### Online lobbies (WebSocket + SFU)

Online matches go through the lobby at **netplay.retcomm.net**
(WebSocket control plane). Match **UDP pad traffic** for online rooms uses an
**SFU star** (selective forwarding unit on the lobby server): every peer sends
to the SFU; the SFU fans out to other seats. Peers do **not** mesh each other
for game data online.

Current lobby policy prefers **always SFU** for online starts so CGNAT /
asymmetric NAT does not strand players on failed ICE attempts.

### ICE + TURN

**ICE** (with **TURN** relay via the project’s coturn on
`netplay.retcomm.net`) remains part of the stack for discovery,
signaling, and fallbacks. With `PSX_NETPLAY=ON`, `runtime.cmake` defaults
`RNET_ENABLE_ICE=ON` (libjuice) before adding `recomp-net`. Pass
`-DRNET_ENABLE_ICE=OFF` only for LAN-only / no-FetchContent builds.
libjuice is pulled as a **pinned URL tarball** (not `git clone`) so Retro
AppImage / mismatched-libcurl hosts can still configure; offline builds can
vendor `lib/recomp-net/third_party/libjuice` or set `-DRNET_LIBJUICE_ROOT`.
“Force TURN” in the UI can raise delay floors for relay-heavy paths; it
does not replace the SFU online architecture above.

### LAN / Direct IP (P2P star)

Without a lobby start (LAN or Direct IP):

- **2 players:** peer-to-peer UDP.
- **3+ players:** **host-as-relay** local star — the session host fans out
  tips to other seats (same star shape as online SFU, but the game host is
  the hub).

Sim authority: pad **slot 0** is the session host (`START`, state transfer).
Guests rearrange among seats **1..N−1**.

**When the start fails.** A LAN host listens on the room's UDP
port (7777 by default; the browser scans 7777 to 7808), a guest on a port the
system picks, and a command-line start on the `--net-bind` address. When the
start fails, the runtime does the library's steps again (it reads the listen
address, looks up a name, and binds) and says what the system answered:

- The port is in use (Windows error 10048, Linux 98, macOS 48): "Netplay could
  not open UDP port 7777 on 0.0.0.0 (system error 10048). Another program or
  the operating system holds that port. Choose another port and start again."
- The system does not hand the port out (Windows error 10013 for a range it
  keeps for Hyper-V or WSL, error 13 for a port below 1024 elsewhere): "The
  operating system does not allow this port. Choose another port and start
  again."
- The listen address is not an address of this computer (Windows error 10049,
  Linux 99, macOS 49): "Netplay could not listen on 192.168.1.50 (system error
  10049). That address is not one of this computer's addresses. Check the
  listen address and start again."
- Any other answer: the port, the address, the system's error number and, when
  it is short plain text, the system's own words. No cause is named.

A listen address that is not `address:port` (the address may be left out:
`:7777` listens on every address) and a peer address that cannot be read get
their own sentence. The peer address is named only when the listen address
opened. An online start that fails says only that the online connection could
not start. The build is named only when it has no netplay. The log line adds
the system's own text in every case. A command-line start exits with code 1; a
match started from the launcher returns to the room with the sentence on the
status line.

---

## Multitap + seat ceiling

| Item | Value |
|------|--------|
| Library / lobby / UI ceiling | **8** seats (`RNET_MAX_SLOTS`, dual SCPH-1070) |
| Per-title cap | `game.toml` `players` / `PSX_MAX_PLAYERS` (e.g. MotK=2, Bomberman Party=5) |

**Offline:** recomp-ui exposes Multitap under Settings → INPUT on PSX titles
with `num_players >= 3`. Off hides seats beyond the two native controller
ports and caps `g_offline_pad_count` at 2 (`settings.toml` `[controller]
multitap`, default on). Multitap still arms at game-start when three or more
offline seats are live (`multitap_port` from `game.toml`).

**Multitap analog (hack):** tap seats are plain digital by default. Opt in
with `game.toml` / `settings.toml` `[controller] multitap_analog = true`
(or Settings → INPUT / Lobby Settings). When on, tap seats may report
DualShock (`0x73` + sticks) in multitap bulk status. Hosts publish
`match_caps.multitap_analog` so every peer applies the same setting at
launch. Not reliable across titles — leave off unless the game needs it.

**Netplay (psxrecomp only):** lobbies with more than **2** seats always
force SCPH-1070 multitap on (`force_session_pads_connected` /
session start when `slot_count >= 3`). The offline Multitap toggle does
not opt out of that. Empty tap seats are fine — not every slot needs a
device.

Rollback and delay-sync both carry multitap pad bytes.

**BIOS settle:** each peer advertises a BIOS offer (can run OpenBIOS, has a
retail dump and that dump's CRC-32, and whether OpenBIOS is selected) — online
on ready, LAN on JOIN. At Start the host freezes one session BIOS (`openbios`,
or `scph1001` = retail with the image's CRC) via `match_caps.session_bios` /
`session_bios_crc` (online) or the `MOTK1 START` lines (LAN). Retail is used
only when every seated peer has the same retail image; otherwise OpenBIOS if
everyone links it; otherwise the host refuses to start and says which images
differ. Peers that cannot apply the settled BIOS abort instead of falling
back. That choice boots the match only — it does not change each peer’s saved
BIOS preference. See `docs/BIOS_SELECTION.md` (Netplay lobby settle).

**Why a match ended:** a match that ends early returns every peer to the lobby
(`netplay_soft_exit`), and the launcher's status line says why, from
`runtime/src/netplay_exit_reason.c`: the other player left, the shared save
state failed or timed out, the link timed out, the games stopped running in
step, or the consoles started differently. The last one is the rollback boot
digest: when both peers' tick-0 digests are known and differ, and the same pair
holds for 3 s (`NETPLAY_BOOT_MISMATCH_GRACE_MS`), the match ends at once instead
of waiting out the 20 s admit-stall watchdog. A peer that receives the other
side's BYE while it sees the same mismatch reports the mismatch, not a
disconnect. Window close and Escape end the match with no message.

---

## Disc identity for multi-track titles

Netplay is **dump-strict** for titles that declare it. Peers must run the
**same playable image geometry**, not “any USA ISO.”

Typical `[netplay]` in `game.toml` (MotK example):

```toml
[netplay]
require_cue = true
required_tracks = 17
# optional: required_disc_fp = "…"   # TOC fingerprint
```

| Rule | Why |
|------|-----|
| Prefer / require **`.cue` + sibling `.bin` track files** | Multi-track Redump layout (data + XA/audio) |
| Exact **track count** when `required_tracks > 0` | Track-01-only dumps desync CDDA/XA vs full cues |
| Reject bare incomplete mounts when `require_cue` | Cue→bin fallback cannot invent missing tracks |
| Optional **TOC fingerprint** (`required_disc_fp`) | Same track count still wrong dump |

Generate & rebuild / prepare flows should point at the **`.cue`**, not a lone
`.bin`. See [`config_schema.md`](config_schema.md) and the release checklist in
[`GAME_PROJECT_SETUP.md`](GAME_PROJECT_SETUP.md).

---

## Related product pieces

- **Lobby UI** (recomp-ui): host/join, room settings, rollback toggles, FORCE
  TURN, player names — only when `PSX_NETPLAY` is on and the title advertises
  netplay.
- **VERSION / lobby match pin:** peers should run the same release pin so
  generated code and protocol stay compatible.
- **Mods:** disabled for netplay sessions unless the title sets
  `[netplay] content_negotiation = true`. With it, online and LAN lobbies publish the host's portable verified plan.
  Each peer prepares its own required media, then compares the complete content
  fingerprint before entering guest execution. Direct launches retain local
  choices and require matching plans. No assets or local paths are transferred;
  the guest's saved offline selection survives the session.
### Own-view mods (per player)

The shared simulation stays stock on every peer; a mod that only changes
what one player sees runs for that player alone, inside the sandboxed own-view
render (`psx_mod_render_local_view`). A package opts in per plugin:

```toml
[[plugin]]
feature = "widescreen"
id = "r4.widescreen"
netplay = "local_view"
```

Two more plugin classes stay on per player. `netplay = "input"` transforms
only this player's pad before it is staged (`psx_mod_set_pad_transform`).
`netplay = "host_output"` only reads guest state to drive this player's host
output (rumble, haptics): its function-entry hooks run in the shared
simulation (including rollback resimulation), a completion request from them
is ignored, and every `psx_mod_write_*` they attempt is refused while the
match runs, so they cannot diverge the peers. Host-side calls that must not
repeat on a resim (e.g. `psx_mod_set_host_rumble`) check
`psx_netplay_is_resimulating()` themselves.

At a netplay session start the runtime resolves the player's own selection
and keeps only features whose every contribution is such a plugin (no EXE or
disc write, overlay or derived disc; `mod_runtime_commit_netplay_view`). They
activate as usual (presentation state: aspect, scene predicates), but:

- their function-entry, filter, guest-function and instruction hooks run only
  while `psx_mod_local_view_scope()` is 1, i.e. inside a local-view draw, whose
  guest-side effects the sandbox discards; vblank and savestate callbacks do
  not run in a match;
- `gpu_ws_set_local_view_only(1)`: the widescreen cull margin
  (`psx_ws_x_margin()`) is the stock 0 in the shared game and this peer's own
  margin only inside the local-view draw; squash widescreen (which changes the
  GTE projection) is not used.

So the canonical frame, rollback snapshots and digests are identical on every
peer whatever each player chose, there is nothing to negotiate or hash, and
each player keeps their own aspect (Fit follows their own window). Nothing is
written to `mods/state.toml`. A title without an own view simply shows its
canonical frame. A plugin must not keep host state that feeds guest writes
outside the scope; anything that persists guest state across frames is not an
own-view mod.

---

## Enabling for a new title

1. Vendor or submodule `recomp-net` under `psxrecomp/lib/recomp-net` (or set
   `RECOMP_NET_ROOT`).
2. Before `include(runtime.cmake)`: `set(PSX_NETPLAY ON CACHE BOOL … FORCE)`.
3. `psxrecomp_add_game_runtime(… ENABLE_NETPLAY_IF_PRESENT …)` with
   `MAX_PLAYERS` / `game.toml` `players` set correctly.
   ICE (libjuice) defaults ON with `PSX_NETPLAY`; override with
   `-DRNET_ENABLE_ICE=OFF` if needed.
4. Fill `[netplay]` disc gates for multi-track games.
5. Test LAN 2P, then online lobby; soak rollback + FMV if the title uses media.

This document will grow as N-way rollback confirmation, SFU soak on 5P titles,
and further ICE/SFU policy land.

## Per-peer full-screen views

`[netplay].local_viewport = "vertical_split"` presents the camera belonging to
the peer's mapped controller port, including lobby seat reordering.
`local_viewport_aspect = "16:9"` fixes the presentation aspect independently
of monitor size. `local_viewport_renderer = "projection"` widens projection
inside the native split framebuffer; `native_wide` selects the separate wide
compositor. Offline rendering keeps the title's original renderer choice.

`local_viewport_aspect = "fixed"` uses the admitted mod plan's 21:9 choice or
defaults to 16:9. It disables adaptive resizing for the session. Enhanced
native-wide local views present their per-camera surface directly through GL,
including both double-buffer bands, and retain the full composite rather than
copying canonical split-divider columns into it. Trusted title plugins may opt
into forward-netplay render passes; rollback resimulation stays excluded (see
[RENDER_PASSES.md](RENDER_PASSES.md)).

Projection uses the PSX display's pixel aspect: each half of a 4:3 display
spans 2:3, so a 16:9 local view needs a 3/8 horizontal projection ratio.
The GL path presents the selected high-resolution FBO half directly.
Source-guarded HUD groups drawn in a full-display area and spanning the seam
are copied completely into both halves, preserving glyphs and shadows.

Titles may configure `local_viewport_state_addr` and
`local_viewport_state_values` to release the crop immediately during modal
menus. V82 uses its modal-menu flag at 0x8006B4F0 (running value 0). Its
`local_viewport_width_sites = ["0x8001C248", "0x8002E01C"]` identifies exact
LW sites for terrain/object frusta. The generated and dirty-RAM paths apply
the same inverse projection ratio to half-display widths while unsplit is
active; ordinary widths, pause and offline execution remain unchanged.
Regenerate title code after changing these sites. At most 16 unique aligned
main-RAM sites are allowed, and they require the projection renderer.
