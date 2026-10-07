#ifndef PGXP_H
#define PGXP_H

/* PGXP value-propagation engine — runtime-internal API (pgxp.cpp).
 *
 * The engine shadows every 32-bit word of guest RAM/scratchpad, every GPR,
 * and every GTE data register with the sub-pixel projection it carries
 * (screen X/Y in 16.16 plus the projected SZ depth). The GTE fills shadows at
 * RTPS/RTPT; the psx_pgxp_* hooks (pgxp_hooks.h) move them along with the
 * data; the GPU asks for the precise position of each GP0 vertex word by the
 * packet's RAM address, validated against the actual word — never guessed
 * from the rounded position (the measured G1.4 dead end).
 *
 * Guest-visible state is NEVER touched: shadows are host-only, dropped on
 * savestate/rewind, and suppressed during speculative validation passes.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- lifecycle / configuration ------------------------------------------ */

/* Master arm. Allocates the RAM shadow lazily; fails closed (stays disabled)
 * if the allocation fails. Idempotent. */
void pgxp_set_enabled(int enabled);
int  pgxp_enabled(void);
/* Armed and not inside a suppression bracket (speculative/replay pass):
 * whether the hooks record anything right now. */
int  pgxp_active(void);

/* Tier-2 propagation through CPU arithmetic (default off, like the reference
 * implementations' default). Off is SAFE — value validation already stops
 * stale shadows — it only bounds coverage. Live-tunable (TCP). */
void pgxp_set_cpu_mode(int enabled);
int  pgxp_cpu_mode(void);

/* Reject a precise position whose sub-pixel offset from the native integer
 * position exceeds this many pixels. < 0 disables the clamp (default). */
void  pgxp_set_tolerance(float pixels);
float pgxp_tolerance(void);

/* Position-cache fallback tier (docs/ENHANCEMENTS.md G1.4/G1.11). On (the
 * default, unchanged behaviour): a vertex with no validated dataflow shadow
 * may take the sub-pixel fraction of the projection last cached at its integer
 * screen position. Off ("dataflow only"): such a vertex draws native, and
 * gte.cpp stops filling the cache. A title whose build carries the PGXP hooks
 * reaches near-total dataflow coverage, so for it the cache only adds wrong
 * fractions to CPU-built 2D polygons that happen to share a position with
 * some unrelated 3D vertex. Live-tunable. */
void pgxp_set_position_fallback(int enabled);
int  pgxp_position_fallback(void);

/* Preserve projection precision (docs/ENHANCEMENTS.md G1.11). Off (the
 * default): RTPS/RTPT shadow the pre-truncation position the GTE itself
 * computes from the integer IR1/IR2 and its UNR divide, so the shadow always
 * truncates to the guest's SXY. On: they shadow the exact projection of the
 * vertex, from the unshifted MACs (12 more fractional bits than IR/SZ3) and a
 * true divide (pgxp_project_precise), which removes the residual wobble of
 * near geometry. Guest-visible SXY, MAC0 and FLAG are unchanged either way.
 * Because that position can then differ from the guest integer by more than
 * the fraction, truncation agreement becomes a window (PGXP_PPP_AGREE_*) and
 * a vertex at the GTE's saturation limit must still agree exactly. One
 * consumer is not visual: [widescreen] precise_nclip takes its branch sign
 * from these shadows, so in a title that sets both, that (already
 * non-faithful) widescreen sign follows the exact projection too. */
void pgxp_set_preserve_projection(int enabled);
int  pgxp_preserve_projection(void);

/* Truncation-agreement window under preserve-projection, in whole pixels:
 * accept -BELOW < precise - native < ABOVE. For a value-validated shadow
 * whose packet half is neither at the GTE saturation limit nor wrapped by the
 * GPU's 11-bit parse (both of which need exact agreement), the guest
 * integer is the IR path's floor of the same projection, so the difference is
 * bounded by the IR path's own error: the dropped fraction (< 1 px), IR1/IR2
 * truncation times H/SZ3 (< 2 px without divide overflow), and SZ3's
 * truncation scaling the offset from the screen centre by up to
 * |offset|/SZ3. The window is a sanity bound over that, not a filter of
 * correct vertices: for R4 (H = 290) every on-screen vertex lies within it
 * (measured range -1.9 .. 2.6 px), and what it rejects is an extreme near
 * vertex far off-screen, which then draws where the hardware puts it. */
