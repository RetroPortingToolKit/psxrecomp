#ifndef PSX_LOBBY_CLIENT_H
#define PSX_LOBBY_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PSX_LOBBY_ID_LEN 40
#define PSX_LOBBY_NAME_LEN 64
#define PSX_LOBBY_VERSION_LEN 32
#define PSX_LOBBY_ENDPOINT_LEN 64
#define PSX_LOBBY_MAX_LIST 32
/* Player seats this framework can seat. Was what PSX_LOBBY_MAX_MEMBERS meant
 * before a lobby held anything that was not a player. */
#define PSX_LOBBY_MAX_PLAYERS 8
/* Spectator seats a host may open, a separate pool on top of the players --
 * so a full eight-player room can still be watched. */
#define PSX_LOBBY_MAX_SPECTATORS 4
/* Rows in the membership table: both tables land in it, tagged by role. */
#define PSX_LOBBY_MAX_MEMBERS (PSX_LOBBY_MAX_PLAYERS + PSX_LOBBY_MAX_SPECTATORS)

/* Seat indices are one namespace, matching the server: below the base is a
 * player seat, at or above it is `index - base` in the gallery. The server
 * republishes its own base in every lobby_update; this is the compiled-in
 * default for an update that arrives without one. */
#define PSX_LOBBY_SPECTATOR_SLOT_BASE 64
#define PSX_LOBBY_MAX_LAN_EPS 4
#define PSX_LOBBY_LANG_LEN 16

#ifndef PSX_GAME_VERSION
#define PSX_GAME_VERSION "dev"
#endif

typedef struct PsxLobbyRow {
    char     lobby_id[PSX_LOBBY_ID_LEN];
    char     name[PSX_LOBBY_NAME_LEN];
    char     game_name[PSX_LOBBY_NAME_LEN];
    char     game_version[PSX_LOBBY_VERSION_LEN];
    int      player_count;
    int      max_slots;
    int      has_password;
    /* Host UDP endpoint from the server list (for one-shot latency probes). */
    char     host_endpoint[PSX_LOBBY_ENDPOINT_LEN];
    /* Legacy hub lan_endpoints (compat). Prefer local UDP beacon by lobby_id. */
    char     lan_endpoints[PSX_LOBBY_MAX_LAN_EPS][PSX_LOBBY_ENDPOINT_LEN];
    int      lan_count;
    /* Round-trip ms to a reachable candidate; -1 unknown / timed out. */
    int      latency_ms;
    /* Host's country (alpha-2) from the server's GeoIP; "" unknown. */
    char     host_country[4];
    /* Gallery: 0 = none; else how many watch out of how many seats. */
    int      allow_spectators;
    int      max_spectators;
    int      spectator_count;
} PsxLobbyRow;

/* One player connected to the hub (the lobby_list `players` array). */
typedef struct PsxLobbyOnlinePlayer {
    char player_id[PSX_LOBBY_ID_LEN];
    char display_name[PSX_LOBBY_NAME_LEN];
    char country[4];
    char lobby_id[PSX_LOBBY_ID_LEN];
    char lobby_name[PSX_LOBBY_NAME_LEN];
    int  hosting;
    /* First 8 characters of the player's connection id: enough to tell
     * "which row is me" without publishing whole ids to browsers. */
    char tag[12];
    /* The title that player is browsing for; rows of other titles are
     * dropped at parse time when this client has a game identity. */
    char game_name[PSX_LOBBY_NAME_LEN];
    /* Opaque, stable id for the ACCOUNT behind this connection; "" for a
     * guest. The server's own row key, published so a client-side block
     * list can survive the other player reconnecting or renaming. A key,
     * never a label: nothing renders it. */
    char account[PSX_LOBBY_ID_LEN];
} PsxLobbyOnlinePlayer;
#define PSX_LOBBY_MAX_ONLINE 64

