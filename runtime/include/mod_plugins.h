#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PSXModVBlankCallback)(void);
typedef void (*PSXModActivationCallback)(void);
struct CPUState;
/* A callback that runs guest code inside psx_mod_render_pass() can be
 * abandoned by a watchdog longjmp; see the note at psx_mod_render_pass(). */
typedef void (*PSXModFunctionEntryCallback)(struct CPUState* cpu,
                                            uint32_t address);
/* Return nonzero to finish this opt-in function with the callback's return
 * registers. The runtime publishes pc=$ra; zero executes the original body. */
typedef int (*PSXModFunctionFilterCallback)(struct CPUState* cpu,
                                           uint32_t address);

/*
 * Register a trusted, statically linked plugin implementation. Package
 * manifests select implementations by this stable id; archives never provide
 * native code or symbol names.
 */
int psx_mod_register_activation_plugin(const char* id,
                                       PSXModActivationCallback callback);
int psx_mod_register_vblank_plugin(const char* id,
                                   PSXModVBlankCallback callback);
/* Runs after successful guest-state restore, before the restored PC resumes.
 * Rebind host hooks here; do not advance guest gameplay as a VBlank would. */
int psx_mod_register_savestate_plugin(const char* id,
                                      PSXModActivationCallback callback);
int psx_mod_register_function_entry_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback);
/* Mod-defined guest functions have no original machine-code body. Addresses
 * must be aligned and in physical 0x0F000000..0x0FFFFFFF (an unused bus range),
 * never hardware/BIOS/game text. Only statically linked code can register one;
 * the active package plan owns its availability and resource context. Address
 * aliases share one globally unique registration. The callback supplies the
 * full function behavior; return publishes $ra through normal dispatch. */
int psx_mod_register_guest_function_plugin(
    const char* id, uint32_t address, PSXModFunctionEntryCallback callback);
int psx_mod_dispatch_guest_function(struct CPUState* cpu, uint32_t address);
extern uint32_t g_psx_mod_guest_functions;
/* Run immediately before a configured instruction, including delay slots.
 * The complete instruction word must match both registration and live RAM.
 * Distinct callbacks/word guards may share an address (overlay reuse); exact
 * duplicate registrations, including segment aliases, are rejected. Matching
 * callbacks run in registration order and must guard their overlay context.
 * Callbacks may update registers/data but cannot redirect PC, finish a guest
 * function, or re-enter guest execution (pending load/branch state is live).
 * Native emits opt in with [recompiler] mod_instruction_sites; dirty-RAM
 * execution uses the same guarded table. Inactive plans are inert. */
int psx_mod_register_instruction_plugin(const char* id, uint32_t address,
                                         uint32_t expected, PSXModFunctionEntryCallback callback);
void psx_mod_instruction(struct CPUState* cpu, uint32_t address, uint32_t instruction);
extern uint32_t g_psx_mod_instruction_hooks;
/* Called from generated functions listed by the game config and from every
 * interpreted entry, so the hook contract does not depend on the backend.
 * Hooks match by code address (segment bits ignored) and run only for plugins
 * the active plan resolved; the table is rebuilt at plugin activation. */
int psx_mod_register_function_filter_plugin(
    const char* id, uint32_t address, PSXModFunctionFilterCallback callback);
int psx_mod_function_entry(struct CPUState* cpu, uint32_t address);
/* Trusted game code (not a mod package) may hook guest functions for netplay
 * only. These are independent of the mod package plan, which every online
 * match clears, and run only while netplay is active; offline execution stays
 * stock. A filter returning nonzero consumes the whole call (PC <- $ra); it
 * must supply any guest-visible side effects itself. */
int psx_game_register_netplay_function_entry(
    uint32_t address, PSXModFunctionEntryCallback callback);
int psx_game_register_netplay_function_filter(
    uint32_t address, PSXModFunctionFilterCallback filter);
/* Complete a guest function from its trusted entry callback after supplying
 * its full result. Valid only for that callback's CPU. Publishes pc=$ra and
 * prevents the original body from executing. Nested callbacks have separate
 * completion scopes; requests outside an entry callback return zero. */
int psx_mod_finish_function(struct CPUState* cpu);
/* Active function-entry hook count (0 = none). Hot callers test it before the
 * call, so a run without an active hook pays one load per interpreted entry. */
extern uint32_t g_psx_mod_function_entry_hooks;

/* Always-on named event counters for trusted plugins (observability, not
 * logging): e.g. how often each guard in a hook rejected. `name` should be a
 * string literal of the form "<plugin>.<event>"; up to 128 distinct names are
 * kept, further names are counted in an overflow bucket. Emulation-thread only.
 * TCP: {"cmd":"mod_counters"} lists every counter with its last frame. */
void psx_mod_counter_add(const char* name, uint32_t delta);
/* Presentation-only filtering: 0 nearest, 1 bilinear, 2 stable minification.
 * Mode 2 uses a bounded palette-aware footprint for proven 3D polygons on
 * OpenGL; untracked UI stays nearest. Other backends use bilinear. A session
 * reset restores the player's configured filter. */
void psx_mod_set_texture_filter(int mode);
/* Entry callbacks can make nested guest calls while retaining host registers.
 * Save/load and rewind must wait until that host context has returned. */
int psx_mod_function_entry_active(void);

/* [timing] guest_cycle_scale mod gate (docs/config_schema.md, Timing block).
 * With guest_cycle_scale_gated = true in game.toml the scale applies only
 * while a trusted plugin holds this gate open (and any declarative
 * guest_cycle_scale_gate RAM predicates hold). Every mod session reset
 * shuts it; open it from the activation callback. Mod plans are cleared
 * online, so this gate keeps a scale that depends on a mod out of netplay.
 * The gate state is part of savestates and the netplay rollback snapshot. */
void psx_mod_set_guest_cycle_scale_gate(int open);
/* Live scale: the configured one while every gate is open, else 1. */
uint32_t psx_mod_guest_cycle_scale(void);
/* Narrow guest services available to trusted plugin callbacks. */
int psx_mod_game_started(void);
/* Read an original mounted-disc file without changing guest CD state/timing.
 * Emulation-thread callbacks only. NULL buffer + zero capacity queries size;
 * otherwise capacity must hold the entire file. Active sector mods apply. */
int psx_mod_read_disc_file(const char* path, void* buffer, uint32_t capacity,
                           uint32_t* size);
/* Effective-disc LBA and byte size of a file (directory records patched by
 * active mods apply, so a relocated file reports its new extent). */
int psx_mod_disc_file_extent(const char* path, uint32_t* lba, uint32_t* size);
/* Experimental retained-texture service (currently OpenGL only). IDs are
 * nonzero, stable game-owned identities, NOT GL names. Banks are immutable
 * 16-bit PS1 texels/indices with a caller-selected row pitch (width).
 * A missing bank may be reconstructed from original assets by the resolver,
 * including when a restored DMA queue refers to a previously unseen level. */
typedef int (*PSXModTextureBankResolver)(uint16_t id);
int psx_mod_texture_banks_supported(void);
int psx_mod_define_texture_bank(uint16_t id, uint32_t width, uint32_t height,
                                const uint16_t* pixels);
void psx_mod_set_texture_bank_resolver(PSXModTextureBankResolver resolver);
/* Default-off GL optimization: batch immutable-bank semi triangles in painter
 * order on the single-pass dual-source path only. Ordinary VRAM, subtractive
 * blending and destination-mask checks retain per-primitive isolation. Call
 * from activation or an emulation-thread render boundary. */
