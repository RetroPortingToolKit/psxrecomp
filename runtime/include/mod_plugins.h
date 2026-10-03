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
/* Called from generated functions listed by the game config and from every
 * interpreted entry, so the hook contract does not depend on the backend.
 * Hooks match by code address (segment bits ignored) and run only for plugins
 * the active plan resolved; the table is rebuilt at plugin activation. */
int psx_mod_register_function_filter_plugin(
    const char* id, uint32_t address, PSXModFunctionFilterCallback callback);
int psx_mod_function_entry(struct CPUState* cpu, uint32_t address);
/* Active function-entry hook count (0 = none). Hot callers test it before the
 * call, so a run without an active hook pays one load per interpreted entry. */
extern uint32_t g_psx_mod_function_entry_hooks;
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

/* Current per-side widescreen reveal in native game pixels (zero at 4:3). */
int32_t psx_mod_widescreen_x_margin(void);

/* Opt into render-only recovery of saturated horizontal GTE projections in
 * native-wide gameplay. Requires exact packet-address/word provenance and
 * depth; never changes guest SXY, vertical coordinates, or the 4:3 path. */
void psx_mod_set_native_wide_projection_correction(int enabled);
/* Bind render-only NCLIP branch consumers to full instruction words. These
 * recover winding only when valid horizontal projections saturated; guest
 * MAC0 and flags remain architectural. Empty registration disables the sites. */
void psx_mod_set_native_wide_nclip_sites(const uint32_t* addresses,
    const uint32_t* expected, int count);

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
 * Identity rotation with projection=0 preserves the architectural path. */
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

/* Trusted offline source owns a player's pad at normal input sampling. A
 * declined/invalid sample delivers neutral, not the previous held input.
 * Existing TCP overrides take priority; netplay/resim and eye redraws never
 * invoke the source. The runtime keeps coherent SIO type requests/recording.
 * Pass NULL to detach. Local keyboard/pad buttons remain merged for menus;
 * the source owns sticks and type. No source leaves faithful defaults intact. */
typedef struct PSXModControllerState {
    uint32_t struct_size, buttons, lx, ly, rx, ry, analog;
} PSXModControllerState;
typedef int (*PSXModControllerSource)(PSXModControllerState *state);
int psx_mod_set_controller_source(uint32_t player, PSXModControllerSource source);

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