typedef struct PsxLobbyMember {
    /* Seat index in the shared namespace: a player seat, or
     * spectator_slot_base + gallery index. Pass it back to kick / move as-is. */
    int  slot;
    char player_id[PSX_LOBBY_ID_LEN];
    char display_name[PSX_LOBBY_NAME_LEN];
    int  ready;
    /* 1 when this row is in the gallery. Read this rather than comparing
     * `slot` against a base: the base is the server's to choose. */
    int  is_spectator;
    /* Peer BIOS capability from set_ready bios_offer (0 if legacy/missing). */
    int  bios_offer_valid;
    int  bios_can_openbios;   /* linked OpenBIOS backend */
    int  bios_can_scph1001;   /* linked retail + validated dump available */
    int  bios_prefer_openbios; /* explicit OpenBIOS pick (not retail) */
    /* Peer memory-card offer from set_ready memcard_offer (0 if legacy). */
    int  memcard_offer_valid;
    int  memcard_has_card;    /* a slot-1 card is enabled locally */
    int  memcard_share;       /* peer opted in to bring it to the match */
    /* Country (alpha-2) from the server's GeoIP; "" unknown. */
    char country[4];
    /* Account key behind this seat; "" for a guest. See
     * PsxLobbyOnlinePlayer.account. */
    char account[PSX_LOBBY_ID_LEN];
} PsxLobbyMember;

/*
 * Local BIOS capability advertised on set_ready (see docs/BIOS_SELECTION.md
 * netplay settle rule). Updated by the host runtime via psx_lobby_set_bios_offer.
 */
typedef struct PsxLobbyBiosOffer {
    int  valid;
    int  can_openbios;
    int  can_scph1001;
    int  prefer_openbios; /* 1 = OpenBIOS selected; 0 = retail / willing SCPH */
} PsxLobbyBiosOffer;

/*
 * Local memory-card offer advertised on set_ready. `has_card` says whether a
 * slot-1 card is enabled here; `share` is the player's opt-in to bring it.
 * Meaningful for seat 1 (whose card becomes the match's slot-2 card); other
 * seats may advertise it harmlessly.
 */
typedef struct PsxLobbyMemcardOffer {
    int  valid;
    int  has_card;
    int  share;
} PsxLobbyMemcardOffer;

/* One lobby chat line (WS op:chat, echoed by the server to everyone seated,
 * sender included — so the ring is the room's order, not ours). */
#define PSX_LOBBY_CHAT_TEXT_LEN 256
#define PSX_LOBBY_CHAT_RING 64
typedef struct PsxLobbyChatMsg {
    char     player_id[PSX_LOBBY_ID_LEN];
    char     from[PSX_LOBBY_NAME_LEN];
    char     text[PSX_LOBBY_CHAT_TEXT_LEN];
    /* The SERVER's id for this line, and what psx_lobby_report_chat names.
     * Empty for a system line, a locally generated one, and anything from a
     * server too old to assign one -- none of which can be reported, because
     * there is no referent both sides agree on.
     *
     * A report carries this and NOT the text: the server writes down what it
     * relayed under this id. Sending the words would let a client fabricate a
     * message and have somebody sanctioned for it. */
    char     mid[40];
    /* Account key of the sender; "" for a guest or a system line. What an
     * ignore/block keys on, so it outlives a rename. */
    char     account[PSX_LOBBY_ID_LEN];
    int      is_local;
    int      is_system;
    uint32_t seq;
} PsxLobbyChatMsg;

/*
 * Host-authoritative sim settings negotiated over the lobby.
 * Guests apply these on launch so both peers boot with matching caps.
 */
typedef struct PsxLobbyMatchCaps {
    int  valid;            /* 1 when a host blob was received / set */
    int  aspect_num;       /* e.g. 4, 16, 21 */
    int  aspect_den;       /* e.g. 3, 9 */
    int  turbo_loads;      /* 0/1 */
    int  bios_hle;         /* 0/1 */
    int  fast_boot;        /* 0/1 */
    int  auto_skip_fmv;    /* 0/1 */
    int  input_delay;      /* recomp-net delay frames (D) */
    int  input_prediction; /* invent runway frames (P); rollback only */
    int  force_input_relay; /* 0/1 — server input relay (vs P2P) */
    int  force_turn;       /* 0/1 — ICE relay-only (Force TURN for UDP) */
    int  rollback;         /* 0/1 — invent/rollback netplay (default on) */
    /* DualShock-on-multitap-tap hack (0/1). Host-authoritative for the match. */
    int  multitap_analog;
    /* Host allows seat 1 to bring its own memory card (default 1; absent
     * field = 1). The lobby icon is lit only when this AND seat 1's offer
     * (has_card && share) hold. */
    int  guest_memcard;
    /* Settled at start by the host: 1 = seat 1's card IS used this match.
     * Peers launch from this, never from their own view of the seat table,
     * so a toggle racing the start cannot split the room. */
    int  guest_memcard_active;
    char language[PSX_LOBBY_LANG_LEN];
    /* Settled match BIOS: "openbios" | "scph1001" | "" (unset / legacy). */
    char session_bios[16];
} PsxLobbyMatchCaps;