void psx_mod_set_texture_bank_batching(int enabled);
/* A dedicated GPU-DMA packet arena. Only GT3 commands sourced from this
 * allocation interpret C1/C2's otherwise-unused high bytes as a bank ID:
 * id = (C1 >> 24) | ((C2 >> 24) << 8). ID zero uses ordinary VRAM. The rest
 * of the packet is standard GP0, retaining OT order, palettes and STP blend.
 * Optional 40-byte suffix after the 40-byte tagged GT3: u32 magic 0x48545031,
 * three IEEE float 1/z weights, six IEEE float x/y coordinates. This enables
 * precise perspective rendering without transient host-pointer side tables.
 * Allocate during activation; do not mix stock game packets into this arena. */
uint32_t psx_mod_alloc_texture_packet_memory(uint32_t size, uint32_t alignment);
uint8_t psx_mod_read_byte(uint32_t address);
void psx_mod_write_byte(uint32_t address, uint8_t value);
uint16_t psx_mod_read_half(uint32_t address);
void psx_mod_write_half(uint32_t address, uint16_t value);
uint32_t psx_mod_read_word(uint32_t address);
void psx_mod_write_word(uint32_t address, uint32_t value);
/*
 * Replace one guest instruction and route that address through the runtime's
 * executable-RAM path. Use this instead of psx_mod_write_word for code so a
 * restored save state cannot leave the compiled instruction stale.
 */
void psx_mod_write_code_word(uint32_t address, uint32_t value);

/*
 * Services shared by seamless-loading adapters (resident disc data: see
 * mod_resident.h). Emulation-thread callbacks only.
 *
 * Run a guest function to completion from a hook: a0..a3 and $ra are set,
 * control returns when the guest reaches return_address, and the caller's
 * GPRs, PC, HI and LO are restored. COP0, GTE and timing deadlines keep the
 * callee's effects, as they would after an ordinary call. Snapshots wait
 * until the call returns (the host stack holds the continuation). Returns v0.
 */
uint32_t psx_mod_call_guest(struct CPUState* cpu, uint32_t function,
                            uint32_t return_address, uint32_t a0, uint32_t a1,
                            uint32_t a2, uint32_t a3);
/* psx_mod_call_guest in zero guest time, for CPU/RAM work a game spends guest
 * time on while it loads (allocators, decoders, table builds). Every RAM,
 * register and device effect of the callee is kept, as if it executed in an
 * instant at the call's guest cycle: none of its cycles are charged, no
 * device advances and no interrupt is taken while it runs, and the caller's
 * pending mult/div, GTE and load-pipeline timing is as it was before the
 * call. Device work the callee starts (a DMA, a CD command) proceeds in guest
 * time after the return. A callee that executes more than budget_cycles
 * (0 = no limit) is charged in full from then on, so one that waits on a
 * device still completes; *charged (optional) is then set to 1. Inside a
 * render pass or another uncharged call, time is already frozen and this is
 * psx_mod_call_guest. The outcome depends only on guest state, so netplay
 * peers and rollback re-simulation agree. Returns v0. */
uint32_t psx_mod_call_guest_uncharged(struct CPUState* cpu, uint32_t function,
                                      uint32_t return_address, uint32_t a0,
                                      uint32_t a1, uint32_t a2, uint32_t a3,
                                      uint32_t budget_cycles, int* charged);
/* psx_mod_call_guest_uncharged that also reports the guest cycles the callee
 * executed (*counted_cycles; 0 when time was already frozen, so the call ran
 * inside an outer span). An adapter can then charge its own estimate, e.g. a
 * share of the measured cost, instead of nothing. */
uint32_t psx_mod_call_guest_uncharged_counted(struct CPUState* cpu,
        uint32_t function, uint32_t return_address, uint32_t a0, uint32_t a1,
        uint32_t a2, uint32_t a3, uint32_t budget_cycles, int* charged,
        uint64_t* counted_cycles);
/* Deliver disc sectors into RAM exactly as a completed CD-ROM DMA would
 * (overlay capture, executable-page invalidation, CD DMA log when lba >= 0).
 * Word-aligned address and length. Returns 0 when the span leaves RAM. */
int psx_mod_dma_write_ram(uint32_t address, const void* data, uint32_t bytes,
                          int lba);
/* Store bytes through the CPU store path (any alignment): for data the
 * original code produces with CPU stores, e.g. a decompressor's output. */
int psx_mod_host_write_ram(uint32_t address, const void* data, uint32_t bytes);
/* PsyQ SpuWrite by DMA, completed synchronously: transfer address, DMA-write
 * transfer mode, the words through the SPU's DMA write path (address
 * advance and sample-IRQ checks), then transfer mode stop when stop_after.
 * spu_address 8-aligned, guest_source/bytes word aligned, at most 512 KiB;
 * the transfer address wraps at the end of SPU RAM as on hardware.
 * Library bookkeeping (transfer callbacks, busy flags) stays the caller's. */
int psx_mod_spu_upload(uint32_t spu_address, uint32_t guest_source,
                       uint32_t bytes, int stop_after);
/* PsyQ LoadImage completed synchronously: texture-cache flush, GP0 A0h
 * rectangle copy of w*h 16-bit pixels from guest RAM (provenance attributed
 * per word), then GP1(04h) DMA direction CPU->GP0 for uploads the library
 * would DMA (16 words or more). Caller drains earlier GPU work first. */
int psx_mod_psyq_load_image(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t guest_source);

/*
 * Allocate opt-in enhancement memory from Expansion 1. Until the first
 * allocation, the region remains hardware-faithful open bus. The returned
 * KSEG0 address is accessible through normal generated guest loads.
 */
uint32_t psx_mod_alloc_guest_memory(uint32_t size, uint32_t alignment);

/*
 * Allocate guest memory that is also addressable by 24-bit GPU linked-list
 * tags. This is intended for opt-in enhanced primitive/ordering-table arenas;
 * without an allocation the aperture remains unmapped and DMA stays faithful.
 */
uint32_t psx_mod_alloc_gpu_dma_memory(uint32_t size, uint32_t alignment);

/*
 * Opt into expanded 8 MiB main RAM for this launch. Default runtime behavior
 * remains stock 2 MiB mirroring unless a trusted activation plugin requests
 * this before memory_init().
 */
int psx_mod_set_main_ram_8mb(int enabled);

/* World-culling envelope in native game pixels, including safety guards. */
int32_t psx_mod_widescreen_x_margin(void);
/* Configured per-side visible reveal, excluding culling guards; zero at 4:3.
 * Use for screen-space layout, including the first frame of a new scene. */
int32_t psx_mod_widescreen_view_x_margin(void);

/* Opt into render-only recovery of saturated horizontal GTE projections in
 * native-wide gameplay. Requires exact packet-address/word provenance and
 * depth; never changes guest SXY, vertical coordinates, or the 4:3 path. */
void psx_mod_set_native_wide_projection_correction(int enabled);
/* Separately qualify near-camera world clipping for a title. The GL path
 * carries signed, unclamped projection through exact PGXP word transport,
 * clips at the camera/view planes, and preserves ordered GPU/VRAM writes.
 * Requires projection correction above; 4:3, software and untracked UI stay
 * on their existing paths. No architectural GTE or gameplay changes. */
void psx_mod_set_native_wide_near_clip(int enabled);
/* Bind render-only NCLIP branch consumers to full instruction words. These
 * recover winding only when valid horizontal projections saturated; guest
 * MAC0 and flags remain architectural. Empty registration disables the sites. */
void psx_mod_set_native_wide_nclip_sites(const uint32_t* addresses,
    const uint32_t* expected, int count);
/* After registering sites, bind a verified first-of-two quad branch to the
 * preceding NCLIP result. Exact address/word must match an existing site;
 * registering the base site list again resets every site to the latest result. */
void psx_mod_set_native_wide_nclip_previous_site(uint32_t address, uint32_t expected);

/* Mark a guest GPU packet (P_TAG address) as persistent screen-space HUD.
 * edge = -1 left, +1 right, 0 clears a reused packet's tag. The native-wide
 * compositor translates it by the live reveal, excluding culling guards.
 * Guest coordinates, world sprites, and native 4:3 remain unchanged. */