#define PGXP_PPP_AGREE_BELOW 4
#define PGXP_PPP_AGREE_ABOVE 5

/* The exact projection for preserve-projection mode, in the 16.16 screen
 * space of the IR path. Inputs are RTPS's unshifted 44-bit MAC1..3, the shift
 * (12 for sf=1), the IR1/IR2 the GTE stored, the SZ3 it pushed, H, OFX/OFY
 * and the horizontal widescreen factor x_num/x_den the IR path applied (1/1
 * when none). Returns 1 with the position in x16/y16 when the vertex
 * qualifies (sf=1, IR1/IR2 not saturated, SZ3 not saturated and nonzero, and
 * no divide overflow: H < 2*SZ3), else 0 and the caller keeps the IR path. */
int pgxp_project_precise(int64_t mac1, int64_t mac2, int64_t mac3, int shift,
                         int32_t ir1, int32_t ir2, uint32_t sz3, uint32_t h,
                         int32_t ofx, int32_t ofy, int64_t x_num, int64_t x_den,
                         int32_t *x16, int32_t *y16);

/* Whether an exact projection may be the shadow of the SXY word `packed`:
 * the consumer's truncation agreement (the PGXP_PPP_AGREE_* window, exact at
 * the saturation limits) on both halves. When it fails, RTPS keeps the IR
 * path's shadow, which always agrees, so the vertex still draws precise
 * instead of being rejected at the GPU. Counts ppp_produced on accept and
 * ppp_window_fallback on reject. */
int pgxp_ppp_accept(int32_t x16, int32_t y16, uint32_t packed);

/* Precise culling (docs/ENHANCEMENTS.md G1.12). Off (the default): NCLIP is
 * exactly the hardware's. On, while geometry correction is armed: when the
 * three SXY FIFO shadows are live, describe the current register words and
 * pass the same acceptance the GPU applies when it draws them, and the sign
 * of their exact determinant differs from the integer one, NCLIP's MAC0
 * takes the exact sign (magnitude: the exact doubled area rounded, at least
 * 1). A game that culls on that sign then keeps the sub-pixel faces PGXP
 * draws with positive area -- the far road rows a native zero-area test
 * drops, which open gaps between their precisely placed neighbours -- and
 * drops the slivers whose drawn winding is reversed. This CHANGES
 * guest-visible MAC0 and so guest control flow: only the mod arms it
 * (netplay clears the mod), and gte.cpp holds it off in every pass that is
 * compared against another execution (overlay shadow diff, speculative and
 * replay passes). Live-tunable over TCP. */
void pgxp_set_culling(int enabled);
int  pgxp_culling(void);

/* NCLIP's exact determinant from the SXY0..2 register shadows, in 2^-32
 * px^2 units (16.16 times 16.16). Returns 1 when all three shadows are live,
 * carry the register words in sxy[] and pass the GPU consumer's truncation
 * agreement and tolerance clamp; else 0. Counts nclip_precise. */
int pgxp_gte_nclip_precise(const uint32_t sxy[3], int64_t *cross);

/* Mod-owned request (the framework's psx.enhancement.pgxp package).
 *
 * The mod's activation runs at session start, before main.cpp's renderer
 * setup, and that setup applies the [video] baseline (geometry_correction,
 * perspective_texturing, pgxp_cpu_mode). An activation that armed the
 * corrections directly was switched straight back off there. So the
 * activation only records a request, and the session arming
 * (psx_pgxp_session_arm, pgxp_session.h) takes it -- reads and clears it --
 * and combines it with the baseline. A request therefore lives only from one
 * session's activation to that session's arming: a later session whose plan
 * is empty (netplay, or the mod disabled) cannot inherit it.
 * reset_mod_owned_presentation() also clears it at every session start. */
void pgxp_mod_request(int enabled, int cpu_mode, int culling);
/* Returns nonzero when this session's plan asked for PGXP; *cpu_mode and
 * *culling (either may be NULL) receive the mod's options. */
int  pgxp_mod_requested(int *cpu_mode, int *culling);
/* pgxp_mod_requested, then clears the request. */
int  pgxp_mod_request_take(int *cpu_mode, int *culling);