typedef struct PsxLobbyJoinInfo {
    int      ok;
    char     lobby_id[PSX_LOBBY_ID_LEN];
    uint32_t session_id;
    int      local_slot;
    char     host_endpoint[PSX_LOBBY_ENDPOINT_LEN];
    char     guest_endpoint[PSX_LOBBY_ENDPOINT_LEN];
    char     bind_hostport[PSX_LOBBY_ENDPOINT_LEN];
    char     peer_hostport[PSX_LOBBY_ENDPOINT_LEN];
    int      player_count;
    int      max_slots;
    /* 1 when this client holds a gallery seat: it runs the match and shows it,
     * and contributes no input to anybody. */
    int      local_is_spectator;
    /* Host opt-in, echoed by the server. 0 when the server predates
     * spectators, which is why every spectator control is gated on it. */
    int      allow_spectators;
    int      max_spectators;
    int      spectator_count;
    int      spectator_slot_base;
    /* Where the gallery starts in the RELAY's slot space -- a different
     * namespace from the lobby seat index above, and the one the engine needs.
     * The relay forwards nothing from a slot at or beyond its player count, so
     * this is the number that makes a spectator unable to send. */
    int      spectator_relay_base;
    /* Launch: the host watches from the gallery but runs the match from
     * session slot 0 (pad muted); player seats sit at lobby seat + 1. */
    int      host_spectates;
    char     last_error[64]; /* need_password | bad_password | … */
} PsxLobbyJoinInfo;

/* Default URL when PSX_NET_LOBBY_URL unset. Order:
 *   1) env PSX_NET_LOBBY_URL
 *   2) compile-time PSX_NET_LOBBY_DEFAULT_URL (title CMake / scaffold)
 *   3) ws://netplay.retcomm.net:8765 */
const char *psx_lobby_default_url(void);

int  psx_lobby_connect(const char *ws_url); /* 0 = connected or connect started */
void psx_lobby_disconnect(void);
int  psx_lobby_connected(void);
/* 1 while DNS/TCP/WebSocket upgrade runs off the UI thread. */
int  psx_lobby_connecting(void);

void psx_lobby_set_display_name(const char *name);
const char *psx_lobby_display_name(void);
const char *psx_lobby_player_id(void);

/* Non-blocking pump — call every frame from the launcher. */
void psx_lobby_pump(void);

/*
 * Title + release pin used for create/join matching and list filters.
 * Defaults: game_name empty (no name filter), game_version PSX_GAME_VERSION.
 * List: Release pins filter by exact game_version; "dev" lists all versions
 * of the title (join still requires an exact version match).
 */
void psx_lobby_set_game_identity(const char *game_name, const char *game_version);
const char *psx_lobby_game_version(void);

/*
 * TOC fingerprint (lowercase hex SHA-256 from DiscIdentity::disc_fp).
 * Sent on create/join so the lobby server can reject mismatched mounts
 * (e.g. Track-01-only vs full multi-track cue). Empty disables the check
 * on the server for that peer (legacy clients); modern clients always set it
 * after a successful disc verify.
 */
void psx_lobby_set_disc_fp(const char *disc_fp);
const char *psx_lobby_disc_fp(void);

/* Default max_slots for create (clamped 2..8, default 2). */
void psx_lobby_set_max_slots(int max_slots);

void psx_lobby_request_list(void);
int  psx_lobby_list_count(void);
int  psx_lobby_list_get(int index, PsxLobbyRow *out);
/* Players connected to the hub, refreshed with every lobby_list. */
int  psx_lobby_online_count(void);
int  psx_lobby_online_get(int index, PsxLobbyOnlinePlayer *out);