void psx_mod_tag_hud_primitive(uint32_t primitive, int edge);
/* Packet-guarded screen-space anchor: -1 left, +1 right, 0 stays centred.
 * Unlike the legacy role tag above, zero explicitly protects centred text
 * from automatic layout transforms. Word immediately before the GP0 colour
 * command; for a compound E1+SPRT packet this is its E1 word (P_TAG+4). */
void psx_mod_anchor_hud_primitive(uint32_t primitive, int edge);
/* Screen-space masks identified by their title's producer, P_TAG addresses.
 * Flat panels extend only their exterior boundaries. Radial flat/Gouraud
 * quads scale around the display centre for an aspect-aware iris transition.
 * Both services are render-only, word guarded and inert at native 4:3. */
void psx_mod_tag_screen_mask_quad(uint32_t primitive);
void psx_mod_tag_radial_screen_mask_quad(uint32_t primitive, float scale);
/* Exclude a known world packet from screen-space backdrop stretching, even
 * if it sorts before the first shaded polygon. Zero clears a recycled tag. */
void psx_mod_tag_world_primitive(uint32_t primitive, int is_world);
/* Enable aspect-derived column selection for a title that opted into the
 * auto_backdrop detector. No effect on titles that did not opt in. */
void psx_mod_set_adaptive_backdrop_preload(int enabled);

/*
 * Width, in native game pixels, of the picture the guest is currently
 * scanning out -- the same value the presenter uses, derived from the display
 * mode and the GP1(06h) horizontal range.
 *
 * Why this exists: a plugin that draws its own overlay primitives needs to
 * know where the right-hand edge of the screen is, and it cannot work that
 * out for itself. GPUSTAT carries the horizontal-resolution bits, so a plugin
 * can recover the coarse MODE width (256/320/512/640, or 368), but the
 * visible width also depends on the GP1(06h) X1/X2 range, which is write-only
 * and mirrored nowhere the plugin can read. Ape Escape is the worked example:
 * it scans out 384 while its mode width is 368, and a plugin that assumed the
 * usual 320 put its HUD row 68 pixels short of the edge.
 *
 * Returns 0 if the display geometry is not yet established, in which case the
 * caller should skip drawing rather than substitute a guess.
 */
uint32_t psx_mod_display_width(void);

/* Height companion to psx_mod_display_width(); same conventions. */
uint32_t psx_mod_display_height(void);

/* Opt-in presentation hold for a game that retains its previous framebuffer
 * while loading. The pure, cheap emulation-thread predicate returns HOLD
 * only while that SAME scene remains displayed; no GPU/API recursion allowed.
 * RELEASE resumes normal classification immediately. UNTIL_FLIP releases a
 * prior HOLD only once the displayed VRAM origin changes: useful when drawing
 * the next backbuffer finishes before it becomes visible. UNTIL_FLIP without
 * a prior HOLD does nothing. Do not use it for in-place scene replacements.
 * Native-wide retains its prior wide/4:3 classification, never stretches art
 * and never overrides FMV. NULL removes the opt-in. Host history is discarded
 * on GPU reset/savestate restore, so loading a frozen scene cannot recreate
 * missing widescreen strips. This does not change guest rendering or memory. */
typedef int (*PSXModRetainedScenePredicate)(void);
enum {
    PSX_MOD_SCENE_RELEASE = 0,
    PSX_MOD_SCENE_HOLD = 1,
    PSX_MOD_SCENE_UNTIL_FLIP = 2
};
void psx_mod_set_retained_scene_predicate(PSXModRetainedScenePredicate predicate);

/* Opt-in supplemental native-wide world classifier. A nonzero result marks a
 * known, rendered 3D scene (for example a real-time intro) as world content even
 * when a game's gameplay-state allowlist excludes it. Zero defers to the normal
 * classifier; NULL removes the callback. Only native-wide mode consults it.
 * FMV and retained-frame presentation rules still apply. The pure, cheap callback
 * runs on the emulation thread: no GPU calls, allocation or guest-state writes.
 * Registration is host configuration, not savestate data. No default behavior
 * changes and no extra geometry is generated by this service. */
typedef int (*PSXModWorldScenePredicate)(void);
void psx_mod_set_world_scene_predicate(PSXModWorldScenePredicate predicate);

/*
 * Read the committed value of one of this package's declared options, as the
 * player left it in the launcher (or the manifest default when untouched).
 * Writes a NUL-terminated string into `out` and returns 1; returns 0 with
 * out[0] = '\0' when the plan is not committed, the ids do not resolve, or the
 * value does not fit — the caller then applies its own default rather than
 * treating an empty string as a selection.
 *
 * Why this exists: the manifest schema already carries typed, validated,
 * launcher-rendered, persisted options ([[option]] boolean/choice/integer), but
 * an activation callback takes no arguments and had no way to read them, so a
 * trusted plugin could only ever be an on/off switch. A parameterised feature
 * then had to be modelled as one feature per value — and `constraint` only
 * expresses ordered_integer WITHIN a feature, so those pseudo-features could
 * not even be made mutually exclusive. This closes that gap: one feature, one
 * option, the plugin reads what was chosen.
 *
 * Ids are passed explicitly because registration is by plugin id alone and the
 * callback carries no package/feature context.
 */
int psx_mod_option_value(const char* package_id, const char* feature_id,
                         const char* option_id, char* out, uint32_t out_size);
/* Read an option of the package/feature whose trusted callback is running.
 * Shared framework plugins use this without hard-coding a title package id.
 * The same committed-plan and buffer rules as psx_mod_option_value apply. */
int psx_mod_current_option_value(const char* option_id,
                                 char* out, uint32_t out_size);
/*
 * Live option changes (in-game overlay P5). A trusted plugin that can re-read
 * an option on a running game registers an option-changed callback under its
 * plugin id; it runs in that plugin's context (psx_mod_current_option_value
 * already answers the new value) and returns 1 if the change took effect now.
 *
 * psx_mod_set_option_live records a new value for a committed feature's
 * option for the rest of the session and notifies its plugin. Returns 1 when
 * a plugin applied it live; 0 when the feature has no live-capable plugin (or
 * is not in the plan) -- then nothing changes until the next start; -1 when
 * refused: during netplay the plan is
 * negotiated between peers and must not change. Persisting the choice stays
 * the mod provider's job (mods/state.toml).
 */
typedef int (*PSXModOptionChangedCallback)(const char* option_id, const char* value);
int psx_mod_register_option_changed_plugin(const char* id,
                                           PSXModOptionChangedCallback callback);
int psx_mod_set_option_live(const char* package_id, const char* feature_id,
                            const char* option_id, const char* value);
/* 1 when the committed feature's plugin registered an option-changed callback
 * (so a live set can take effect now). Read-only; any thread. */
int psx_mod_option_live_capable(const char* package_id, const char* feature_id);
/* Write the player's current selection (feature switches and option values,
 * as edited through the launcher mod provider) to mods/state.toml now,
 * without re-resolving the running plan. Main thread. 1 on success. */
int psx_mod_save_selection(void);
/*
 * Read the committed owner-selected path for a resource declared by the
 * package feature whose trusted plugin is currently running. Returns 0 when
 * the feature has no selected path for that resource; plugins then leave the
 * stock presentation unchanged.
 */
int psx_mod_current_resource_path(const char* resource_id,
                                  char* out, uint32_t out_size);
/* Configure the host HD texture pack from this callback's directory resource.
 * Replacements are currently OpenGL-only; dumping also works on software and
 * Vulkan. Configuration is host state, never guest RAM or savestate data.
 * Activation/emulation-thread only. Failure is reported to the runtime log. */
int psx_mod_set_hd_texture_pack(const char* resource_id,
                                int replacements_enabled, int dump_enabled);