/* Drop all shadows (savestate load, raw RAM restore, timeline breaks).
 * O(1) via generation bump. Deferred while suppressed. */
void pgxp_invalidate_all(void);

/* Counted suppression bracket for speculative native-validation passes and
 * the GTE replay sandbox: hooks and producers no-op inside it. */
void pgxp_suppress_begin(void);
void pgxp_suppress_end(void);

/* --- GTE producer (gte.cpp) ---------------------------------------------- */

/* Called at the RTPS/RTPT projection with the pre-truncation 16.16 screen
 * coordinates, the projected depth (SZ3), and the packed SXY word the guest
 * sees. Shifts the shadow FIFO exactly like push_sxy (regs 12..15). */
void pgxp_gte_push_sxy(int32_t x16, int32_t y16, uint16_t sz3, uint32_t packed);

/* Host-only homogeneous screen projection, before SZ/SXY/divider saturation.
 * x/z and y/z are screen pixels; signed z retains camera-plane crossings.
 * Exact full-word copies carry it. Arithmetic/partial writes invalidate it. */
typedef struct PGXPProjection { float x, y, z, near_z; } PGXPProjection;
void pgxp_set_projection_tracking(int enabled);
int pgxp_projection_tracking(void);
void pgxp_gte_set_projection(const PGXPProjection *projection);
int pgxp_load_projection(uint32_t addr, uint32_t packed, PGXPProjection *out);
int pgxp_get_gte_projection(uint32_t index, uint32_t packed, PGXPProjection *out);

/* Current SXY FIFO shadow (index 0..3 selects GTE data regs 12..15).
 * Returns nonzero when the shadow is live and carries X/Y precision. */
int pgxp_get_gte_sxy(uint32_t index, int32_t *x16, int32_t *y16);
/* Validate-on-read variant: check!=0 additionally requires the shadow's packed
 * word to equal expect (the live guest SXY word). Sign consumers must use this
 * form so stale FIFO shadows fail closed to native integer behavior. */
int pgxp_get_gte_sxy_checked(uint32_t index, uint32_t expect, int check,
                             int32_t *x16, int32_t *y16);

/* Guest write to a GTE data register outside gte_execute (MTC2/CTC2 handled
 * by psx_pgxp_cop2; this is for direct gte_write_data paths): reg 15 performs
 * the SXYP FIFO push on the shadows, others just invalidate/overwrite. */
void pgxp_gte_reg_written(int reg, uint32_t value);

/* --- GPU consumer (gpu.c) ------------------------------------------------ */

enum {
    PGXP_SRC_NATIVE   = 0,   /* no shadow — caller uses the parsed integers  */
    PGXP_SRC_FALLBACK = 1,   /* position-cache fallback (never carries depth)*/
    PGXP_SRC_DATAFLOW = 2,   /* address-keyed shadow, value-validated        */
};

/* Precise position for one GP0 vertex word.
 *   addr        guest address the word was DMA'd from, or 0xFFFFFFFF if
 *               unknown (immediate GP0 writes) — skips the dataflow tier.
 *   packet_word the actual word (validation key).
 *   int_x/int_y the natively parsed 11-bit positions (pre-draw-offset).
 * On DATAFLOW/FALLBACK, *x16/*y16 hold the sub-pixel position (16.16, same
 * coordinate space as the packet halves); *sz is the projected depth or 0.
 * Safeguards applied here: the integer part must match the native parse
 * (truncation agreement; a bounded window under preserve-projection) and the
 * tolerance clamp. The fallback tier is skipped when position fallback is
 * off. */
int pgxp_get_precise_vertex(uint32_t addr, uint32_t packet_word,
                            int32_t int_x, int32_t int_y,
                            int32_t *x16, int32_t *y16, uint16_t *sz);

/* Same decision as pgxp_get_precise_vertex, without counting it in the stats
 * (the caller looks the vertex up again when it draws). */
int pgxp_probe_precise_vertex(uint32_t addr, uint32_t packet_word,
                              int32_t int_x, int32_t int_y);

/* --- observability -------------------------------------------------------- */