/*
 * Create lobby. host_bind e.g. "0.0.0.0:7777". password may be NULL/empty.
 * game_version NULL/empty → identity / PSX_GAME_VERSION / "dev".
 * match_caps may be NULL (legacy); when non-NULL and valid, sent to the server
 * so guests join with the host's sim settings.
 * Returns 0 if request sent; poll psx_lobby_join_info() / in_lobby().
 */
int  psx_lobby_create(const char *name, const char *game_name,
                      const char *game_version,
                      const char *password, const char *host_bind,
                      const PsxLobbyMatchCaps *match_caps);

int  psx_lobby_join(const char *lobby_id, const char *password,
                    const char *guest_bind);

int  psx_lobby_leave(void);

/* ---- spectators --------------------------------------------------------
 *
 * A gallery seat runs the match locally, in sync, and contributes nothing.
 * The lobby server enforces the "contributes nothing" half at its UDP relay;
 * this side enforces the rest -- no Ready vote, no controller reaching the sim.
 *
 * Every one of these reads 0 against a server that predates spectators, so a
 * caller that gates its UI on allow_spectators degrades to the old lobby. */

/* Host: open a gallery on the NEXT create. Sticky, like the max_slots
 * default -- create carries whatever this was last set to. */
void psx_lobby_set_allow_spectators(int allow);
/* What the toggle is set to, for the UI to render: what the host ASKED for. */
int  psx_lobby_allow_spectators_pref(void);
/* What the current lobby actually has (server-echoed), not what was asked.
 * These disagree whenever the server predates spectators -- exactly the case
 * the UI must not offer a gallery in. */
int  psx_lobby_allow_spectators(void);
int  psx_lobby_max_spectators(void);
int  psx_lobby_spectator_count(void);
/* 1 when THIS client is in the gallery. The one call the engine needs. */
int  psx_lobby_local_is_spectator(void);
/* Base of the gallery half of the seat namespace, as the server reports it. */
int  psx_lobby_spectator_slot_base(void);
/* Seat index for gallery position `index`, for move / kick; <0 out of range. */
int  psx_lobby_spectator_slot(int index);
/* This client's slot in the RELAY's namespace, for RNetConfig.wire_slot.
 * -1 when not a spectator or the server published no relay base -- and a
 * spectator without one must not launch, because sending as a player slot is
 * exactly what it must not do. */
int  psx_lobby_local_wire_slot(void);
/* 1 when `slot` addresses a seat in either table -- the shared guard the
 * kick / move entry points use, so a gallery index is not rejected as if it
 * were out of range. */
int  psx_lobby_seat_valid(int slot);

/* Host-only: remove the player in `slot` (not the host / self). */
int  psx_lobby_kick(int slot);

/* Host-only: swap/move a seated player between slots (server broadcasts update). */
int  psx_lobby_move_member(int from_slot, int to_slot);

int  psx_lobby_in_lobby(void);
int  psx_lobby_is_host(void);
/* Host's player_id from the last create/lobby_update (empty if unknown). */
const char *psx_lobby_host_player_id(void);
/* Filled after create/join/lobby_update; peer endpoints for PsxNetplayConfig. */
const PsxLobbyJoinInfo *psx_lobby_join_info(void);

/* Latest host match_caps (valid==0 until create/join/launch delivers one). */
const PsxLobbyMatchCaps *psx_lobby_match_caps(void);

/* Host: push updated caps while in lobby (clears ready via lobby_update). */
int  psx_lobby_set_match_caps(const PsxLobbyMatchCaps *caps);

/* Live member table from lobby_update (and create/join). */
int  psx_lobby_member_count(void);
int  psx_lobby_member_get(int index, PsxLobbyMember *out);

/* Waiting-room RTT in ms *to* `slot`, or -1 if unknown / local seat.
 * Prefers ICE/TURN data-channel RTT when the lobby probe completes
 * (CGNAT / Force TURN); else direct UDP rnet_rtt_probe. Stored as
 * max(local, peer REPORT) so an optimistic guest sample cannot undercut
 * delay. Not the lobby WebSocket RTT. */
int  psx_lobby_member_latency_ms(int slot);

/* True when member.player_id matches psx_lobby_host_player_id().
 * Prefer this over `slot == 0` — seats can move. */
int  psx_lobby_member_is_host(const PsxLobbyMember *member);

