#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PSXModVBlankCallback)(void);
typedef void (*PSXModActivationCallback)(void);
struct CPUState;
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
/* Entry callbacks can make nested guest calls while retaining host registers.
 * Save/load and rewind must wait until that host context has returned. */
int psx_mod_function_entry_active(void);

/* Narrow guest services available to trusted plugin callbacks. */
int psx_mod_game_started(void);
/* Read an original mounted-disc file without changing guest CD state/timing.
 * Emulation-thread callbacks only. NULL buffer + zero capacity queries size;
 * otherwise capacity must hold the entire file. Active sector mods apply. */
int psx_mod_read_disc_file(const char* path, void* buffer, uint32_t capacity,
                           uint32_t* size);
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

/* Session-owned precision selection, applied immediately and retained across
 * renderer initialization. Disabling a mod restores the player's base config
 * at the next session; explicit debug environment overrides still take priority.
 * Does not modify persistent settings or architectural GTE state. */
void psx_mod_set_pgxp_precision(int enabled, int cpu_mode);

/* Current per-side widescreen reveal in native game pixels (zero at 4:3). */
int32_t psx_mod_widescreen_x_margin(void);

/* Mark a guest GPU packet (P_TAG address) as persistent screen-space HUD.
 * edge = -1 left, +1 right, 0 clears a reused packet's tag. The native-wide
 * compositor translates it by the live reveal, excluding culling guards.
 * Guest coordinates, world sprites, and native 4:3 remain unchanged. */
void psx_mod_tag_hud_primitive(uint32_t primitive, int edge);
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
/*
 * Read the committed owner-selected path for a resource declared by the
 * package feature whose trusted plugin is currently running. Returns 0 when
 * the feature has no selected path for that resource; plugins then leave the
 * stock presentation unchanged.
 */
int psx_mod_current_resource_path(const char* resource_id,
                                  char* out, uint32_t out_size);
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
 * or rolled back (state is restored either way). */
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

/* Optional local P1 mouse policy. The runtime delivers ordered events on the
 * SDL owner thread, owns relative capture and folds the resulting right-stick
 * bytes after native input/presentation, before normal SIO delivery. No SDL
 * type, guest address, gesture, or game setting belongs in this interface.
 * This first version is inert during netplay, injected input and replay.
 * Exactly one trusted activation plugin may register per session. */
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