/* Emulation-thread controls for an already configured pack. */
int psx_mod_set_hd_texture_dump(int enabled);
int psx_mod_reload_hd_texture_pack(void);
/* Read-only canonical media verified by the engine for this plugin's owning
 * package/feature. The pointer lives until the committed plan is replaced or
 * cleared. Available only during that plugin's callbacks; returns 0 for an
 * ordinary unverified resource, an inactive feature, or a different owner. */
int psx_mod_current_resource_bytes(const char* resource_id,
                                   const uint8_t** bytes, uint64_t* size);

/* Append raw Mode-2 sectors (2336 bytes, starting with the XA subheader)
 * from a verified resource owned by this activation callback. Returns their
 * native CD LBA in first_lba. Registration is deterministic, append-only and
 * rejected outside activation, for invalid ranges or beyond 99:59:74.
 * The immutable committed snapshot remains mounted until the plan changes;
 * no host path or donor data is saved into guest RAM/savestates. */
int psx_mod_append_disc_extent(const char* resource_id, uint64_t byte_offset,
                               uint32_t sector_count, uint32_t* first_lba);

/* Optional CD-DA playlist, built only during activation. Audio has a separate
 * TOC/timeline (track 1 is a data placeholder); data/XA LBAs are unchanged.
 * Sources are verified immutable raw 2352-byte stereo PCM sectors, or audio
 * tracks on the mounted disc. Playback, reports and saves use the native CD
 * controller. No conversion, resampling or host clock is involved. */
typedef struct PSXModCDDATrack {
    const char* resource_id; /* non-NULL: verified external raw PCM */
    uint64_t byte_offset;
    uint32_t sector_count;
    uint32_t disc_track;    /* resource_id == NULL: mounted audio track */
} PSXModCDDATrack;
/* Append the whole group atomically. Both output arrays have count elements;
 * failure leaves the previous playlist intact and clears all outputs. */
int psx_mod_append_cdda_tracks(const PSXModCDDATrack* tracks, uint32_t count,
                              uint32_t* first_lbas, uint32_t* sector_counts);

/* Display aspects have no framework ceiling: the native-wide surfaces size
 * themselves from the live width, and each title caps its own view at what
 * it has validated (fixed ratio, or the adaptive maximum below). Requests
 * must be at least native 4:3, with numerator and denominator in 1..99. */

/*
 * Request a fixed host display aspect before renderer/window initialization.
 * Intended for activation callbacks that move a game's widescreen enhancement
 * out of generic Settings and into its mod catalog.
 */
int psx_mod_set_fixed_display_aspect(uint32_t numerator,
                                     uint32_t denominator);
/*
 * Request resize-driven widescreen, capped at the supplied maximum aspect.
 * Pass (0, 0) for Fit to window with no upper aspect limit. Both modes retain
 * the native 4:3 minimum; a single zero is invalid.
 * The current fixed aspect continues to shape the initial game window, so a
 * plugin may select that first with psx_mod_set_fixed_display_aspect().
 */
int psx_mod_set_adaptive_display_aspect(uint32_t max_numerator,
                                        uint32_t max_denominator);
/*
 * HUD size for the auto-UI widescreen HUD ([widescreen] auto_ui_squash):
 * 0 keeps it at the display height's scale ("original"), 1 makes it
 * proportional (unchanged up to 16:9, shrinking by sqrt((16:9) / aspect)
 * beyond, about each widget's anchors). Overrides game.toml
 * [widescreen] auto_ui_size for the session; every session start restores the
 * title's setting before activation.
 */
int psx_mod_set_widescreen_hud_size(int proportional);
/* Request an internal rendering height for this mod session (120..8192).
 * The existing resolution resolver selects an integer raster scale and the
 * backend applies its normal allocation limits. Zero clears the request.
 * Player settings are preserved, explicit PSX_INTERNAL_RESOLUTION overrides
   * win, and each session clears this request before activating its committed
   * mod plan. */
int psx_mod_set_internal_resolution(uint32_t target_lines);
/*
 * Set the wall-clock cadence of simulated guest VBlanks. A value of zero
 * removes frontend pacing; 60 and higher request that many native guest
 * update opportunities per host second. This intentionally changes whole-
 * machine realtime speed and is for experimental game-owned frame-rate mods.
 */
int psx_mod_set_native_vblank_rate(uint32_t frames_per_second);

/*
 * Enable presentation-only frame interpolation while leaving guest VBlank,
 * game logic, timers, and audio at their stock cadence. The OpenGL presenter
 * temporally blends completed guest frames at the requested output rate on its
 * owning render thread/context. It does not derive motion vectors or generate
 * true intermediate object positions.
 * A value of zero follows the measured host-display refresh rate.
 */
int psx_mod_set_frame_interpolation(uint32_t frames_per_second);
/*
 * Choose how the OpenGL presenter combines completed frames. Linear is a
 * full-frame crossfade. Motion-adaptive retains temporal blending for
 * small temporal changes but switches large changes cleanly to reduce the
 * double-image trails produced by moving objects.
 */
enum {
    PSX_MOD_FRAME_INTERPOLATION_LINEAR = 0,
    PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE = 1,
    /* No crossfade: every output frame repeats the newest game frame. For a
     * plugin that supplies its own in-between images with render passes
     * (below); wherever it has none, the output matches stock timing and
     * nothing is shown later than the game shows it. */
    PSX_MOD_FRAME_INTERPOLATION_HOLD = 2
};
/* Usually called from activation. It may also be called later from the
 * emulation thread (a function-entry hook or VBlank callback), e.g. to swap
 * HOLD for a crossfade while render passes are unavailable; the OpenGL
 * presenter then uses the new mode from its next present. */
int psx_mod_set_frame_interpolation_blend(uint32_t blend_mode);
/*
 * Choose what the OpenGL presenter treats as a new source frame. VBLANK (the
 * default, reset at every session start) treats every guest VBlank as one,
 * which suits games that flip every VBlank. FLIP rotates the blend history
 * only when the guest really flips (the displayed VRAM origin moves, or the
 * displayed rect is redrawn) and spreads each crossfade over the measured
 * flip period (1..4 VBlanks). A 30 Hz game then blends across its whole frame
 * instead of blending for one VBlank and holding for the next. Guest timing
 * is unchanged either way.
 */
enum {
    PSX_MOD_FRAME_SOURCE_VBLANK = 0,
    PSX_MOD_FRAME_SOURCE_FLIP = 1
};
int psx_mod_set_frame_interpolation_source(uint32_t source);

/*
 * Host-timed render passes: true in-between frames for a game whose logic
 * runs slower than the presentation rate. Default off: nothing happens unless
 * a trusted plugin calls these. OpenGL, frame interpolation enabled with the
 * FLIP source, never in netplay, rollback, rewind, fast-forward (manual,
 * turbo-through-loads, FMV auto-skip) or while the presenter is suspended
 * (FMV); psx_mod_render_pass_plan() returns 0 then, and
 * psx_mod_render_pass_status() says why.
 *
 * Call both from an emulation-thread function-entry hook placed where the
 * game has finished its logic for game frame N+1 but the display still has to
 * flip to frame N (for a PsyQ double-buffered loop: the VSync(0) that precedes
 * PutDispEnv). Each pass runs `fn`, which may call guest functions with
 * psx_dispatch_call() to draw an intermediate image of the scene. While it
 * runs, guest time is frozen: cycles are counted but no device advances, no
 * interrupt is delivered and GPU DMA completes synchronously; SPU, CD, timer
 * and other device stores are dropped (counted). Afterwards CPU state
 * (including the GTE), RAM, scratchpad, I-cache tags, interrupt, timer, DMA
 * and GPU registers, and the VRAM rect are exactly as before: the pass's only
 * product is the image of the rect, which the presenter shows at `alpha_q16`
 * of the way through frame N's time on screen (Q16, 0 = frame N's own image).
 * The display rect is the one the next flip shows (its DISPENV); the pass may
 * draw only inside it.
 */