/*
 * ICE signaling relay (MotK WS op:signal). text is SDP/candidate (max 2047).
 * send returns 0 if queued/written; poll returns 1 when an inbound signal was
 * copied out (LOCAL_* types as emitted by the peer — remap to REMOTE_* before
 * rnet_session_push_signal).
 */
int  psx_lobby_send_signal(int type, int flag, const char *text);
int  psx_lobby_poll_signal(int *type, int *flag, char *text, size_t text_cap);
/* Drop queued ICE SDP/candidates (soft-return / rematch hygiene). */
void psx_lobby_clear_signals(void);
/* When 0, inbound ICE op:signal is discarded (lobby / post-match). Launch
 * re-enables so early peer offers are kept until netplay drains them. */
void psx_lobby_set_ice_signal_accept(int accept);

/*
 * Coturn / ICE credentials minted by the WS lobby
 * (`get_turn_credentials` → `turn_credentials`). Valid until disconnect or TTL.
 * Strings are stable until the next successful mint or disconnect — safe to
 * pass into RNetIceConfig for juice_create.
 */
typedef struct PsxLobbyTurnCredentials {
    int      valid; /* 1 when ok mint cached and not expired */
    char     stun_host[128];
    int      stun_port;
    char     turn_host[128];
    int      turn_port;
    char     username[192];
    char     password[128];
    uint32_t ttl_secs;
} PsxLobbyTurnCredentials;

/* Queue WS get_turn_credentials. Returns 0 if sent/queued. */
int  psx_lobby_request_turn_credentials(void);
/* Non-NULL; valid==0 when unavailable / expired / STUN-only. */
const PsxLobbyTurnCredentials *psx_lobby_turn_credentials(void);

/* Local ready flag (from last lobby_update matching our player_id). */
int  psx_lobby_local_ready(void);
/* True when every seated player is ready and player_count >= 2. */
int  psx_lobby_all_ready(void);

/* Toggle ready in the current lobby (attaches current bios_offer). */
int  psx_lobby_set_ready(int ready);

/* Local BIOS offer used on the next set_ready (and included in settle). */
void psx_lobby_set_bios_offer(const PsxLobbyBiosOffer *offer);
const PsxLobbyBiosOffer *psx_lobby_bios_offer(void);
/* Local memory-card offer (attached to set_ready alongside bios_offer). */
void psx_lobby_set_memcard_offer(const PsxLobbyMemcardOffer *offer);
const PsxLobbyMemcardOffer *psx_lobby_memcard_offer(void);

/* Seat self-service (server ops seat_move / seat_swap_request /
 * seat_swap_answer). A player may move itself to a FREE player seat; taking
 * an occupied one asks its occupant, who answers from the prompt.
 *   seat_move_self(to)    : 0 queued; a refusal comes back as a lobby error
 *   seat_swap_request(to) : 0 queued; outgoing() then reports the answer
 *   seat_swap_incoming    : 1 while somebody is asking THIS player
 *   seat_swap_respond(ok) : answer (and drop) the incoming ask
 *   seat_swap_outgoing    : 0 idle, 1 waiting, 2 accepted, -1 declined
 *   seat_swap_clear       : back to idle after showing a finished result */
int  psx_lobby_seat_move_self(int to_slot);
int  psx_lobby_seat_swap_request(int target_slot);
int  psx_lobby_seat_swap_incoming(char *who, size_t who_cap, int *from_slot);
int  psx_lobby_seat_swap_respond(int accept);
int  psx_lobby_seat_swap_outgoing(void);
void psx_lobby_seat_swap_clear(void);

/* Lobby chat. send: 0 when queued (the line appears via the server echo).
 * count/get read the ring, oldest first; cleared on create/join/leave. */
/* Report chat lines for moderation. Same contract on every console -- the
 * frame is built by recomp-net (recomp_net/chat_report.h), which is also where
 * the RNET_REPORT_* categories live; this only says where we are and sends it.
 *
 * `mids` are PsxLobbyChatMsg.mid values, several at once because harassment is
 * usually a burst rather than a line. Entries with an empty id are skipped.
 * The text is never sent: see PsxLobbyChatMsg.mid.
 *
 * 0 when handed to the server, which is not the same as accepted -- it refuses
 * an expired line, a guest's line, your own, and anything over the rate
 * limit. */