typedef struct PGXPStats {
    uint64_t lookups;            /* pgxp_get_precise_vertex calls            */
    uint64_t dataflow_hit;
    uint64_t fallback_hit;
    uint64_t native;
    uint64_t value_mismatch;     /* shadow present but wrong word (stale)    */
    uint64_t trunc_reject;       /* integer part disagreed with native parse */
    uint64_t tolerance_reject;
    uint64_t w_valid;            /* lookups that also carried a usable depth */
    uint64_t produced;           /* RTPS/RTPT projections pushed into shadows */
    uint64_t swc2_stores;        /* GTE reg shadows copied to RAM shadows     */
    uint64_t ppp_produced;       /* of those, shadowed with the exact projection */
    uint64_t ppp_window_fallback;/* exact projection outside the agreement
                                  * window: shadowed with the IR path instead */
    /* Geometry-corrected triangles by how many of their 3 vertices came out
     * precise (gpu.c prepare_precise_triangle). `mixed` is the mesh-cracking
     * exposure of docs/ENHANCEMENTS.md G1.1: an edge between a precise and a
     * native vertex can disagree with its neighbour by the dropped fraction. */
    uint64_t tri_precise;        /* all three precise                         */
    uint64_t tri_mixed;          /* one or two precise                        */
    uint64_t tri_native;         /* none precise                              */
    /* Textured quads that are axis-aligned rectangles in integer screen space
     * (and in UV) whose four vertices are all dataflow-precise, so they were
     * drawn as two precise triangles instead of the native 2D rectangle
     * shortcut; rect_partial: such rectangles with 1..3 precise vertices,
     * which keep the shortcut (drawing them as triangles would mix). */
    uint64_t rect_bypass;
    uint64_t rect_partial;
    /* NCLIPs while geometry correction is armed (docs/ENHANCEMENTS.md G1.12):
     * nclip_precise had an exact determinant; nclip_disagree: its sign (non-
     * zero) differs from the integer MAC0's, so the game's cull decision on
     * that face disagrees with the face PGXP draws -- the crack exposure that
     * tri_* cannot see, because a culled face draws no triangle at all;
     * nclip_corrected: of those, MAC0 replaced by precise culling. */
    uint64_t nclip_precise;
    uint64_t nclip_disagree;
    uint64_t nclip_corrected;
    /* pgxp_load_precise_word outcomes (perspective texturing, per vertex):
     * which check refused the depth. untracked = no shadow for that address in
     * this generation; mismatch = shadow describes a different word; partial =
     * only one half carries a projection; no_z = projection without depth. */
    uint64_t word_lookups;
    uint64_t word_hit;
    uint64_t word_untracked;
    uint64_t word_mismatch;
    uint64_t word_partial;
    uint64_t word_no_z;
} PGXPStats;

void pgxp_get_stats(PGXPStats *out);
/* Count one geometry-corrected triangle with `precise` (0..3) precise
 * vertices. */
void pgxp_note_triangle(int precise);
/* One rectangle-shortcut quad sent down the triangle path (all_precise) or
 * kept on the shortcut with only some precise vertices. */
void pgxp_note_rect_bypass(int all_precise);
/* One NCLIP whose exact sign disagreed with MAC0, and whether MAC0 was
 * corrected. */
void pgxp_note_nclip(int disagree, int corrected);

/* --- gte.cpp forwarding surface (v14 ABI compat) -------------------------- */

/* SWC2 site: copy the GTE register shadow (regs 12..15) to the RAM shadow. */
void pgxp_store_gte_reg(uint32_t addr, uint8_t reg);

/* Address-keyed precise-word lookup (perspective texturing path). Returns
 * nonzero when the tracked word matches `packed` AND carries a depth. */
int pgxp_load_precise_word(uint32_t addr, uint32_t packed,
                           int32_t *x16, int32_t *y16, uint16_t *z);

/* Render-pass checkpoint: journal every shadow a sandboxed pass mutates and
 * put the shadows back when the sandbox restores the machine. Use through
 * gte_precision_checkpoint_begin / _rollback (which also cover gte.cpp). */
void pgxp_checkpoint_begin(void);
void pgxp_checkpoint_rollback(void);