/* Returns nonzero to keep the image, 0 to discard it (state is restored
 * either way). */
typedef int (*PSXModRenderPassFn)(struct CPUState* cpu, void* user,
                                  uint32_t alpha_q16);
typedef struct PSXModRenderPass {
    uint32_t struct_size;        /* sizeof(PSXModRenderPass) */
    uint32_t alpha_q16;          /* a phase returned by the plan */
    uint16_t x, y, w, h;         /* VRAM display rect the pass draws */
} PSXModRenderPass;
/*
 * Phases (Q16, ascending, excluding 0) at which the presenter will actually
 * show frame N: its output deadlines during the `period_vblanks` guest VBlanks
 * the frame stays on screen, starting after `shown_after_vblanks` more
 * VBlank presents (R4 at its VSync(0) entry: 1 -- the next VBlank still shows
 * the previous frame). When the host cannot afford them all, an evenly spread
 * subset is returned and the presenter crossfades the gaps. The first
 * psx_mod_render_pass() after a plan captures frame N's own image. Returns 0
 * when passes are unavailable or unaffordable.
 */
uint32_t psx_mod_render_pass_plan(uint32_t period_vblanks,
                                  uint32_t shown_after_vblanks,
                                  uint32_t* alpha_q16, uint32_t max);
/* Returns 1 when the pass ran and its image was queued, 0 when it was refused
 * or rolled back (state is restored either way).
 * A watchdog abort rolls the pass back by longjmp, past every frame between
 * the watchdog and psx_mod_render_pass(): plugin callbacks, guest functions
 * and function-entry hooks they called. Runtime nesting state (including the
 * function-entry context) is restored, but plugin-owned state, held locks and
 * C++ destructors in those frames are NOT unwound. A callback run inside a
 * pass must keep nothing that needs cleanup (no locks, no RAII objects, no
 * partially updated plugin state) across guest code, or tolerate the abort. */
int psx_mod_render_pass(struct CPUState* cpu, const PSXModRenderPass* pass,
                        PSXModRenderPassFn fn, void* user);
/*
 * Why passes cannot run right now, the host-time budget aside (a plan that
 * returns 0 while this says READY was shed for time). A plugin that relies on
 * passes uses it to fall back, e.g. to a crossfade with
 * psx_mod_set_frame_interpolation_blend(), while the reason lasts.
 * NO_PRESENTER, BACKEND and DISABLED persist; the others are transient.
 */
enum {
    PSX_MOD_RENDER_PASS_READY = 0,
    /* Not OpenGL, interpolation off or suspended (FMV), or not the FLIP
     * source. */
    PSX_MOD_RENDER_PASS_NO_PRESENTER = 1,
    /* The renderer declines passes in its current mode. */
    PSX_MOD_RENDER_PASS_BACKEND = 2,
    /* Switched off for this session after repeated faults. */
    PSX_MOD_RENDER_PASS_DISABLED = 3,
    /* Netplay, rollback, rewind, load/save replay or self-check resim. */
    PSX_MOD_RENDER_PASS_SESSION = 4,
    /* Fast-forward, turbo-through-loads or FMV auto-skip is running. */
    PSX_MOD_RENDER_PASS_FAST_FORWARD = 5,
    /* Inside an exception, a pass or a GPU DMA walk, or the presenter has not
     * captured a frame since its history restarted (display mode change). */
    PSX_MOD_RENDER_PASS_BUSY = 6
};
uint32_t psx_mod_render_pass_status(void);
/*
 * Netplay local view: this peer's own image of a display rect, drawn by the
 * game's code inside the render-pass sandbox, replaces what the presenter
 * shows of that rect. For a title whose netplay frame draws every seat's view
 * (so guest state stays identical on every peer) but whose players should
 * each see their own seat's single full-screen view.
 *
 * `fn` runs as a render pass does (frozen guest time, sandboxed stores, the
 * watchdog; CPU with the GTE, RAM, scratchpad, devices and the authoritative
 * VRAM restored afterwards) and draws into rect->x/y/w/h; alpha_q16 is 0. When
 * it returns nonzero the presenter's own copy of the rect keeps the image
 * until the guest draws there again: the canonical frame stays in the
 * authoritative VRAM, savestates, rollback snapshots and digests. A committed
 * image also cancels any psx_netplay_present_local_view() crop. Call it where
 * the next flip will show rect and the guest has finished drawing it.
 *
 * Only in a netplay session on forward frames, with the OpenGL presenter
 * keeping a surface separate from the authoritative VRAM (dual raster); never
 * while resimulating, in rewind, lockstep replay, fast-forward, inside an
 * exception or a pass. psx_mod_render_local_view_status() says why not, with
 * the PSX_MOD_RENDER_PASS_* reasons; a title then shows its canonical frame
 * (for example its own view's part of it through
 * psx_netplay_present_local_view). Returns 1 when the image was committed.
 */
int psx_mod_render_local_view(struct CPUState* cpu,
                              const PSXModRenderPass* rect,
                              PSXModRenderPassFn fn, void* user);
uint32_t psx_mod_render_local_view_status(void);
/* 1 while a psx_mod_render_local_view draw runs. In that scope a netplay
 * match's own-view mods ([[plugin]] netplay = "local_view") run their hooks
 * and the widescreen cull margin is this peer's; outside it neither touches
 * the shared simulation. */
int psx_mod_local_view_scope(void);
/* Simultaneous stereo capture, independent of temporal interpolation. Each
 * eye starts from the same guest state; CPU/RAM/devices/VRAM are restored
 * before the other eye and on failure. Publish only after both succeed.
 * The caller provides a draw-only callback at a main-thread frame boundary.
 * period_vblanks (1..8) describes the game's draw cadence for whole-pair cost
 * shedding. No alpha/time phase is used. Returns 1 for a published pair. */
enum { PSX_MOD_EYE_LEFT = 0, PSX_MOD_EYE_RIGHT = 1 };
typedef struct PSXModStereoFrame {
    uint32_t struct_size;
    uint32_t period_vblanks;
    uint16_t x, y, w, h;
} PSXModStereoFrame;
typedef int (*PSXModStereoFn)(struct CPUState*, void*, uint32_t eye);
int psx_mod_render_stereo(struct CPUState *cpu, const PSXModStereoFrame *frame,
                          PSXModStereoFn fn, void *user);
uint32_t psx_mod_render_stereo_status(void);
/* 0 disables stereo output; 1 shows a complete pair side by side. Capture
 * itself does not enable presentation. Default 0; applies to this session. */
int psx_mod_set_stereo_presentation(uint32_t mode);
/* Camera-space addition to RT*V+TR before RTPS/RTPT perspective division.
 * Valid only inside a render callback. Replaces (never accumulates) the host
 * offset and is restored automatically on completion or watchdog abort.
 * Units are the game's GTE camera units; IPD/world-scale calibration is game
 * specific. Zero is faithful. Guest TR registers are not modified. */
int psx_mod_render_view_offset(int32_t x, int32_t y, int32_t z);
/* Rigid camera transform after guest RT*V+TR, before division. Rotation is
 * row-major Q12; translation uses camera units. Optional projection supplies
 * focal lengths and centre deltas from guest OFX/OFY in Q16 pixel units.
 * Identity rotation with projection=0 preserves the architectural path.
 * projection=1 REPLACES the guest X/Y projection (guest H, widescreen squash and
 * [video] fov_scale do not apply); projection_h_ref != 0 scales the focal lengths
 * by (fov-scaled guest H)/ref. Valid only inside a render callback; restored with it. */