int  psx_lobby_report_chat(const char *const *mids, int mid_count,
                           const char *reason, const char *note);

int  psx_lobby_send_chat(const char *text);

/* Server chat: per-game, outside any room (op server_chat). Its own ring,
 * cleared on disconnect; lines are masked on arrival like lobby chat. */
int  psx_lobby_send_server_chat(const char *text);
int  psx_lobby_server_chat_count(void);
int  psx_lobby_server_chat_get(int index, PsxLobbyChatMsg *out);
int  psx_lobby_chat_count(void);
int  psx_lobby_chat_get(int index, PsxLobbyChatMsg *out);
void psx_lobby_chat_clear(void);

/*
 * Settle session BIOS from seated peers' bios_offer (+ local offer):
 *   OpenBIOS if anyone cannot run SCPH-1001 (missing offer ⇒ cannot);
 *   else SCPH-1001 when the host prefers retail and every peer can;
 *   else OpenBIOS if anyone prefers OpenBIOS; else SCPH-1001.
 * Host preference wins over guest OpenBIOS picks when all can SCPH.
 * Writes "openbios" or "scph1001" into out. Returns 0 on success.
 */
int  psx_lobby_settle_session_bios(char *out, size_t out_cap);

/*
 * Host: ask server to broadcast launch. When match_caps is non-NULL and valid,
 * it is attached to start so launch freezes the latest host settings.
 */
int  psx_lobby_request_start(const PsxLobbyMatchCaps *match_caps);

/*
 * Set when server sends op:launch. Both host and guests should boot netplay.
 * Cleared by psx_lobby_clear_launch_pending() after consuming.
 */
int  psx_lobby_launch_pending(void);
void psx_lobby_clear_launch_pending(void);

/* After soft-return / rematch: allow waiting-room ICE/UDP RTT probes again.
 * Launch suspends them so they cannot steal match ICE signaling. */
void psx_lobby_resume_waiting_room_rtt(void);

/*
 * Tell the server which accounts this player has blocked.
 *
 * ';'-separated opaque account ids, replacing the whole set -- the client's
 * own file (recomp-ui moderation.ini) is the authority, so an unblock needs
 * no separate op. NULL/"" clears.
 *
 * Sent to the SERVER rather than applied only here, because two of the three
 * things a block has to do cannot be done from this side: the matchmaker must
 * not pair the two, and the blocked player must not see or be able to join
 * the blocker's room. A client can hide what it was sent; it cannot know it
 * was blocked by somebody else, and that is the direction that matters.
 *
 * The server holds the list for the life of a connection and never persists
 * it; recomp-ui re-sends it on every reconnect.
 */
int  psx_lobby_set_blocks(const char *accounts);

/* -- Automatch --------------------------------------------------------------
 *
 * Server-run pairing: the player asks for a match and the SERVER creates the
 * room, is its host, and owns its match_caps from a named ruleset. Protocol:
 * recomp-net-server docs/AUTOMATCH.md. Everything downstream of the pairing
 * is the ordinary `joined` / `lobby_update` / `launch` path already in this
 * file, so this adds a queue and an accept gate and nothing else.
 *
 * Requires a signed-in account: the accept gate charges a dodge cooldown, and
 * a cost that reconnecting erases is not a cost.
 *
 * Same shape as the SNES client (snes_lobby_client.h), on purpose: the host
 * adapter in main.cpp is then a one-line translation per entry point.
 */
#define PSX_LOBBY_MAX_RULESETS 8
#define PSX_LOBBY_RULESET_ID_LEN 48
#define PSX_LOBBY_RULESET_LABEL_LEN 64
#define PSX_LOBBY_CAPS_SUMMARY_LEN 128

/* One queue type this server offers for this title. */
typedef struct PsxLobbyRuleset {
    char id[PSX_LOBBY_RULESET_ID_LEN];
    char label[PSX_LOBBY_RULESET_LABEL_LEN];
    /* Server-derived one-liner ("Delay 2 - Rollback on"). Derived from the
     * caps rather than authored, so it cannot drift from what the match
     * runs. The caps themselves arrive with `joined` like any host's. */
    char caps_summary[PSX_LOBBY_CAPS_SUMMARY_LEN];
    /* Empty = any release may queue (still only pooling with its own). */
    char game_version[PSX_LOBBY_VERSION_LEN];
    int  max_slots;
} PsxLobbyRuleset;