/* Exact address-keyed packet precision, for a renderer preserving completed
 * static packets around a smaller interpolated drawing section. `expected`
 * MUST be the actual destination RAM/scratchpad word, read by the caller
 * immediately before either operation (the same convention as the lookups
 * above). Neither operation reads MMIO or changes guest memory.
 *
 * Capture also succeeds for an absent/stale/value-mismatched shadow, recording
 * a valid no-shadow state. Restore is allowed only in an active checkpoint,
 * only at the captured canonical address, and only when the capture generation
 * equals that checkpoint's ENTRY generation. It may then rebind live metadata
 * to the sandbox's current generation. This never promotes an older timeline's
 * stale metadata. All restores, including clearing a shadow, are journaled.
 * Receipts are runtime-only host data: do not persist them in game saves. */
typedef struct PGXPWordShadow {
    uint32_t address, value, source_generation, valid;
    int32_t x16, y16;
    uint16_t z, flags;
    PGXPProjection projection;
} PGXPWordShadow;
int pgxp_capture_word_shadow(uint32_t addr, uint32_t expected, PGXPWordShadow *out);
int pgxp_restore_word_shadow(uint32_t addr, uint32_t expected, const PGXPWordShadow *in);

/* Explicit packet relocation variant. The caller owns and validates both
 * packet ranges and their relocation map, writes the destination guest word,
 * then supplies that word's current value as expected. The captured source
 * must still name canonical RAM/scratchpad, but need not equal the destination.
 * All other restore rules above apply, including checkpoint ENTRY generation.
 * Only destination metadata changes; an absent receipt clears its shadow.
 * Rollback restores the destination's previous metadata. */
int pgxp_restore_relocated_word_shadow(uint32_t destination, uint32_t expected,
                                       const PGXPWordShadow *in);

/* Refused precise-word lookups, newest at (seq - 1) % cap (TCP pgxp_miss_ring). */
enum {
    PGXP_MISS_UNTRACKED = 1,
    PGXP_MISS_MISMATCH  = 2,
    PGXP_MISS_PARTIAL   = 3,
    PGXP_MISS_NO_Z      = 4
};
#define PGXP_MISS_RING_CAP 8192u
typedef struct PGXPWordMiss {
    uint64_t seq;
    uint32_t addr;          /* packet word address (canonical RAM offset)   */
    uint32_t packet;        /* the word the GPU consumed                    */
    uint32_t shadow_value;  /* the word the shadow describes                */
    uint8_t  shadow_flags;  /* 1 = X, 2 = Y, 4 = Z                          */
    uint8_t  live;          /* shadow belongs to the current generation     */
    uint8_t  reason;        /* PGXP_MISS_*                                  */
} PGXPWordMiss;
/* Returns the total number of misses ever recorded. */
uint64_t pgxp_word_miss_ring(const PGXPWordMiss **ring, uint32_t *cap);

/* MVMVA as a multiplier (scalar tier, cpu-mode). gte.cpp reports every
 * MVMVA with the operands exactly as it used them and the results it wrote;
 * the engine recomputes the rows from the precise matrix elements (CTC2'd
 * tracked halves), vector (IR scalars or tracked V halves) and translation,
 * and leaves the results as scalars on MAC1..3 / IR1..3 for MFC2. */
typedef struct PGXPMvmva {
    uint32_t mx, vv, tv;     /* matrix / vector / translation selectors       */
    int      shift;          /* 12 when sf = 1, else 0                        */
    int16_t  m[3][3];        /* matrix elements as used                       */
    int16_t  v[3];           /* vector as used                                */
    int64_t  t[3];           /* translation as used (already << 12)           */
    int32_t  mac[3];         /* MAC1..3 written                               */
    int32_t  ir[3];          /* IR1..3 written                                */
    uint32_t flag;           /* FLAG after the op                             */
} PGXPMvmva;
void pgxp_gte_mvmva(const PGXPMvmva *op);
/* Called after every GTE command (its function code): the IR / MAC scalars it
 * overwrote end there. */
void pgxp_gte_op_end(uint32_t func);

/* Geometry-corrected triangles, newest at (seq - 1) % cap (TCP pgxp_tri_ring).
 * Always-on: every triangle prepare_precise_triangle resolves, with each
 * vertex's packet integers, the position handed to the rasterizer and where
 * that position came from. Two triangles that share a packet vertex (same
 * integers, same frame) but were handed different positions are a crack:
 * the ring is what localizes a seam to the producer of its vertices. */
#define PGXP_TRI_RING_CAP 65536u
typedef struct PGXPTriRecord {
    uint64_t seq;
    uint32_t frame;         /* s_frame_count when the triangle was drawn      */
    uint32_t src_addr;      /* GP0 packet base address (0xFFFFFFFF = none)    */
    uint8_t  op;            /* GP0 opcode                                     */
    uint8_t  pass;          /* drawn inside a render-pass checkpoint (frame   */
                            /* smoothing redraws); set by the engine          */
    uint8_t  src[3];        /* PGXP_SRC_* per vertex                          */
    uint8_t  vidx[3];       /* packet word index of each vertex               */
    int16_t  raw_x[3];      /* parsed 11-bit packet integers (pre-offset)     */
    int16_t  raw_y[3];
    uint32_t word[3];       /* the packet words                               */
    int32_t  x16[3];        /* position handed to the rasterizer (16.16,     */
    int32_t  y16[3];        /* draw offset and widescreen already applied)   */
} PGXPTriRecord;
void pgxp_note_triangle_detail(const PGXPTriRecord *rec);
/* Returns the total number of triangles ever recorded. */
uint64_t pgxp_tri_ring(const PGXPTriRecord **ring, uint32_t *cap);

/* Shadowed memory writes, newest at (seq - 1) % cap (TCP pgxp_store_ring).
 * Always-on while the engine is armed: every hooked SW / SH / SB / SWC2 into
 * RAM or scratchpad, with the storing PC, the source register's shadow state
 * and the shadow the destination word holds afterwards. Walking a packet
 * word's provenance backwards (store PC -> source register -> the load that
 * filled it) is how a precision loss is pinned to one instruction. */
#define PGXP_STORE_RING_CAP 262144u
typedef struct PGXPStoreRecord {
    uint64_t seq;
    uint32_t frame;
    uint32_t pc;            /* g_debug_last_store_pc of the store           */
    uint32_t addr;          /* guest address written                        */
    uint32_t value;         /* the value stored (half / byte for SH / SB)   */
    uint8_t  op;            /* primary opcode (0x2B SW, 0x29 SH, 0x3A SWC2) */
    uint8_t  reg;           /* source GPR (or GTE data register for SWC2)   */
    uint8_t  src_live;      /* source shadow belongs to this generation     */
    uint8_t  pad;
    uint16_t src_flags;     /* source shadow flags (1 X, 2 Y, 4 Z)          */
    uint16_t dst_flags;     /* destination word shadow flags afterwards     */
    int32_t  dst_x16, dst_y16;
} PGXPStoreRecord;
uint64_t pgxp_store_ring(const PGXPStoreRecord **ring, uint32_t *cap);
/* The host's store-PC breadcrumb and frame counter, for the rings above. */
void pgxp_set_trace_sources(const uint32_t *store_pc, const uint64_t *frame);

/* Debug read of one shadow slot (TCP pgxp_shadow). `space`: 0 = guest
 * address (RAM / scratchpad), 1 = GPR index (32 = HI, 33 = LO), 2 = GTE data
 * register. Returns 0 when the slot does not exist; *live says whether it
 * belongs to the current generation. */
int pgxp_debug_shadow(int space, uint32_t key, int *live, uint32_t *value,
                      uint32_t *flags, int32_t *x16, int32_t *y16,
                      uint16_t *z);

/* --- test accessors (always compiled; trivial) ---------------------------- */

/* index 0..3 selects the SXY0..SXYP register shadow (GTE data regs 12..15). */
void pgxp_test_seed_gte_sxy(uint32_t index, uint32_t packed,
                            int32_t x16, int32_t y16, uint16_t z, int valid);
void pgxp_test_get_gte_sxy(uint32_t index, uint32_t *packed,
                           int32_t *x16, int32_t *y16, uint16_t *z,
                           uint8_t *valid);
uint32_t pgxp_test_generation(void);
uint32_t pgxp_test_suppress_depth(void);
int      pgxp_test_active(void);
void     pgxp_test_set_generation(uint32_t gen);

#ifdef __cplusplus
}
#endif

#endif /* PGXP_H */