typedef struct PSXModRenderView {
    uint32_t struct_size;
    int32_t rotation_q12[9], translation[3];
    uint32_t projection;
    uint32_t projection_h_ref; /* 0 absolute FOV; otherwise scale focal lengths by guest H/ref */
    int32_t fx_q16, fy_q16, cx_delta_q16, cy_delta_q16;
} PSXModRenderView;
int psx_mod_render_view(const PSXModRenderView *view);
/* Begin locates both views at one predicted time. End submits only a fresh
 * complete pair; failed/shed redraws submit zero layers. */
int psx_mod_openxr_enable(int enabled);
int psx_mod_openxr_begin(uint32_t width, uint32_t height, double units_per_meter);
int psx_mod_openxr_view(uint32_t eye, PSXModRenderView *view);
int psx_mod_openxr_end(int pair_rendered);
/* Frame-local UI surface: the fresh pair's left image is shown to both eyes
 * on a head-relative quad. Call after begin, outside the draw transaction.
 * Dimensions and distance are meters; zero distance restores projection.
 * Requests reset at begin/end and never affect faithful guest rendering. */
int psx_mod_openxr_quad(double distance_m, double width_m, double height_m);
/* Opt-in native presentation surface for boot, videos and menus. Copies the
 * freshly drawn desktop content before host overlays, without guest replay.
 * Zero distance disables it. Applications disable it before scene begin and
 * re-enable when their scene renderer is inactive. Dimensions are meters;
 * height follows the presented content aspect. No retained stereo substitution. */
int psx_mod_openxr_native_surface(double distance_m, double width_m,
                                  double units_per_meter);
void psx_mod_openxr_recenter(void);
/* Fresh action sample at the offline input boundary, never in an eye replay.
 * Positive Y is forward/up in XR. active[] refers only to thumbsticks;
 * other actions have independent activity. Unavailable/unfocused actions
 * return zero values. Click masks are active-high, unrelated to PSX pad bits. */
#define PSX_MOD_XR_PRIMARY   1u /* left X / right A */
#define PSX_MOD_XR_SECONDARY 2u /* left Y / right B */
#define PSX_MOD_XR_MENU      4u /* Touch left Menu */
#define PSX_MOD_XR_STICK     8u /* thumbstick click */
#define PSX_MOD_XR_CLICKS   15u
typedef struct PSXModOpenXRInput {
    uint32_t struct_size, focused, active[2], synthetic;
    float stick[2][2];
    uint64_t sequence;
    float trigger[2], squeeze[2]; /* [0,1] */
    uint32_t trigger_active[2], squeeze_active[2];
    uint32_t buttons[2], buttons_active[2]; /* PSX_MOD_XR_* masks */
} PSXModOpenXRInput;
int psx_mod_openxr_input(PSXModOpenXRInput *input);

/* Read-only controller snapshot from the latest located XR frame. Grip and aim
 * share the eye poses' predicted time and LOCAL space; no action sync or locate
 * occurs here, including during eye replay. Positions are meters; quaternions
 * are x,y,z,w. The origin matches the rendered view's recenter basis. Consumers
 * must check focus, activity, validity and age before using a cached pose. */
enum { PSX_MOD_XR_GRIP_POSE = 0, PSX_MOD_XR_AIM_POSE = 1 };
#define PSX_MOD_XR_ORIENTATION_VALID   1u
#define PSX_MOD_XR_POSITION_VALID      2u
#define PSX_MOD_XR_ORIENTATION_TRACKED 4u
#define PSX_MOD_XR_POSITION_TRACKED    8u
typedef struct PSXModTrackedPose {
    uint32_t active, flags;
    float position_m[3], orientation_xyzw[4];
} PSXModTrackedPose;
typedef struct PSXModOpenXRHands {
    uint32_t struct_size, focused, synthetic, origin_valid;
    uint64_t sequence, predicted_time;
    uint32_t age_ms; /* UINT32_MAX when no real frame has been located */
    double origin_position_m[3], origin_orientation_xyzw[4];
    PSXModTrackedPose pose[2][2]; /* [left/right][grip/aim] */
} PSXModOpenXRHands;
int psx_mod_openxr_hands(PSXModOpenXRHands *hands);



/*
 * When the game flips relative to the pass point. PENDING (default, reset at
 * every session start): the pass rect is the one the next flip will show --
 * the PsyQ VSync(0)-then-PutDispEnv loop, whose frame N is drawn and waits.
 * SHOWN: the game flips each frame as soon as it is drawn (for example
 * PutDispEnv in a VBlank callback armed by the DrawSync callback), so by the
 * time frame N+1's logic is done frame N is already on screen. The pass rect
 * is then that on-screen rect, and frame N's own image and its in-between
 * images are shown from the game's next flip on: one game frame later than
 * the game shows them, as a host interpolator with one frame of history
 * would. Set it from activation, before the first plan.
 */
enum {
    PSX_MOD_RENDER_PASS_FLIP_PENDING = 0,
    PSX_MOD_RENDER_PASS_FLIP_SHOWN = 1
};
int psx_mod_set_render_pass_flip(uint32_t mode);
/* A trusted title may opt into presentation-only passes during forward
 * netplay. Resimulation and lockstep replay still refuse them. The backend
 * restores authoritative CPU VRAM as well as the captured display surface. */
int psx_mod_set_render_pass_netplay(int enabled);

/*
 * The per-frame plumbing every frame-rate plugin repeats (render_pass_frame.c).
 * Blending the game state itself: render_pass_motion.h.
 *
 * Activation: read a choice option holding "display" (follow the measured
 * display refresh) or a frame rate, and select the FLIP source, HOLD blend,
 * `flip_mode` and that rate. Returns 0 if any setting was refused.
 */
int psx_mod_activate_render_pass_rate(const char* package, const char* feature,
                                      const char* option, uint32_t flip_mode);
typedef struct PSXModRenderPassFrame {
    uint32_t struct_size;          /* sizeof(PSXModRenderPassFrame) */
    uint32_t period_vblanks;       /* VBlanks this game frame stays on screen */
    uint32_t shown_after_vblanks;  /* VBlank presents before it is shown */
    uint16_t x, y, w, h;           /* VRAM rect the passes draw */
} PSXModRenderPassFrame;
/*
 * Plan this game frame's passes and run `fn` at each phase. While passes are
 * unavailable for a lasting reason (BACKEND, DISABLED) the presenter is
 * switched to a motion-adaptive crossfade, and back to HOLD once they return.
 * Returns how many passes produced an image.
 */
uint32_t psx_mod_render_pass_frame(struct CPUState* cpu,
                                   const PSXModRenderPassFrame* frame,
                                   PSXModRenderPassFn fn, void* user);

/*
 * Replay part of an already-loaded guest function inside a render pass:
 * run from start_pc with the CPU state given until control reaches stop_pc.
 * Every PC in [start_pc, stop_pc) is interpreted from its RAM bytes, so the
 * span may start anywhere in a compiled function, including just after one
 * of its calls; calls the span makes run normally and return into it.
 * A plugin uses this to redraw with the game's own frame code, branches and
 * all, from registers it captured at start_pc during the real frame (an
 * instruction hook), instead of re-implementing that code's call sequence.
 * Returns 1 when stop_pc is reached; 0 when control leaves the range another
 * way (a return or jump out, a nested span), after 1M interpreted
 * instructions, or outside a pass. The pass restores the machine either way.
 * Both PCs are 4-aligned, in one segment, start_pc < stop_pc.
 */
int psx_mod_run_guest_span(struct CPUState* cpu, uint32_t start_pc,
                           uint32_t stop_pc);
int psx_mod_set_auto_skip_fmv(int enabled);
/*
 * Draw still artwork behind the game image in OpenGL letterbox/pillarbox
 * margins. The image path is an owner-selected mod resource; with no enabled
 * mod/resource path, the margins remain the historical black clear.
 * An absolute path is used unchanged. A relative path (e.g. "bezels/x.png"
 * for artwork a title stages beside its binary) is resolved against the
 * executable's directory, never the current working directory, when the
 * artwork is loaded.
 */
