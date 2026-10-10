#ifndef PSX_NETPLAY_H
#define PSX_NETPLAY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Delay-sync netplay facade over recomp-net (LAN UDP or MotK ICE).
 * Online hosted lobbies use ICE + WS signaling; Direct IP / LAN stay on UDP.
 *
 * Lockstep contract (matches recomp-net host_integration.md):
 *   wait_admit (publish pads for tick T) → guest runs frame T →
 *   finish_frame (advance) → wait_admit for T+1 → …
 * Guest must NOT run while linking or while try_admit fails.
 *
 * Input ownership:
 *   - Each peer stages one local device sample; recomp-net maps it onto
 *     that peer's local_slot (slot 0 = sim P1, slot 1 = sim P2). Transport
 *     host/client role is determined by peer_hostport, not by local_slot.
 *   - input_player selects which host PlayerInput to sample; -1 = auto
 *     (prefer g_players[local_slot] if assigned, else player 0).
 *   - While active, publish / release_pads is the sole SIO writer.
 *   - Every session slot stays plugged for in-game N-player detect.
 *   - slot_count >= 3 enables SCPH-1070 multitap (sio_set_multitap).
 *
 * Pad blob (8 bytes):
 *   [0..1] buttons LE u16 (PSX active-low)
 *   [2] lx  [3] ly  [4] rx  [5] ry
 *   [6] controller type (0 digital, 1 DualShock, 2 JogCon, 3 NeGcon)
 *   [7] connected (always 1)
 *
 * Packed per type. DualShock: lx/ly/rx/ry are the sticks. JogCon: lx is the
 * wheel position (0x80 centre). NeGcon: lx = twist (0x80 centre), rx = analog
 * I, ry = analog II, ly = analog L (0 released .. 0xFF pressed). The rollback
 * row carries only lx/ly (stick view), i.e. NeGcon twist and L.
 */

#define PSX_NETPLAY_PAD_BYTES 8

typedef struct PsxNetPad {
    uint16_t buttons;
    uint8_t  lx, ly, rx, ry;
    uint8_t  analog; /* SIO_PAD_*; legacy field name retained on the wire */
    uint8_t  connected;
} PsxNetPad;

/* Highest controller type a PsxNetPad may carry (SIO_PAD_NEGCON). Larger
 * values decode as digital. */
#define PSX_NETPAD_TYPE_MAX 3u

/* The one way a resolved pad reaches SIO, shared by offline sampling, netplay
 * (delay and rollback apply) and selfcheck replay: buttons, the type's axes
 * (NeGcon unpacked to twist + I/II/L), and a deferred type request. A
 * multitap seat without the analog-on-tap hack is made config-inert, as SIO
 * then forces it digital. Callers own connection and other policy. */