/* Matches RECOMP_LAUNCHER_AUTOMATCH_* so the host layer can hand these
 * straight to the launcher without a translation table that could drift. */
enum {
    PSX_LOBBY_AUTOMATCH_IDLE = 0,
    PSX_LOBBY_AUTOMATCH_QUEUED = 1,
    PSX_LOBBY_AUTOMATCH_FOUND = 2,
    PSX_LOBBY_AUTOMATCH_ACCEPTED = 3,
    PSX_LOBBY_AUTOMATCH_FAILED = 4
};

/* The offer on the table while state is FOUND. */
typedef struct PsxLobbyAutomatchFound {
    char opponent[PSX_LOBBY_NAME_LEN];
    /* Discord @username: display names are not unique, so this is the
     * disambiguator. "" when the server did not say. */
    char opponent_username[PSX_LOBBY_NAME_LEN];
    char opponent_country[4];
    char ruleset_id[PSX_LOBBY_RULESET_ID_LEN];
    char ruleset_label[PSX_LOBBY_RULESET_LABEL_LEN];
    int  est_rtt_ms;      /* the pair's estimate: rtt_a + rtt_b; <0 unknown */
    int  accept_secs;     /* seconds left to answer, recomputed per read */
} PsxLobbyAutomatchFound;

/* Ask the server what queues it offers for this title. Answered into
 * psx_lobby_automatch_ruleset_count/get; 0 of them means automatch is off
 * here. Refuses to re-send while one is outstanding. */
int  psx_lobby_automatch_request_rulesets(void);
/* 1 once the server has answered with at least one ruleset. Not having
 * asked yet is "no": the button must not be offered on an assumption. */
int  psx_lobby_automatch_available(void);
int  psx_lobby_automatch_ruleset_count(void);
int  psx_lobby_automatch_ruleset_get(int index, PsxLobbyRuleset *out);

/*
 * Join the queue. `ruleset_id` NULL/"" takes the first one.
 *
 * `mods_enabled` is the caller's assertion that a SIM-AFFECTING mod feature is
 * on locally, beyond whatever the ruleset itself imposes. The server refuses a
 * true with mods_not_pooled and cannot check it -- the assertion exists so a
 * modified client makes a deliberate false statement rather than exploiting
 * an omission (AUTOMATCH.md 5). The host layer decides it; this file only
 * carries it. `mod_exempt` is ';'-separated evidence for cosmetic exemptions
 * (NULL or "" = none), sent as a JSON array for the server to check.
 *
 * 0 = sent. <0 = refused locally (not connected, no fingerprint, already
 * queued). A server refusal arrives asynchronously as state FAILED with a
 * reason in psx_lobby_automatch_error().
 */
int  psx_lobby_automatch_queue(const char *ruleset_id, int mods_enabled,
                               const char *mod_exempt);
int  psx_lobby_automatch_cancel(void);
int  psx_lobby_automatch_state(void);
int  psx_lobby_automatch_queued_secs(void);
int  psx_lobby_automatch_pool(void);
int  psx_lobby_automatch_found_get(PsxLobbyAutomatchFound *out);
/* accept != 0 accepts; 0 declines and takes the dodge strike. */
int  psx_lobby_automatch_accept(int accept);
/*
 * Non-zero while the room this client is seated in was created by AUTOMATCH.
 *
 * A human-hosted room is somebody's room: it survives the match, and staying
 * seated is how a rematch happens. An automatch room is the server's, it is
 * created at both-accept and is not joinable by anyone, and there is no host
 * to rematch with -- so staying in it leaves the player parked in a room that
 * can never fill, and the server refuses their next ticket with
 * `already_in_lobby`. Cleared by leaving, and by anything else that ends the
 * seating.
 */
int  psx_lobby_automatch_room(void);
/* Refuse a queue locally, with the reason the player sees. For when the HOST
 * already knows the server would bounce the ticket. */
void psx_lobby_automatch_refuse_local(const char *why);
const char *psx_lobby_automatch_error(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_LOBBY_CLIENT_H */