int psx_mod_set_bezel_artwork(const char* path);

/*
 * Upper bounds for the two loading-speed knobs below. Both are generous on
 * purpose: games surface them to players as free-form integers, and neither
 * can corrupt guest state (see the notes on each setter). They exist to reject
 * nonsense, not to curate a list of "blessed" speeds.
 */
#define PSX_MOD_LOAD_ACCEL_MAX  1024u
#define PSX_MOD_DISC_SPEED_MAX  1024u

/*
 * Accelerate only the wall-clock pacing of sustained non-XA data loads while
 * preserving every guest VBlank, CD deadline, interrupt, and callback.
 * wall_clock_multiplier accepts 1..PSX_MOD_LOAD_ACCEL_MAX, or zero for
 * uncapped host speed (1 is a no-op, i.e. authentic pacing).
 * release_frames controls how many guest frames acceleration may remain active
 * after the load predicate clears; zero is the precise/speedrun-safe policy.
 */
int psx_mod_set_load_acceleration(uint32_t wall_clock_multiplier,
                                  uint32_t release_frames);

/*
 * Select guest-visible CD timing for a game-owned loading feature. divisor
 * divides the emulated sector delay, so it IS the speed multiplier: 1 is
 * authentic timing, higher is faster, up to PSX_MOD_DISC_SPEED_MAX. Zero
 * selects the bounded "instant" scheduler, and instant_max_per_frame (1..256)
 * applies only in that case. cdrom.c floors the divided delay at
 * CDROM_MIN_DELAY and leaves XA streaming at authentic timing, so no value
 * here can produce a zero-delay storm or speed up FMV audio. Unlike host load
 * acceleration this changes WHEN the guest receives CD interrupts and can
 * expose game timing bugs, which is why it is a separate, opt-in knob.
 */
int psx_mod_set_disc_speed(uint32_t divisor,
                           uint32_t instant_max_per_frame);

/*
 * Turn the title's [[draw_distance.clamp]] sites on or off for this session
 * (draw_distance.h): a far primitive the game would drop past the end of its
 * ordering table is kept in the farthest slot instead. Returns 1 when the
 * title configured sites, 0 when it has none (the call then changes nothing
 * the guest sees). Off by default and reset to off at every session start;
 * it adds guest work, so call it from activation, never in netplay.
 */
int psx_mod_set_draw_distance_clamp(int enabled);
int psx_mod_draw_distance_clamp_enabled(void);

/* Controller presentation values exposed to trusted game-owned plugins. */
enum {
    PSX_MOD_CONTROLLER_ANALOG = 1,
    PSX_MOD_CONTROLLER_DIGITAL = 2
};
/*
 * Per-sample input facts for an opt-in controller presentation policy. The
 * runtime owns SDL sampling and SIO delivery; the game-owned plugin owns only
 * the policy decision of whether this sample should present as DualShock
 * analog or a digital pad.
 */
typedef struct PSXModControllerInput {
    uint32_t struct_size;
    uint32_t player;
    uint32_t sio_slot;
    uint32_t configured_mode;
    uint32_t current_mode;
    uint32_t stick_active;
    uint32_t dpad_active;
    uint32_t buttons;
    uint32_t lx;
    uint32_t ly;
    uint32_t rx;
    uint32_t ry;
} PSXModControllerInput;
typedef uint32_t (*PSXModControllerPresentationCallback)(
    const PSXModControllerInput* input);
/* Read-only snapshot of the local host stick state for an emulation-thread
 * presentation callback. Values are ordered lx, ly, rx, ry and use the
 * DualShock byte range (0..255, centered at 128). This reads mapped local
 * controller axes independently of the guest's current digital/analog SIO
 * mode and never changes the simulation's controller sample. Returns 0 and
 * centers the output for invalid/disconnected input, netplay, or rollback
 * resimulation; presentation state must never follow synchronized peer input. */
int psx_mod_read_local_pad_sticks(uint32_t player, uint8_t out[4]);
/*
 * Override one player's resolved controller presentation mode for this launch.
 * This is intentionally a trusted-plugin API, not a generic launcher setting.
 */
int psx_mod_set_controller_mode_override(uint32_t player,
                                         uint32_t controller_mode);
/*
 * Let a game-owned plugin choose analog/digital presentation for one player on
 * every input sample. The initial mode is used for boot/hotplug before the
 * first sample. config_capable should be non-zero when the selected policy may
 * present a DualShock, even if a later sample currently reports digital.
 */
int psx_mod_set_controller_presentation_policy(
    uint32_t player,
    PSXModControllerPresentationCallback callback,
    uint32_t initial_mode,
    int config_capable);

/* ---- External offline input: ONE ordered resolution per player ----------
 * Three optional, mod-supplied inputs can act on an offline player's pad. The
 * runtime resolves them in a fixed order in pad_external_input.h:
 *   1. physical/local capture (keyboard, controllers: buttons, sticks, type),
 *      plus the host extras of that port (gamepad present, LT/RT 0..255)
 *   2. offline controller source (psx_mod_set_controller_source): buttons =
 *      source AND physical; sticks/type come from the source, through the
 *      same mode override / multitap rule / presentation policy as a physical
 *      pad. A declined/invalid sample delivers neutral, not the last input.
 *   2b. title pad transform (psx_mod_set_pad_transform): sees the pad of 1-2
 *      plus the host extras and may rewrite buttons, sticks and the presented
 *      controller type (e.g. a NeGcon fed from the triggers). Its output is
 *      validated; an invalid one delivers neutral.
 *   3. local mouse policy (psx_mod_set_local_mouse_policy), P1 only: may
 *      override ONLY the right analog axes of the pad resolved by 1-2. It sees
 *      the final buttons/analog flag/right stick, so it composes with a
 *      source (a source never suppresses it) and no capture is taken to be
 *      discarded. A deflected source right stick, a digital pad, or Start
 *      held disables it exactly as for a physical pad; a source release,
 *      no device, or an armed input guard resets (releases) the capture.
 * Gating. Neither input reaches the guest under netplay or rollback resim,
 * selfcheck replay, headless, or a debug-server input override (which wins and
 * is never mixed with external input). The mouse policy is additionally live
 * only outside selfcheck input lock/resim, render passes, rewind, the
 * savestate menu and the savestate input guard (pad_ext_live). A source is
 * still sampled while the input guard is armed but its output is discarded
 * (neutral). With no source and no mouse policy registered, pad bytes and
 * host event handling are exactly the faithful defaults.
 *
 * ONE registration rule for both: call only on the main (emulation) thread;
 * struct arguments are validated at registration (struct_size) and a bad one
 * is rejected (return 0, logged to stderr); results of each callback are
 * validated per use and fall back to neutral. These are mod-trusted APIs --
 * arbitrary native code, no caller check -- not launcher settings. A source
 * is per player (NULL detaches, with one neutral release frame); the mouse
 * policy is one per session (a second registration returns 0). Callbacks run
 * on the main thread only: sources once per frame, in the normal offline
 * sampler.
 * The source's `analog` is the pad capability; the final type still goes
 * through the multitap rule, mod mode override and presentation policy.
 */
typedef struct PSXModControllerState {
    uint32_t struct_size, buttons, lx, ly, rx, ry, analog;
} PSXModControllerState;
typedef int (*PSXModControllerSource)(PSXModControllerState *state);
int psx_mod_set_controller_source(uint32_t player, PSXModControllerSource source);