void psx_pad_apply_to_sio(int port, const PsxNetPad *pad);
typedef struct PsxNetplayConfig {
    int         enabled;
    int         local_slot;    /* 0 .. slot_count-1 */
    /* 1 = spectator: simulate the match, display it, contribute nothing.
     * The session owns no seat (RNetConfig.local_slot == slot_count) and is
     * never sampled for input, so the local pads never enter the pipeline at
     * all -- the only version of "cannot affect the game" that survives a
     * spectator with a controller in their hands. */
    int         spectator;
    /* Spectator only: slot in the input relay's namespace, at or above the
     * relay's player count. Required when spectator is set; it is what makes
     * the relay refuse to forward anything this peer sends. */
    int         spectator_wire_slot;
    /* 1 = the host watches from the gallery: it is still session slot 0
     * (the seat every host-only path keys on) but its pad is muted and no
     * controller port is mapped to it; session slot s >= 1 drives pad
     * port s - 1. slot_count then includes the host's silent slot, so it
     * may reach PSX_MAX_PLAYERS + 1. Env PSX_NET_HOST_SPECTATES=1. */
    int         host_spectates;
    /* Session slot -> controller port (-1 = none), when port_map_valid. The
     * lobby host is always session slot 0 whatever seat it holds; the other
     * players follow in seat order and drive the port of their lobby seat.
     * Without it: identity, or slot - 1 with host_spectates. */
    int         port_map_valid;
    int         port_of_slot[9];
    int         slot_count;    /* 2 .. PSX_MAX_PLAYERS (+1 with host_spectates) */
    int         player_count;  /* seated players at launch (0 = use slot_count) */
    /* Bit i = lobby seat i occupied. 0 = all seats occupied (legacy). Sparse
     * rooms (moved seats leaving a hole) must set this so recomp-net does not
     * wait forever on empty remotes. */
    uint32_t    occupied_mask;
    int         input_player;  /* host device index; -1 = auto */
    int         input_delay;
    /* Rollback invent runway (phase_lock / P). Clamped 2..16. Unused in delay-sync. */
    int         input_prediction;
    int         force_input_relay; /* 1 = lobby UDP SFU star (from launch) */
    /* 1 = the launch said transport "host": an online room whose host
     * carries the match on bind_hostport; guests dial peer_hostport. Takes
     * the LAN / hub transport path, not the server-relay one. */
    int         transport_host;
    /* 1 = the launch said transport "host" AND relay_via "ice" (host relay
     * over ICE). transport_host is ALSO 1 then; this is tested first. No UDP
     * port is bound and nothing is dialled (bind_hostport is a placeholder,
     * peer_hostport empty): the match runs over the ICE agents the waiting
     * room connected -- host: a hub over one agent per guest; guest: a 1:1
     * agent to the host. Neither the single-agent ICE path nor the SFU is
     * ever chosen for it. The agents come from psx_netplay_ice_stash_from_lobby
     * (taken when the launch was consumed) or, failing that, straight from the
     * lobby client. */
    int         transport_ice_hub;
    int         force_turn;        /* 1 = ICE relay-only (Force TURN for UDP) */
    /* 0 = auto (MotK room → ICE, else LAN), 1 = force ICE, 2 = force LAN.
     * Env PSX_NET_TRANSPORT=lan|ice overrides. */
    int         transport;
    /* 0 = delay-sync, 1 = rollback invent/contract (lobby default on).
     * Env PSX_NET_MODE=delay|rollback overrides. */
    int         rollback;
    /* 1 = seat 1 brings its own memory card: before the host's card broadcast,
     * seat 1 uploads its LOCAL slot-1 card to the host, which installs it as
     * the match's slot-2 card; every peer then receives it in the usual SRAM
     * blob. Both the host's real slot-2 card and seat 1's real cards are
     * left untouched (host sandboxes slot 2; guests already sandbox both).
     * Must be identical on every peer (the lobby decides it at start).
     * Env PSX_NET_GUEST_MEMCARD=1 overrides. */
    int         guest_memcard;
    /* Nonempty for direct/LAN sessions: full portable mod-plan SHA-256.
     * Gameplay waits for every occupied peer to agree. */
    char        content_fingerprint[65];
    uint32_t    session_id;
    char        bind_hostport[64];
    char        peer_hostport[64];
} PsxNetplayConfig;

void psx_netplay_config_defaults(PsxNetplayConfig *cfg);
void psx_netplay_apply_env(PsxNetplayConfig *cfg);

/* Internal RB_SYNC demultiplexing; returns 1 for a content identity packet. */
int psx_netplay_content_note(uint32_t epoch, uint32_t word0, uint32_t word1,
                             uint32_t seen, uint8_t slot, uint8_t op, uint8_t flags);
int  psx_netplay_active(void);
/* Every other occupied seat of the session (bit i = seat i): the peers that
 * must all answer a rollback episode. 0 offline. */
uint32_t psx_netplay_peer_seats(void);
int  psx_netplay_is_running(void);
/* "ice" | "lan" | "none" */
const char *psx_netplay_transport_name(void);
/* 1 when ICE agent reached FAILED (online path). */
int  psx_netplay_ice_failed(void);
/* Optional JSONL samples when PSX_NET_DIAG=1 (saves/netplay/net_diag.jsonl). */
void psx_netplay_diag_tick(void);
int  psx_netplay_local_slot(void);
/* Guest controller port of this peer; differs from session slot after seat swaps. */
int  psx_netplay_local_port(void);
/* 1 while this build is watching rather than playing. */
int  psx_netplay_is_spectator(void);
/* 1 when the local peer is the host running the match from the gallery. */
int  psx_netplay_host_spectates(void);
/* Resolved host player index used for local capture. */
int  psx_netplay_input_player(void);
uint32_t psx_netplay_sim_tick(void);
/* Session seats, including any host gallery seat; zero when offline. */
int psx_netplay_seat_count(void);
/* Read the pad actually published for one session seat in the current
 * simulation tick. This follows the delay-sync publisher and rollback's
 * sealed-frame override of predicted history, so a trusted game plugin can
 * use the same seat inputs that SIO sees when a title consumes more player
 * commands than its SIO protocol exposes. Call on the emulation thread while
 * guest code runs; returns 0 when netplay is off, the seat is absent, or no
 * pad has been published for this tick. */
int psx_netplay_sim_pad(int seat, PsxNetPad *out);

/* Presentation-only local view. A title whose netplay mode draws every seat's
 * view into one frame on every peer (so guest state stays identical) asks the
 * present path to show only this peer's view: a rectangle of the display area
 * in guest pixels, relative to the GP1(05h) display start. The present path
 * scales it to the window at 4:3. Host state only: never serialized, never
 * visible to the guest. A request lapses a few simulation ticks after the
 * last renewal, so the title renews it every frame its multi-view screen is
 * up. Ignored while netplay is off. */
void psx_netplay_present_local_view(uint32_t x, uint32_t y,
                                    uint32_t w, uint32_t h);
/* Drop any current request at once (the full frame is presented again).
 * A committed psx_mod_render_local_view() image calls it: the peer's own
 * image of the display supersedes a crop of the canonical frame. */
void psx_netplay_local_view_clear(void);
/* 1 while this peer is behind the other peers' inputs: presentation-only work
 * (psx_mod_render_local_view) is shed first so the simulation can catch up.
 * See psx_netplay_local_view_shed_step. PSX_NET_LOCAL_VIEW_SHED=0 keeps the
 * own view regardless. 0 offline. */
int psx_netplay_local_view_shed(void);
/* 1 and the rectangle while a current request fits a display of
 * display_w x display_h. */
int psx_netplay_local_view(uint32_t display_w, uint32_t display_h,
                           uint32_t *x, uint32_t *y,
                           uint32_t *w, uint32_t *h);

/*
 * Snapshot for diagnostic dumps (starvation_dump.jsonl meta, etc.).
 * arch_out: "off" | "p2p" | "host_relay" | "ice_host_relay" | "server_relay" (never NULL when
 * arch_cap > 0). Returns 1 when netplay is/was configured this run.
 */
int  psx_netplay_diag_snapshot(char *arch_out, size_t arch_cap,
                               int *max_players_out, int *player_count_out);

int  psx_netplay_start(const PsxNetplayConfig *cfg);

/* Host relay over ICE: take the launch's connected agents from the lobby
 * client and hold them until psx_netplay_start adopts them. Call where the
 * launch is consumed, BEFORE the lobby launch is cleared (the lobby destroys an
 * untaken bundle after 60 s). No-op unless the lobby's launch is an ICE hub
 * launch with a bundle pending. Returns 1 when agents are now held. */
int  psx_netplay_ice_stash_from_lobby(void);
/* Release held agents (a launch that never started). Safe at any time. */
void psx_netplay_ice_stash_discard(void);
/* 1 when held agents await psx_netplay_start. */
int  psx_netplay_ice_stash_held(void);
void psx_netplay_shutdown(void);
/* Soft-return rematch / new lobby opponent: make host sim state match a cold
 * process peer. Call from session_reboot (rematch) and after BYE teardown.
 * Does not replace device *_init — orchestrates sticky host statics. */
void psx_netplay_cold_reset(void);

/*
 * After savestate_configure + memcard_init: refresh RB integrity keys from
 * savestate (host + guest — start() ran before configure), then guest-only
 * redirect .pst/.mcd writes to saves/netplay/ so host sync never touches
 * personal saves.
 */
void psx_netplay_bind_guest_saves(void);

/* 1 if this peer is sim authority (local_slot == 0). */
int  psx_netplay_is_host(void);

/*
 * Host-only save/load orchestration (hash probe → transfer on miss).
 * Returns 1 if the request was accepted/ignored-as-guest, 0 if netplay inactive.
 */
int  psx_netplay_request_save(int slot);
int  psx_netplay_request_load(int slot);

/* 1 while a save/load/memcard probe, chunk transfer, or post-load ready owns
 * the clock (long admit timeout, no peer-silence kick, FPS suppressed). */
int  psx_netplay_in_load_barrier(void);

/* 1 once after a staged netplay load apply failed (stale .pst / mismatch).
 * Clears. Caller should soft-exit to lobby — do not keep waiting on the barrier. */
int  psx_netplay_consume_load_apply_failed(void);

/* Stage local pad for the current sim tick. Ignored once that tick is latched.
 * Always refreshes the live physical snapshot (see live_pad_buttons). */
void psx_netplay_stage_local(const PsxNetPad *pad);

/* PSX_RB_PAD_TRACE=1: host capture checkpoint (card / SDL Start / fallback).
 * Call from capture_local_human_pad each sample; no-op when trace off. */
void psx_netplay_pad_trace_dev(int card, int fallback, int sdl_start,
                               uint16_t buttons);

/*
 * Start cadence bisect (PSX_START_BISECT=1): every-sample pipeline log for
 * offline vs online. Does not change debounce. Optional feature kills
 * (one at a time) for Stage-3 isolation — see ROLLBACK_MOTK_HOOKUP.md.
 *
 *   PSX_START_BISECT_NO_GC_UPDATE_IN_ADMIT=1
 *   PSX_START_BISECT_NO_TIPHOLD_CAPTURE=1
 *   PSX_START_BISECT_NO_CATCHUP=1
 *   PSX_START_BISECT_NO_REPLAY_PRODUCE=1
 *   PSX_START_BISECT_SPIN=1  — also log raw SDL on admit spins (dense)
 */
int  psx_start_bisect_enabled(void);
int  psx_start_bisect_no_gc_update_in_admit(void);
int  psx_start_bisect_no_tiphold_capture(void);
int  psx_start_bisect_no_catchup(void);
int  psx_start_bisect_no_replay_produce(void);
int  psx_start_bisect_spin_log(void);
/* path: SDL|cap|stage|tip|sio|spin  — start bits are 1=pressed (active-low decoded). */
void psx_start_bisect_log(const char *path, uint32_t sim, int sdl_start,
                          int cap_start, int deb_start, int sio_start,
                          int latched, int tip_hold, int resim);

/*
 * Start consumer bisect (PSX_START_CONSUMER=1): log the Start bit SIO presents
 * to the guest each sim frame (offline present sample / online publish apply).
 * Compare offline vs online timelines — not SDL. edge=↓|↑|- .
 */
int  psx_start_consumer_enabled(void);
/* Offline: bump once per sample_pad_into_sio; returned value is the frame id. */
uint32_t psx_start_consumer_offline_frame(void);
void psx_start_consumer_note(int slot, uint32_t sim, uint16_t buttons);

/* 1 while linking or before this sim tick's local pad is latched. */
int  psx_netplay_needs_local_sample(void);

/* Latest physical local buttons (0xFFFF idle). TipHold SAFETY/quiet must use
 * this — staged/delay-ring peeks stay frozen while sim is invent-capped. */
int  psx_netplay_live_pad_buttons(uint16_t *out);

/* 1 if INPUT_CONFIRM hash disagreement stalled the session. */
int  psx_netplay_input_desync(uint32_t *tick, uint32_t *local_hash, uint32_t *remote_hash);

/* 1 if peer sent BYE or went silent for ~timeout_ms (default 1500).
 * Pass 0 for BYE-only (load barrier / LINKING before HELLO). */
int  psx_netplay_peer_disconnected(uint32_t timeout_ms);

/* Re-arm RUNNING silence clock after a pump (admit barrier entry). */
void psx_netplay_touch_peer_liveness(void);

/* RUNNING peer_disconnected timeout. 0 = BYE-only (early sim / boot tip wait /
 * pcap_freeze — rematch dig0 can exceed 1.5s without INPUT). */
uint32_t psx_netplay_running_liveness_timeout_ms(void);

/*
 * Ingress / lobby / INPUT retransmit without try_admit. Used while the
 * delay-sync starvation latch holds for remote runway refill.
 */
void psx_netplay_pump(void);

/*
 * Pump + try_admit for the current sim tick. On success, publish has written
 * SIO and a finish_frame() is owed after the guest completes that tick.
 * Returns 1 if admitted, 0 if caller must keep polling (linking / wait).
 * Does NOT advance the session clock.
 *
 * After sustained admit misses, latches starvation (pump-only) until
 * remote_lead >= D for a few frames, then arms a recovery catch-up boost.
 * Env: PSX_NET_STARVATION_ENTER_FRAMES, EXIT_FRAMES, EXIT_HR_LEAD.
 */
int  psx_netplay_poll_admit(void);

/* Call after the guest finishes the admitted tick (vblank boundary). */
void psx_netplay_finish_frame(void);

/* highest_remote_wire - sim_tick (0 if inactive; can be negative). */
int  psx_netplay_remote_lead(void);
/* Session input delay frames (default 2 when inactive). */
int  psx_netplay_input_delay(void);

/* Clear timesync pegged-streak at episode/tip-hold boundaries so resim cost
 * cannot look like WAN transit and trip adaptive-off (see §22). */
void psx_netplay_timesync_on_episode_boundary(void);

/*
 * Extra headroom for post-starvation / behind-peer catch-up
 * (min(16, max(0, remote_lead - D, recovery_burst))).
 * Host should skip wall-clock pace while this is > 0, then call
 * psx_netplay_catchup_consume_frame() once per skipped pace.
 */
int  psx_netplay_catchup_budget(void);
void psx_netplay_catchup_consume_frame(void);

/* Park the admit barrier until a peer datagram may be ready (or timeout). */
void psx_netplay_wait_recv(int timeout_ms);

/* Diagnostics for a stuck admit barrier (stall name, sim tick, remote lead). */
void psx_netplay_admit_wait_info(char *stall_out, size_t stall_cap,
                                 uint32_t *sim_tick_out, int *lead_out);

/* Normalize for stabler cross-device blobs: unknown types become digital;
 * centred axes (sticks, wheel, twist) snap to 0x80 inside a deadzone; NeGcon
 * pressure bytes (ly/rx/ry) are kept exactly; digital centres everything. */
void psx_netplay_normalize_pad(PsxNetPad *pad);

void psx_netplay_release_pads(void);

/*
 * Bind the live CPUState for per-frame master digests (FRAME_COMMIT).
 * Call once after CPU init (alongside debug_server_set_cpu).
 */
struct CPUState;
void psx_netplay_bind_cpu(struct CPUState *cpu);

/* Highest sim tick whose master digest matched the peer (0 if none). */
uint32_t psx_netplay_resolved_through(void);
/* 1 if peer master-hash watermark covers tick (hash_confirm_promote). */
int  psx_netplay_hash_confirm_through(uint32_t tick);

/* 1 when PSX_NET_MODE=rollback (invent + input contract path). */
int  psx_netplay_rollback_mode(void);

/* Safe BB-edge snap save/load (call beside savestate_poll when !in_exception). */
void psx_netplay_poll_snap(struct CPUState *cpu, uint32_t resume_pc);
/* 1 while a rollback episode is resimulating (mute audio / skip wall pacer). */
int  psx_netplay_is_resimulating(void);

/* §63: reset host pad-edge trackers after snap load (guest SIO restored). */
void psx_netplay_on_rb_snap_loaded(void);

/* §64: restart hc-fork persist/retry clocks after tip-extend abandon so a
 * second recovery episode cannot open on the same Live tick. */
void psx_netplay_hc_fork_recovery_restart(void);
/* §102: drop pending hc-fork bookkeeping (peer-ahead NACK already scheduled
 * a light tip reopen — do not open a deep SPAN recovery). */
void psx_netplay_hc_fork_recovery_clear(void);

/* §93 P1 bisect: PSX_RB_CD_BISECT=1 (optional =LO-HI, default 150-250) logs
 * per-tick CD/MDEC/clk crumbs + CD cmd ISSUE/QUEUE/DONE timeline (cdrom.c;
 * includes stat/resp FIFO + GetTN tracks / GetTD td_lba).
 * Arm N more ticks after a media flush_resume. */
void psx_netplay_cd_bisect_arm(uint32_t from_sim, uint32_t ticks);
/* 1 when CD bisect env is on and sim is in the fixed or post-media arm window. */
int psx_netplay_cd_bisect_active(void);

/* Netplay GPU lock: CPU-authoritative VRAM (software raster) so snaps/digests
 * are peer-identical. If OpenGL was requested, keep the GL context for
 * present-only (CPU scanout → gl_renderer_present) — never glReadPixels.
 * Sim is forced to internal scale 1 (no SW SSAA hi-res mirror); present may
 * still upscale via GL/SDL filter. Vulkan still falls back to pure software
 * present. Implemented in main.cpp. */
void psx_frontend_netplay_force_sw_gpu(void);

#ifdef __cplusplus
}
#endif

#endif /* PSX_NETPLAY_H */