/* Title pad transform (stage 2b above). Offline local play only: never under
 * netplay or rollback resim (the session is vanilla), selfcheck replay or a
 * plain debug-server override; the debug host-input layer feeds stage 1 and
 * so does reach it. Main thread; may run more than once per guest frame
 * (low-latency resample), so keep it a function of its input plus game state.
 *
 * The frame is the resolved pad: active-low buttons, sticks (0x80 centred),
 * type (PSX_MOD_PAD_*), and host extras of the port. A player presented as
 * digital (and not driven by a controller source) is given as its host pad
 * instead: the real sticks, and buttons without the stick->D-pad fold, with
 * type still DIGITAL; the transform decides what the guest sees.
 * host_flags bit 0 = a
 * gamepad is assigned, bit 1 / bit 2 = it has a left / right trigger axis;
 * host_lt / host_rt are those triggers, 0 released .. 255 fully pressed
 * (0 when absent or while the savestate input guard is armed).
 *
 * The output arrives pre-filled with the stock pad (what the player gets
 * without a transform, so a digital pad keeps its fold and centred sticks;
 * pressures 0). Return non-zero to apply it, 0 to deliver the stock pad. The
 * type must be one of allowed_types (bit per PSX_MOD_PAD_*); bytes are
 * 0..255 and buttons 0..0xFFFF; anything else delivers a neutral frame.
 * NeGcon uses lx as twist (0x80 centre) and negcon_i / negcon_ii / negcon_l
 * as pressures. A type change reaches SIO through the deferred, idle-bus
 * request; entering or leaving NeGcon is a device swap (sio.h).
 *
 * Registration validates struct_size, a non-NULL callback, allowed_types
 * within the known types and initial_type within allowed_types; a bad one
 * returns 0. initial_type is the type presented at boot/hotplug before the
 * first frame. NULL detaches with one neutral release frame; mod/session
 * reset detaches all (with release). Not registering keeps the faithful
 * default path untouched. */
enum {
    PSX_MOD_PAD_DIGITAL = 0,
    PSX_MOD_PAD_DUALSHOCK = 1,
    PSX_MOD_PAD_JOGCON = 2,
    PSX_MOD_PAD_NEGCON = 3
};
#define PSX_MOD_PAD_TYPE_BIT(type) (1u << (type))
enum {
    PSX_MOD_PAD_HOST_GAMEPAD = 1u << 0,
    PSX_MOD_PAD_HOST_LT = 1u << 1,
    PSX_MOD_PAD_HOST_RT = 1u << 2
};
typedef struct PSXModPadFrame {
    uint32_t struct_size, player, buttons, lx, ly, rx, ry, type;
    uint32_t host_flags, host_lt, host_rt;
} PSXModPadFrame;
typedef struct PSXModPadOutput {
    uint32_t struct_size, buttons, type, lx, ly, rx, ry;
    uint32_t negcon_i, negcon_ii, negcon_l;
} PSXModPadOutput;
typedef struct PSXModPadTransform {
    uint32_t struct_size, allowed_types, initial_type;
    int (*transform)(const PSXModPadFrame *frame, PSXModPadOutput *out);
} PSXModPadTransform;
int psx_mod_set_pad_transform(uint32_t player, const PSXModPadTransform *transform);

/* Let a host shortcut bound to ONE controller button act on that button
 * alone. Without this, a one-button binding means Select + button (the
 * legacy rule), so a title's default one-button binding is safe whether or
 * not its mod is enabled. While allowed (and, for Rewind, while Rewind is
 * enabled) the runtime claims that host button: it is removed from P1's
 * guest pad, and a claim made while it is held lasts until it is released.
 * Call at mod activation; cleared at every mod/session reset. Returns 0 for
 * an unknown shortcut. Multi-button bindings are unaffected. */
enum {
    PSX_MOD_SHORTCUT_REWIND = 0,
    PSX_MOD_SHORTCUT_SAVE_STATE_MENU = 1,
    PSX_MOD_SHORTCUT_FAST_FORWARD = 2,
    PSX_MOD_SHORTCUT_FAST_FORWARD_TOGGLE = 3
};
int psx_mod_allow_direct_shortcut(uint32_t shortcut);

/* Refuse local Rewind while the title is in a mode it must not rewind (e.g.
 * local split-screen multiplayer). While blocked, opening Rewind is refused
 * with an OSD note, no history is captured (snapshots already in the ring are
 * kept), and an allowed direct Rewind shortcut is not in force: its button is
 * not claimed and reaches the guest pad / title transform as if Rewind were
 * disabled. Netplay already refuses Rewind on its own. Opt-in: call with 1
 * when the mode starts and 0 when it ends (setting it every frame from the
 * title's state is fine). Cleared at every mod/session reset. */
void psx_mod_set_rewind_blocked(int blocked);

/* Title-supplied host rumble for one guest port (0-based): DualShock motor
 * values (small: 0 off / nonzero on, large: 0..255) for a title that
 * computes vibration but cannot send it over SIO to the presented pad (e.g. a
 * NeGcon has no motors). The runtime drives the host pad with the louder of
 * this and the guest's own SIO motors. Online only the local seat's value
 * is used (this peer's local port, on its pad) and calls during a rollback resim are
 * ignored, so a netplay = "host_output" plugin can call it for every seat.
 * Opt-in: call every frame while it applies; a value lapses 8 VBlanks after the last call, and all are cleared
 * at every mod/session reset (psx_host_rumble.h). Returns 0 for a bad seat. */
int psx_mod_set_host_rumble(uint32_t player, uint32_t small, uint32_t large);

/* Local P1 mouse policy. The runtime delivers ordered events on the SDL owner
 * (main) thread, owns relative capture and folds the resulting right-stick
 * bytes after native input/presentation (and after any controller source),
 * before normal SIO delivery. No SDL type, guest address, gesture, or game
 * setting belongs in this interface. */
enum {
    PSX_MOD_MOUSE_HOLD_NONE = 0,
    PSX_MOD_MOUSE_HOLD_RIGHT = 1, /* Mouse3 in keybinds.ini */
    PSX_MOD_MOUSE_HOLD_LEFT_ALT = 2,
    PSX_MOD_MOUSE_RESET = 0,
    PSX_MOD_MOUSE_ACQUIRED = 1,
    PSX_MOD_MOUSE_MOTION = 2,
    PSX_MOD_MOUSE_HOLD_PRESS = 3,
    PSX_MOD_MOUSE_HOLD_RELEASE = 4
};
typedef struct PSXModMouseEvent {
    uint32_t struct_size;
    uint32_t type;
    uint64_t time_ns;
    double dx, dy;
} PSXModMouseEvent;
typedef struct PSXModMouseOutput {
    uint32_t struct_size;
    uint32_t override_right;
    uint32_t rx, ry;
} PSXModMouseOutput;
typedef struct PSXModMousePolicy {
    uint32_t struct_size;
    uint32_t hold_control;
    /* Re-read persistent guest context, including pause/menus, on every event
     * and local sample. Must not infer it from a host Start toggle. */
    int (*eligible)(uint32_t native_buttons);
    void (*event)(const PSXModMouseEvent* event);
    /* A query: must not consume motion, change the event anchor or deadlines. */
    void (*sample)(uint64_t now_ns, PSXModMouseOutput* output);
} PSXModMousePolicy;
int psx_mod_set_local_mouse_policy(const PSXModMousePolicy* policy);

/*
 * Register a C plugin before main() on the compilers supported by the runtime.
 * The registry itself uses function-local initialization, so constructor order
 * between game sources and the framework is safe.
 */
#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define PSX_MOD_CONSTRUCTOR(name)                                           \
    static void __cdecl name(void);                                        \
    __declspec(allocate(".CRT$XCU"))                                       \
    static void (__cdecl* name##_constructor)(void) = name;                \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define PSX_MOD_CONSTRUCTOR(name)                                           \
    static void name(void) __attribute__((constructor));                    \
    static void name(void)
#else
#error "PSX mod plugin registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif
