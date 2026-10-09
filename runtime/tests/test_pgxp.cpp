/* test_pgxp.cpp — PGXP value-propagation engine unit tests (docs/ENHANCEMENTS.md
 * G1.2/G1.3). White-box over runtime/src/pgxp.cpp with the gte.cpp fallback
 * cache stubbed, exercising exactly the properties the engine's safety rests
 * on: provenance roundtrips, validate-on-read, half-word semantics, the
 * repack arithmetic, the suppression bracket, and the GPU-side safeguards. */

#include "pgxp.h"
#include "pgxp_hooks.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---- gte.cpp fallback-cache stub ----------------------------------------- */

static uint32_t g_fb_packed = 0;
static int32_t  g_fb_x16 = 0, g_fb_y16 = 0;
static int      g_fb_valid = 0;
static uint32_t g_fb_probe_lookups = 0;
static uint32_t g_fb_render_lookups = 0;

extern "C" int gte_geometry_correction_lookup(uint32_t packed,
                                              int32_t *x16, int32_t *y16) {
    ++g_fb_render_lookups;
    if (!g_fb_valid || packed != g_fb_packed) return 0;
    if (x16) *x16 = g_fb_x16;
    if (y16) *y16 = g_fb_y16;
    return 1;
}

extern "C" int gte_geometry_correction_lookup_probe(uint32_t packed,
                                                     int32_t *x16,
                                                     int32_t *y16) {
    ++g_fb_probe_lookups;
    if (!g_fb_valid || packed != g_fb_packed) return 0;
    if (x16) *x16 = g_fb_x16;
    if (y16) *y16 = g_fb_y16;
    return 1;
}

/* ---- MIPS encodings ------------------------------------------------------ */

static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | imm;
}
static uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh,
                      uint32_t funct) {
    return (rs << 21) | (rt << 16) | (rd << 11) | (sh << 6) | funct;
}
static uint32_t enc_cop2(uint32_t sub, uint32_t rt, uint32_t rd) {
    return (0x12u << 26) | (sub << 21) | (rt << 16) | (rd << 11);
}

#define LW(rs, rt)   enc_i(0x23, rs, rt, 0)
#define SW(rs, rt)   enc_i(0x2B, rs, rt, 0)
#define LH(rs, rt)   enc_i(0x21, rs, rt, 0)
#define LHU(rs, rt)  enc_i(0x25, rs, rt, 0)
#define SH(rs, rt)   enc_i(0x29, rs, rt, 0)
#define SB(rs, rt)   enc_i(0x28, rs, rt, 0)
#define LWC2(rt)     enc_i(0x32, 1, rt, 0)
#define SWC2(rt)     enc_i(0x3A, 1, rt, 0)
#define MFC2(rt, rd) enc_cop2(0x00, rt, rd)
#define MTC2(rt, rd) enc_cop2(0x04, rt, rd)
#define ADDIU(rs, rt, imm) enc_i(0x09, rs, rt, (uint16_t)(imm))
#define LUI(rt, imm) enc_i(0x0F, 0, rt, (uint16_t)(imm))
#define SLL(rt, rd, sh) enc_r(0, rt, rd, sh, 0x00)
#define SRA(rt, rd, sh) enc_r(0, rt, rd, sh, 0x03)
#define OR(rs, rt, rd)  enc_r(rs, rt, rd, 0, 0x25)
#define ADDU(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x21)
#define SUBU(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x23)
#define AND(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x24)
#define NOR(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x27)
#define SLT(rs, rt, rd) enc_r(rs, rt, rd, 0, 0x2A)
#define ANDI(rs, rt, imm) enc_i(0x0C, rs, rt, (uint16_t)(imm))
#define ORI(rs, rt, imm) enc_i(0x0D, rs, rt, (uint16_t)(imm))

/* One projected vertex: x = 160.5, y = 80.25 -> packed integer word. */
static const uint32_t PACKED  = (80u << 16) | 160u;
static const int32_t  X16     = (160 << 16) | 0x8000;   /* 160.5  */
static const int32_t  Y16     = (80 << 16)  | 0x4000;   /* 80.25  */
static const uint16_t SZ3     = 100;

static const uint32_t ADDR_A  = 0x80100000u;   /* packet slot A (KSEG0)  */
static const uint32_t ADDR_B  = 0x00100040u;   /* packet slot B (KUSEG)  */

static void produce_at(uint32_t addr) {
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, SWC2(14), PACKED, addr);
}

static int lookup(uint32_t addr, uint32_t word, int32_t ix, int32_t iy,
                  int32_t *x, int32_t *y, uint16_t *z) {
    int32_t lx, ly; uint16_t lz;
    int r = pgxp_get_precise_vertex(addr, word, ix, iy, &lx, &ly, &lz);
    if (x) *x = lx;
    if (y) *y = ly;
    if (z) *z = lz;
    return r;
}

int main(void) {
    pgxp_set_enabled(1);
    pgxp_set_tolerance(-1.0f);
    pgxp_set_cpu_mode(0);

    /* --- SWC2 produce -> GPU consume (the perspective-texturing spine) --- */
    produce_at(ADDR_A);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        /* mirrors resolve to the same shadow word */
        CHECK(lookup(0xA0100000u, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
    }

    /* --- DMA/untracked overwrite: value validation rejects the shadow --- */
    {
        int32_t x, y; uint16_t z;
        uint32_t other = (81u << 16) | 161u;
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_NATIVE);
        CHECK(x == (161 << 16) && y == (81 << 16) && z == 0);
    }

    /* --- DMA/host rewrite of the identical word: equal bits are not the
     * same projection, so memory.c drops the shadow by provenance; a
     * render pass rolls the drop back with the rest of its writes --- */
    produce_at(ADDR_A);
    pgxp_invalidate_word(0xA0100000u);           /* a mirror of ADDR_A     */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    produce_at(ADDR_A);
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);
    pgxp_checkpoint_begin();
    pgxp_invalidate_word(ADDR_A);
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_checkpoint_rollback();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);
    pgxp_invalidate_word(0x1F000000u);           /* untracked: no slot     */

    /* --- LW/SW roundtrip: packet copied by the CPU keeps provenance --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
    }

    /* --- stale GPR: register changed between load and store --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, 0xDEADBEEFu);   /* r8 mutated */
    CHECK(lookup(ADDR_B, 0xDEADBEEFu, 0, 0, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- coprocessor reads replace the GPR: a word equal to the vertex the
     * register held is still not that vertex (MFC0 decodes through the ALU
     * hook, CFC2 through the COP2 hook) --- */
    {
        const uint32_t mfc0 = (0x10u << 26) | (8u << 16) | (12u << 11);
        produce_at(ADDR_A);
        psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
        psx_pgxp_alu(nullptr, mfc0, PACKED, 0, 0);
        psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED);
        CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
        psx_pgxp_cop2(nullptr, enc_cop2(0x02, 8, 31), PACKED, 0);  /* CFC2 */
        psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED);
        CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
    }

    /* --- MOVE idiom (memory mode, no cpu_mode needed) --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDU(8, 0, 10), PACKED, PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- MFC2 -> SW (register transfer path) --- */
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, MFC2(9, 14), PACKED, 0);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, PACKED);
    CHECK(lookup(ADDR_B, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);

    /* --- LH/SH: halves travel independently, depth does not survive --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B + 2u, PACKED >> 16);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A, PACKED & 0xFFFFu);     /* X   */
    psx_pgxp_store(nullptr, SH(1, 8), ADDR_B, PACKED & 0xFFFFu);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
        CHECK(z == 0);                       /* SH killed the vertex depth   */
    }

    /* --- SB destroys the touched half only --- */
    produce_at(ADDR_B);
    psx_pgxp_store(nullptr, SB(1, 8), ADDR_B, PACKED & 0xFFu);  /* same byte */
    {
        int32_t x, y; uint16_t z;
        /* low half invalidated -> not a full XY hit anymore */
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, &z) != PGXP_SRC_DATAFLOW);
    }

    /* --- cpu-mode repack: lhu / sll 16 / or (the classic vertex build) --- */
    pgxp_set_cpu_mode(1);
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);     /* Y   */
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_load(nullptr, LHU(1, 10), ADDR_A, PACKED & 0xFFFFu);    /* X   */
    psx_pgxp_alu(nullptr, OR(9, 10, 11), PACKED,
                 (PACKED >> 16) << 16, PACKED & 0xFFFFu);
    psx_pgxp_store(nullptr, SW(1, 11), ADDR_B, PACKED);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED, 160, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16);
    }

    /* --- cpu-mode addiu: fraction rides an integer offset (incl. -N) --- */
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, 4), PACKED + 4u, PACKED, 4u);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED + 4u);
    {
        int32_t x, y;
        CHECK(lookup(ADDR_B, PACKED + 4u, 164, 80, &x, &y, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 + (4 << 16) && y == Y16);
    }
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 12, (uint16_t)-4), PACKED - 4u, PACKED,
                 (uint32_t)(int32_t)-4);
    psx_pgxp_store(nullptr, SW(1, 12), ADDR_B, PACKED - 4u);
    {
        int32_t x;
        CHECK(lookup(ADDR_B, PACKED - 4u, 156, 80, &x, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 - (4 << 16));
    }
    pgxp_set_cpu_mode(0);

    /* --- cpu-mode OFF: the same repack must degrade to native, cleanly --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LHU(1, 8), ADDR_A + 2u, PACKED >> 16);
    psx_pgxp_alu(nullptr, SLL(8, 9, 16), (PACKED >> 16) << 16,
                 PACKED >> 16, 16);
    psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, (PACKED >> 16) << 16);
    CHECK(lookup(ADDR_B, (PACKED >> 16) << 16, 0, 80, nullptr, nullptr,
                 nullptr) == PGXP_SRC_NATIVE);

    /* --- clip flags packed above the GPU field (Spider-Man 0x8007F31C):
     *   mfc2 t2,SXY2; subu t9,t2,t4; nor t9,t9,t5; and t2,t2,t5; or t2,t2,t9
     *   ... and t2,t2,a3; or t2,t2,t9'; sw t2
     * The flag ops rewrite bits 14/15 and 30/31 of the word, never the 11-bit
     * fields GP0 decodes, so the vertex (and its depth) must survive - in
     * BOTH tiers, since bitwise carries are exact. --- */
    for (int mode = 0; mode < 2; mode++) {
        pgxp_set_cpu_mode(mode);
        const uint32_t T2 = 10, T4 = 12, T5 = 13, T9 = 25, A3 = 7;
        const uint32_t bounds = (240u << 16) | 128u;       /* x >= 128 -> flag */
        const uint32_t m14 = 0xBFFFBFFFu, m15 = 0x7FFF7FFFu;
        pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
        psx_pgxp_cop2(nullptr, MFC2(T2, 14), PACKED, 0);
        psx_pgxp_load(nullptr, LW(1, T4), 0x80180000u, bounds);  /* untracked */
        psx_pgxp_alu(nullptr, LUI(T5, 0xBFFF), 0xBFFF0000u, 0, 0);
        psx_pgxp_alu(nullptr, ORI(T5, T5, 0xBFFF), m14, 0xBFFF0000u, 0xBFFFu);
        uint32_t d = PACKED - bounds;
        psx_pgxp_alu(nullptr, SUBU(T2, T4, T9), d, PACKED, bounds);
        uint32_t f = ~(d | m14);
        psx_pgxp_alu(nullptr, NOR(T9, T5, T9), f, d, m14);
        uint32_t w = PACKED & m14;
        psx_pgxp_alu(nullptr, AND(T2, T5, T2), w, PACKED, m14);
        psx_pgxp_alu(nullptr, OR(T2, T9, T2), w | f, w, f);
        w |= f;
        CHECK(w == (PACKED | 0x4000u));                    /* x flagged      */
        psx_pgxp_alu(nullptr, AND(T2, A3, T2), w & m15, w, m15);
        w &= m15;
        psx_pgxp_load(nullptr, LW(1, T9), 0x80180004u, 0x80000000u);
        psx_pgxp_alu(nullptr, OR(T2, T9, T2), w | 0x80000000u, w, 0x80000000u);
        w |= 0x80000000u;
        psx_pgxp_store(nullptr, SW(1, T2), ADDR_B, w);
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, w, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        uint16_t wz = 0;
        CHECK(pgxp_load_precise_word(ADDR_B, w, nullptr, nullptr, &wz) == 1);
        CHECK(wz == SZ3);
    }
    pgxp_set_cpu_mode(0);

    /* --- quad from a projected-vertex table (Spider-Man 0x8007C5D4):
     *   lwc2 SXY0..2 <- table; NCLIP; lwc2 SXYP <- table; NCLIP;
     *   swc2 SXY0..2 -> packet
     * The SXYP write pushes the FIFO; the stored words are corners 1..3. --- */
    {
        const uint32_t TABLE = 0x80180100u, PKT = 0x80180200u;
        uint32_t words[4];
        for (uint32_t i = 0; i < 4; i++) {
            words[i] = ((80u + i) << 16) | (160u + i);
            pgxp_gte_push_sxy((int32_t)((160 + i) << 16) | 0x8000,
                              (int32_t)((80 + i) << 16) | 0x4000,
                              (uint16_t)(100 + i), words[i]);
            psx_pgxp_cop2(nullptr, SWC2(14), words[i], TABLE + i * 4u);
        }
        for (uint32_t i = 0; i < 3; i++) {          /* lwc2 SXY0..2        */
            pgxp_gte_reg_written((int)(12 + i), words[i]);
            psx_pgxp_cop2(nullptr, enc_i(0x32, 1, 12 + i, 0), words[i],
                          TABLE + i * 4u);
        }
        pgxp_gte_reg_written(15, words[3]);          /* lwc2 SXYP: push     */
        psx_pgxp_cop2(nullptr, enc_i(0x32, 1, 15, 0), words[3], TABLE + 12u);
        for (uint32_t i = 0; i < 3; i++)
            psx_pgxp_cop2(nullptr, SWC2(12 + i), words[i + 1], PKT + i * 4u);
        for (uint32_t i = 0; i < 3; i++) {
            int32_t x, y; uint16_t z;
            CHECK(lookup(PKT + i * 4u, words[i + 1], 161 + (int32_t)i,
                         81 + (int32_t)i, &x, &y, &z) == PGXP_SRC_DATAFLOW);
            CHECK(x == ((int32_t)((161 + i) << 16) | 0x8000));
            CHECK(z == 101 + i);
        }
    }

    /* --- render-pass checkpoint: a sandboxed pass rewrites a packet word
     * (raw-restored afterwards); the shadow must come back with it --- */
    {
        const uint32_t other = (90u << 16) | 170u;
        produce_at(ADDR_A);
        pgxp_checkpoint_begin();
        pgxp_gte_push_sxy((170 << 16) | 0x1000, (90 << 16) | 0x2000, 7, other);
        psx_pgxp_cop2(nullptr, SWC2(14), other, ADDR_A);
        CHECK(lookup(ADDR_A, other, 170, 90, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);                /* the pass sees its own word */
        pgxp_invalidate_all();                   /* undone by the rollback     */
        pgxp_suppress_begin();                   /* left open by an abort      */
        pgxp_checkpoint_rollback();
        CHECK(pgxp_test_suppress_depth() == 0);
        CHECK(pgxp_test_active());
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(x == X16 && y == Y16 && z == SZ3);
        /* a generation wrap inside the pass cannot be undone: fail closed */
        produce_at(ADDR_A);
        pgxp_checkpoint_begin();
        pgxp_test_set_generation(0xFFFFFFFFu);
        pgxp_invalidate_all();
        pgxp_checkpoint_rollback();
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
    }

    /* --- in-place ops read their source before the destination is reset --- */
    pgxp_set_cpu_mode(1);
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ADDIU(8, 8, 4), PACKED + 4u, PACKED, 4u);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED + 4u);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_B, PACKED + 4u, 164, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);
        CHECK(x == X16 + (4 << 16) && y == Y16);
        CHECK(z == SZ3);                       /* vertex + offset keeps depth */
    }
    pgxp_set_cpu_mode(0);

    /* --- a mask that changes the GPU field does not carry that half --- */
    produce_at(ADDR_A);
    psx_pgxp_load(nullptr, LW(1, 8), ADDR_A, PACKED);
    psx_pgxp_alu(nullptr, ANDI(8, 8, 0x0080), PACKED & 0x80u, PACKED, 0x80u);
    psx_pgxp_store(nullptr, SW(1, 8), ADDR_B, PACKED & 0x80u);
    CHECK(lookup(ADDR_B, PACKED & 0x80u, 128, 0, nullptr, nullptr, nullptr) !=
          PGXP_SRC_DATAFLOW);
    CHECK(pgxp_load_precise_word(ADDR_B, PACKED & 0x80u, nullptr, nullptr,
                                 nullptr) == 0);

    /* --- a saturated projection is not that half's value: never carried --- */
    {
        const uint32_t sat = (80u << 16) | 1023u;          /* x clamped     */
        pgxp_gte_push_sxy(2000 << 16, Y16, SZ3, sat);
        psx_pgxp_cop2(nullptr, MFC2(10, 14), sat, 0);
        psx_pgxp_load(nullptr, LW(1, 25), 0x80180008u, 0);  /* untracked 0   */
        psx_pgxp_alu(nullptr, OR(10, 25, 10), sat, sat, 0);
        psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, sat);
        CHECK(pgxp_load_precise_word(ADDR_B, sat, nullptr, nullptr, nullptr) == 0);
    }

    /* --- SLT-family results are not vertices: the destination resets --- */
    pgxp_gte_push_sxy(X16, Y16, SZ3, PACKED);
    psx_pgxp_cop2(nullptr, MFC2(10, 14), PACKED, 0);
    psx_pgxp_alu(nullptr, SLT(0, 10, 10), 1u, 0, PACKED);
    psx_pgxp_store(nullptr, SW(1, 10), ADDR_B, 1u);
    CHECK(lookup(ADDR_B, 1u, 1, 0, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- truncation agreement: integer part must match the native parse --- */
    produce_at(ADDR_A);
    CHECK(lookup(ADDR_A, PACKED, 161, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- tolerance clamp --- */
    produce_at(ADDR_A);
    pgxp_set_tolerance(0.25f);                 /* fraction is 0.5 -> reject  */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_set_tolerance(0.75f);                 /* 0.5 <= 0.75 -> accept      */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_DATAFLOW);
    pgxp_set_tolerance(-1.0f);

    /* --- the clamp keeps the native position but the validated depth --- */
    {
        int32_t x, y; uint16_t z = 0;
        pgxp_set_tolerance(0.25f);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_NATIVE);
        CHECK(x == (160 << 16) && y == (80 << 16) && z == SZ3);
        pgxp_set_tolerance(-1.0f);
        /* a truncation reject (CPU-modified integer) carries no depth */
        z = 1;
        CHECK(lookup(ADDR_A, PACKED, 161, 80, &x, &y, &z) == PGXP_SRC_NATIVE);
        CHECK(z == 0);
    }

    /* --- trusted-mod guest regions are tracked like RAM --- */
    {
        static const uint32_t APERTURE = 0x809D0004u;   /* GPU DMA aperture   */
        static const uint32_t MODMEM   = 0x9F000040u;   /* Expansion 1 memory */
        int32_t x, y; uint16_t z;
        produce_at(APERTURE);
        CHECK(lookup(APERTURE & 0x00FFFFFFu, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_DATAFLOW);                  /* 24-bit DMA tag form   */
        CHECK(x == X16 && y == Y16 && z == SZ3);
        CHECK(lookup(0x801D0004u, PACKED, 160, 80, nullptr, nullptr, nullptr) !=
              PGXP_SRC_DATAFLOW);                  /* never aliases its RAM mirror */
        produce_at(MODMEM);
        CHECK(lookup(MODMEM, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
    }

    /* --- fallback tier: no address -> position cache, never a depth --- */
    g_fb_valid = 1; g_fb_packed = PACKED; g_fb_x16 = X16; g_fb_y16 = Y16;
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(0xFFFFFFFFu, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_FALLBACK);
        CHECK(x == X16 && y == Y16 && z == 0);
    }
    g_fb_valid = 0;

    /* --- dataflow only (G1.11): position fallback off skips the cache --- */
    g_fb_valid = 1; g_fb_packed = PACKED; g_fb_x16 = X16; g_fb_y16 = Y16;
    CHECK(pgxp_position_fallback() == 1);              /* default: unchanged */
    pgxp_set_position_fallback(0);
    {
        int32_t x, y; uint16_t z;
        CHECK(lookup(0xFFFFFFFFu, PACKED, 160, 80, &x, &y, &z) ==
              PGXP_SRC_NATIVE);
        CHECK(x == (160 << 16) && y == (80 << 16) && z == 0);
        /* a validated dataflow shadow is unaffected */
        produce_at(ADDR_A);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        /* a stale shadow no longer falls through to the cache */
        uint32_t other = (81u << 16) | 161u;
        g_fb_packed = other;
        g_fb_x16 = (161 << 16) | 0x8000;
        g_fb_y16 = (81 << 16) | 0x4000;
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_NATIVE);
        pgxp_set_position_fallback(1);
        CHECK(lookup(ADDR_A, other, 161, 81, &x, &y, &z) == PGXP_SRC_FALLBACK);
    }
    g_fb_valid = 0;

    /* --- preserve projection (G1.11): the agreement window ---------------- */
    {
        CHECK(pgxp_preserve_projection() == 0);        /* default: unchanged */
        auto seed = [](int32_t x16, int32_t y16, uint32_t packed) {
            pgxp_gte_push_sxy(x16, y16, SZ3, packed);
            psx_pgxp_cop2(nullptr, SWC2(14), packed, ADDR_A);
        };
        const int32_t half = 1 << 15;
        /* exact projection half a pixel BELOW the guest integer: the IR path
         * can never produce that, so off rejects it and on accepts it */
        seed((160 << 16) - half, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        pgxp_set_preserve_projection(1);
        {
            int32_t x, y;
            CHECK(lookup(ADDR_A, PACKED, 160, 80, &x, &y, nullptr) ==
                  PGXP_SRC_DATAFLOW);
            CHECK(x == (160 << 16) - half && y == Y16);
        }
        /* window edges: (-BELOW, +ABOVE) px exclusive */
        const int32_t hi = PGXP_PPP_AGREE_ABOVE, lo = PGXP_PPP_AGREE_BELOW;
        seed(((160 + hi) << 16) - 1, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        seed((160 + hi) << 16, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        seed(((160 - lo) << 16) + 1, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        seed((160 - lo) << 16, Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        seed(X16, (80 + hi) << 16, PACKED);            /* Y axis too      */
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        /* a half beyond the GTE range, which the GPU's 11-bit parse wraps
         * (a CPU-modified word), needs exact agreement: 0x0402 parses as
         * -1022, a shadow at 1026.5 lies 2048 px away and one at -1021.5
         * truncates to -1022 */
        const uint32_t wrap = (80u << 16) | 0x0402u;
        seed((1026 << 16) + half, Y16, wrap);
        CHECK(lookup(ADDR_A, wrap, -1022, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        seed(-(1022 << 16) + half, Y16, wrap);
        CHECK(lookup(ADDR_A, wrap, -1022, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        seed(-(1022 << 16) - half, Y16, wrap);   /* in the window, not exact */
        CHECK(lookup(ADDR_A, wrap, -1022, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        /* an 11-bit wrapped coordinate is far outside any window */
        seed(X16 + (2048 << 16), Y16, PACKED);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        /* at the GTE saturation limit the guest integer is a clamp: the
         * window does not apply, agreement must be exact */
        const uint32_t sat = (80u << 16) | 0x3FFu;   /* SX saturated at 1023 */
        seed((1023 << 16) + half, Y16, sat);
        CHECK(lookup(ADDR_A, sat, 1023, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);                    /* exact agreement ok */
        seed((1024 << 16) + half, Y16, sat);
        CHECK(lookup(ADDR_A, sat, 1023, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        const uint32_t sat_lo = (0xFC00u << 16) | 160u;  /* SY at -1024 */
        seed(X16, -(1024 << 16) - half, sat_lo);
        CHECK(lookup(ADDR_A, sat_lo, 160, -1024, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        /* the tolerance clamp measures the offset both ways */
        seed((160 << 16) - half, Y16, PACKED);
        pgxp_set_tolerance(0.25f);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_NATIVE);
        pgxp_set_tolerance(0.75f);
        CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
              PGXP_SRC_DATAFLOW);
        pgxp_set_tolerance(-1.0f);
        pgxp_set_preserve_projection(0);
    }

    /* --- pgxp_project_precise: the exact projection and its qualifiers --- */
    {
        /* V = (100, -50, 1000) through the identity: MAC = V * 4096 (sf=1),
         * IR = V, SZ3 = 1000, H = 300, OFX/OFY = (160, 120) in 16.16. The
         * exact screen position is (160 + 100 * 0.3, 120 - 50 * 0.3). */
        const int64_t m1 = 100 * 4096 + 1234;   /* fractional MAC bits     */
        const int64_t m2 = -50 * 4096 + 777;
        const int64_t m3 = 1000 * 4096 + 2048;
        const int32_t ofx = 160 << 16, ofy = 120 << 16;
        int32_t x, y;
        CHECK(pgxp_project_precise(m1, m2, m3, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), (uint32_t)(m3 >> 12),
                                   300, ofx, ofy, 1, 1, &x, &y) == 1);
        const double ex = 160.0 + (double)m1 * 300.0 / (double)m3;
        const double ey = 120.0 + (double)m2 * 300.0 / (double)m3;
        CHECK(x == (int32_t)std::floor(ex * 65536.0));
        CHECK(y == (int32_t)std::floor(ey * 65536.0));
        /* the horizontal widescreen factor is applied to X only */
        int32_t xs, ys;
        CHECK(pgxp_project_precise(m1, m2, m3, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), (uint32_t)(m3 >> 12),
                                   300, ofx, ofy, 3, 4, &xs, &ys) == 1);
        CHECK(ys == y);
        CHECK(xs == (int32_t)std::floor((160.0 + (double)m1 * 300.0 /
                                         (double)m3 * 0.75) * 65536.0));
        /* disqualified: sf=0, a clamped IR, SZ3 0 or clamped, divide
         * overflow (H >= 2*SZ3) -- the caller keeps the IR path */
        CHECK(pgxp_project_precise(m1, m2, m3, 0, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), 1000, 300, ofx, ofy,
                                   1, 1, &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, m3, 12, 0x7FFF, (int32_t)(m2 >> 12),
                                   1000, 300, ofx, ofy, 1, 1, &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, 2048, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), 0, 300, ofx, ofy, 1, 1,
                                   &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, (int64_t)0x12345 << 12, 12,
                                   (int32_t)(m1 >> 12), (int32_t)(m2 >> 12),
                                   0xFFFF, 300, ofx, ofy, 1, 1, &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, 150 * 4096, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), 150, 300, ofx, ofy, 1, 1,
                                   &x, &y) == 0);
        CHECK(pgxp_project_precise(m1, m2, 151 * 4096, 12, (int32_t)(m1 >> 12),
                                   (int32_t)(m2 >> 12), 151, 300, ofx, ofy, 1, 1,
                                   &x, &y) == 1);
    }

    /* --- probe: the same decision, not counted --- */
    {
        produce_at(ADDR_A);
        PGXPStats a, b;
        pgxp_get_stats(&a);
        CHECK(pgxp_probe_precise_vertex(ADDR_A, PACKED, 160, 80) ==
              PGXP_SRC_DATAFLOW);
        CHECK(pgxp_probe_precise_vertex(ADDR_A, PACKED, 161, 80) ==
              PGXP_SRC_NATIVE);                /* truncation reject */
        CHECK(pgxp_probe_precise_vertex(ADDR_A, PACKED ^ 1u, 160, 80) ==
              PGXP_SRC_NATIVE);                /* value mismatch */
        const float tolerance_was = pgxp_tolerance();
        pgxp_set_tolerance(0.1f);
        CHECK(pgxp_probe_precise_vertex(ADDR_A, PACKED, 160, 80) ==
              PGXP_SRC_NATIVE);                /* tolerance reject */
        pgxp_set_tolerance(tolerance_was);
        pgxp_get_stats(&b);
        CHECK(std::memcmp(&a, &b, sizeof a) == 0);
        pgxp_note_rect_bypass(1);
        pgxp_note_rect_bypass(0);
        pgxp_note_rect_bypass(0);
        pgxp_get_stats(&b);
        CHECK(b.rect_bypass == a.rect_bypass + 1);
        CHECK(b.rect_partial == a.rect_partial + 2);

        const int fallback_was_enabled = pgxp_position_fallback();
        pgxp_set_position_fallback(1);
        g_fb_valid = 1;
        g_fb_packed = PACKED;
        g_fb_x16 = 160 << 16;
        g_fb_y16 = 80 << 16;
        const uint32_t render_lookups = g_fb_render_lookups;
        const uint32_t probe_lookups = g_fb_probe_lookups;
        pgxp_get_stats(&a);
        CHECK(pgxp_probe_precise_vertex(0xFFFFFFFFu, PACKED, 160, 80) ==
              PGXP_SRC_FALLBACK);
        pgxp_get_stats(&b);
        CHECK(std::memcmp(&a, &b, sizeof a) == 0);
        CHECK(g_fb_render_lookups == render_lookups);
        CHECK(g_fb_probe_lookups == probe_lookups + 1u);
        int32_t x16, y16;
        uint16_t sz;
        CHECK(pgxp_get_precise_vertex(0xFFFFFFFFu, PACKED, 160, 80,
                                      &x16, &y16, &sz) == PGXP_SRC_FALLBACK);
        CHECK(g_fb_render_lookups == render_lookups + 1u);
        CHECK(g_fb_probe_lookups == probe_lookups + 1u);
        g_fb_valid = 0;
        pgxp_set_position_fallback(fallback_was_enabled);
    }

    /* --- preserve projection: the RTPS-side window check (G1.11) ---------- */
    {
        pgxp_set_preserve_projection(1);
        PGXPStats a, b;
        pgxp_get_stats(&a);
        const int32_t half = 1 << 15;
        /* inside the window: the exact projection is the shadow */
        CHECK(pgxp_ppp_accept((160 << 16) - half, Y16, PACKED) == 1);
        CHECK(pgxp_ppp_accept(((160 + PGXP_PPP_AGREE_ABOVE) << 16) - 1, Y16,
                              PACKED) == 1);
        /* outside it (either axis): RTPS keeps the IR path's shadow */
        CHECK(pgxp_ppp_accept((160 + PGXP_PPP_AGREE_ABOVE) << 16, Y16,
                              PACKED) == 0);
        CHECK(pgxp_ppp_accept(X16, (80 - PGXP_PPP_AGREE_BELOW) << 16,
                              PACKED) == 0);
        /* at the saturation limit agreement is exact */
        const uint32_t sat = (80u << 16) | 0x3FFu;
        CHECK(pgxp_ppp_accept((1023 << 16) + half, Y16, sat) == 1);
        CHECK(pgxp_ppp_accept((1024 << 16) + half, Y16, sat) == 0);
        pgxp_get_stats(&b);
        CHECK(b.ppp_produced == a.ppp_produced + 3);
        CHECK(b.ppp_window_fallback == a.ppp_window_fallback + 3);
        pgxp_set_preserve_projection(0);
    }

    /* --- NCLIP's exact determinant (G1.12) -------------------------------- */
    {
        /* A far road row: native y 113, 113, 113 (zero area), exact y
         * 112.64 / 113.12 / 113.83 -> positive area. */
        const uint32_t w0 = (113u << 16) | 100u;
        const uint32_t w1 = (113u << 16) | 220u;
        const uint32_t w2 = (113u << 16) | 160u;
        const int32_t y0 = (112 << 16) + (int32_t)(0.64 * 65536);
        const int32_t y1 = (113 << 16) + (int32_t)(0.12 * 65536);
        const int32_t y2 = (113 << 16) + (int32_t)(0.83 * 65536);
        auto seed3 = [&](void) {
            pgxp_test_seed_gte_sxy(0, w0, 100 << 16, y0, SZ3, 1);
            pgxp_test_seed_gte_sxy(1, w1, 220 << 16, y1, SZ3, 1);
            pgxp_test_seed_gte_sxy(2, w2, 160 << 16, y2, SZ3, 1);
        };
        const uint32_t words[3] = { w0, w1, w2 };
        auto expect_cross = [&](void) {
            return ((int64_t)(220 << 16) - (100 << 16)) * ((int64_t)y2 - y0) -
                   ((int64_t)y1 - y0) * ((int64_t)(160 << 16) - (100 << 16));
        };
        PGXPStats a, b;
        pgxp_get_stats(&a);
        int64_t cross = 0;
        /* IR path: y0 = 112.64 does not truncate to 113 -> not believed */
        seed3();
        CHECK(pgxp_gte_nclip_precise(words, &cross) == 0);
        /* preserve-projection window: believed, exact determinant */
        pgxp_set_preserve_projection(1);
        seed3();
        CHECK(pgxp_gte_nclip_precise(words, &cross) == 1);
        CHECK(cross == expect_cross());
        CHECK(cross > 0);
        /* a stale shadow (word changed under it) fails closed */
        const uint32_t stale[3] = { w0, w1, w2 ^ 1u };
        CHECK(pgxp_gte_nclip_precise(stale, &cross) == 0);
        /* the tolerance clamp applies as at the GPU */
        pgxp_set_tolerance(0.25f);
        CHECK(pgxp_gte_nclip_precise(words, &cross) == 0);
        pgxp_set_tolerance(-1.0f);
        /* nothing inside a suppression bracket */
        pgxp_suppress_begin();
        CHECK(pgxp_gte_nclip_precise(words, &cross) == 0);
        pgxp_suppress_end();
        pgxp_get_stats(&b);
        CHECK(b.nclip_precise == a.nclip_precise + 1);
        pgxp_note_nclip(1, 0);
        pgxp_note_nclip(1, 1);
        pgxp_get_stats(&b);
        CHECK(b.nclip_disagree == a.nclip_disagree + 2);
        CHECK(b.nclip_corrected == a.nclip_corrected + 1);
        pgxp_set_preserve_projection(0);
    }

    /* --- the mod request lives until the session's arming takes it -------- */
    {
        CHECK(pgxp_culling() == 0);                    /* default off         */
        pgxp_set_culling(1);
        CHECK(pgxp_culling() == 1);
        pgxp_set_culling(0);
        int cpu = -1, cull = -1;
        pgxp_mod_request(1, 1, 1);
        CHECK(pgxp_mod_requested(&cpu, &cull) == 1 && cpu == 1 && cull == 1);
        CHECK(pgxp_mod_requested(nullptr, nullptr) == 1);   /* peek keeps it */
        CHECK(pgxp_mod_request_take(&cpu, &cull) == 1 && cpu == 1 && cull == 1);
        CHECK(pgxp_mod_request_take(&cpu, &cull) == 0 && cpu == 0 && cull == 0);
        pgxp_mod_request(0, 1, 1);                     /* options need the mod */
        CHECK(pgxp_mod_requested(&cpu, &cull) == 0 && cpu == 0 && cull == 0);
    }

    /* --- triangle census (G1.1 crack exposure) --- */
    {
        PGXPStats a, b;
        pgxp_get_stats(&a);
        pgxp_note_triangle(3);
        pgxp_note_triangle(2);
        pgxp_note_triangle(1);
        pgxp_note_triangle(0);
        pgxp_get_stats(&b);
        CHECK(b.tri_precise == a.tri_precise + 1);
        CHECK(b.tri_mixed == a.tri_mixed + 2);
        CHECK(b.tri_native == a.tri_native + 1);
    }

    /* --- suppression bracket: nothing records inside it --- */
    pgxp_invalidate_all();
    pgxp_suppress_begin();
    produce_at(ADDR_A);
    pgxp_suppress_end();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- deferred invalidate inside the bracket --- */
    produce_at(ADDR_A);
    pgxp_suppress_begin();
    pgxp_invalidate_all();                     /* deferred                   */
    pgxp_suppress_end();                       /* applies here               */
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);

    /* --- invalidate-all + generation wrap --- */
    produce_at(ADDR_A);
    pgxp_invalidate_all();
    CHECK(lookup(ADDR_A, PACKED, 160, 80, nullptr, nullptr, nullptr) ==
          PGXP_SRC_NATIVE);
    pgxp_test_set_generation(0xFFFFFFFFu);
    pgxp_invalidate_all();
    CHECK(pgxp_test_generation() == 1u);

    /* --- test accessors mirror the SXY FIFO shadows --- */
    pgxp_test_seed_gte_sxy(2, PACKED, X16, Y16, SZ3, 1);
    {
        uint32_t packed; int32_t x, y; uint16_t z; uint8_t valid;
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(valid && packed == PACKED && x == X16 && y == Y16 && z == SZ3);
        pgxp_test_seed_gte_sxy(2, 0, 0, 0, 0, 0);
        pgxp_test_get_gte_sxy(2, &packed, &x, &y, &z, &valid);
        CHECK(!valid);
    }

    /* Saturated SXY retains the original projection in exact transport. The
     * ordinary precision consumer stays conservative; the opt-in native-wide
     * renderer can query the address/word-validated projection separately. */
    {
        const uint32_t packed = (152u << 16) | 1023u;
        pgxp_gte_push_sxy(1040 * 65536, 152 * 65536 + 17000, 994, packed);
        psx_pgxp_cop2(nullptr, MFC2(9, 14), packed, 0);
        psx_pgxp_store(nullptr, SW(1, 9), ADDR_B, packed);
        int32_t x, y; uint16_t z;
        CHECK(pgxp_load_precise_word(ADDR_B, packed, &x, &y, &z));
        CHECK(x == 1040 * 65536 && y == 152 * 65536 + 17000 && z == 994);
        CHECK(!pgxp_load_precise_word(ADDR_B, packed ^ 1, &x, &y, &z));
        CHECK(lookup(ADDR_B, packed, 1023, 152, &x, &y, &z) == PGXP_SRC_NATIVE);
    }

    /* Signed camera depth survives exact word transport and sandbox rollback;
     * byte-identical partial writes must still discard the 3D association. */
    {
        PGXPProjection p={12345.5f,-7311.0f,-62.0f,150.0f}, out;
        pgxp_set_projection_tracking(1);
        pgxp_gte_push_sxy(X16,Y16,0,PACKED);
        pgxp_gte_set_projection(&p);
        CHECK(pgxp_get_gte_projection(2,PACKED,&out) && out.z==-62.0f);
        CHECK(!pgxp_get_gte_projection(2,PACKED^1,&out));
        psx_pgxp_cop2(nullptr,MFC2(9,14),PACKED,0);
        psx_pgxp_store(nullptr,SW(1,9),ADDR_B,PACKED);
        CHECK(pgxp_load_projection(ADDR_B,PACKED,&out) && out.x==p.x && out.z==p.z);
        CHECK(!pgxp_load_projection(ADDR_B,PACKED^1,&out));
        pgxp_checkpoint_begin();
        psx_pgxp_store(nullptr,SB(1,9),ADDR_B,PACKED&255);
        CHECK(!pgxp_load_projection(ADDR_B,PACKED,&out));
        pgxp_checkpoint_rollback();
        CHECK(pgxp_load_projection(ADDR_B,PACKED,&out) && out.z==p.z);
        psx_pgxp_store(nullptr,SH(1,9),ADDR_B,PACKED&65535);
        CHECK(!pgxp_load_projection(ADDR_B,PACKED,&out));
        pgxp_store_gte_reg(ADDR_B,14);
        CHECK(pgxp_load_projection(ADDR_B,PACKED,&out));
        pgxp_invalidate_all();
        CHECK(!pgxp_load_projection(ADDR_B,PACKED,&out));
        pgxp_set_projection_tracking(0);
    }

    /* --- scalar tier: GTE-as-multiplier edge midpoint (Ape Escape terrain
     *     subdivision, 0x8001DB74): CTC2 two tracked X halves into a light-
     *     matrix row, MVMVA by the vertex depths, MFC2 MAC1, ADDU the rounding
     *     bias, DIV by the depth sum, MFLO, SH into the vertex table. --- */
    {
        pgxp_set_cpu_mode(1);
        pgxp_set_tolerance(1.0f);
        const uint32_t TBL = 0x1F800280u;
        /* v0 (100.9, 50.5) z 200, v1 (121.9, 60.25) z 300 */
        const uint32_t w0 = (50u << 16) | 100u, w1 = (60u << 16) | 121u;
        pgxp_gte_push_sxy((100 << 16) | 0xE666, (50 << 16) | 0x8000, 200, w0);
        psx_pgxp_cop2(nullptr, SWC2(14), w0, TBL + 0);
        pgxp_gte_push_sxy((121 << 16) | 0xE666, (60 << 16) | 0x4000, 300, w1);
        psx_pgxp_cop2(nullptr, SWC2(14), w1, TBL + 8);

        /* lh t0,0(s1); lh v0,8(s1); andi v1,t0,0xffff; sll a0,v0,16;
         * or v1,v1,a0; ctc2 v1,$8 */
        psx_pgxp_load(nullptr, LH(17, 8), TBL + 0, 100u);
        psx_pgxp_load(nullptr, LH(17, 2), TBL + 8, 121u);
        psx_pgxp_alu(nullptr, ANDI(8, 3, 0xFFFF), 100u, 100u, 0xFFFFu);
        psx_pgxp_alu(nullptr, SLL(2, 4, 16), 121u << 16, 121u, 16u);
        psx_pgxp_alu(nullptr, OR(3, 4, 3), (121u << 16) | 100u, 100u, 121u << 16);
        psx_pgxp_cop2(nullptr, enc_cop2(0x06, 3, 8), (121u << 16) | 100u, 0);
        /* mtc2 t9(200),IR1; mtc2 t8(300),IR2; mtc2 zero,IR3 (exact) */
        psx_pgxp_alu(nullptr, ADDIU(0, 25, 200), 200u, 0u, 200u);
        psx_pgxp_alu(nullptr, ADDIU(0, 24, 300), 300u, 0u, 300u);
        psx_pgxp_cop2(nullptr, MTC2(25, 9), 200u, 0);
        psx_pgxp_cop2(nullptr, MTC2(24, 10), 300u, 0);
        psx_pgxp_cop2(nullptr, MTC2(0, 11), 0u, 0);

        /* MVMVA sf=0 mx=LLM v=IR cv=none: MAC1 = 100*200 + 121*300 = 56300 */
        PGXPMvmva op;
        std::memset(&op, 0, sizeof op);
        op.mx = 1; op.vv = 3; op.tv = 3; op.shift = 0;
        op.m[0][0] = 100; op.m[0][1] = 121;
        op.v[0] = 200; op.v[1] = 300; op.v[2] = 0;
        op.mac[0] = 56300; op.ir[0] = 0x7FFF;
        op.flag = 1u << 24;                       /* IR1 saturated          */
        pgxp_gte_mvmva(&op);
        pgxp_gte_op_end(0x12);

        /* mfc2 a1,MAC1; addu a1,a1,a3(0); addu v0,t9,t8; div a1,v0; mflo a1 */
        psx_pgxp_cop2(nullptr, MFC2(5, 25), 56300u, 0);
        psx_pgxp_alu(nullptr, ADDIU(0, 7, 0), 0u, 0u, 0u);
        psx_pgxp_alu(nullptr, ADDU(5, 7, 5), 56300u, 56300u, 0u);
        psx_pgxp_alu(nullptr, ADDU(25, 24, 2), 500u, 200u, 300u);
        psx_pgxp_muldiv(nullptr, enc_r(5, 2, 0, 0, 0x1A), 56300u % 500u,
                        56300u / 500u, 56300u, 500u);
        psx_pgxp_alu(nullptr, enc_r(0, 0, 5, 0, 0x12), 112u, 112u, 0u);
        /* sh a1,0x48(s1) */
        psx_pgxp_store(nullptr, SH(17, 5), TBL + 0x48, 112u);

        /* precise midpoint: (100.9*200 + 121.9*300) / 500 = 113.5 */
        uint32_t live = 0, value = 0, flags = 0; int32_t sx = 0, sy = 0; uint16_t sz = 0;
        int lv = 0;
        CHECK(pgxp_debug_shadow(0, TBL + 0x48, &lv, &value, &flags, &sx, &sy, &sz));
        (void)live;
        CHECK(lv == 1 && (flags & 1u) != 0);
        CHECK(std::fabs(sx / 65536.0 - 113.5) < 1e-3);
        CHECK((flags & 0x10u) != 0);              /* derived: floor 113 != 112 */

        /* y half from a non-derived value; then the whole word reaches a
         * packet and the GPU believes it inside the derived window. */
        psx_pgxp_load(nullptr, LH(17, 6), TBL + 2, 50u);
        psx_pgxp_store(nullptr, SH(17, 6), TBL + 0x4A, 50u);
        const uint32_t mid = (50u << 16) | 112u;
        psx_pgxp_cop2(nullptr, LWC2(12), mid, TBL + 0x48);
        psx_pgxp_cop2(nullptr, SWC2(12), mid, ADDR_A);
        int32_t x, y; uint16_t z;
        CHECK(lookup(ADDR_A, mid, 112, 50, &x, &y, &z) == PGXP_SRC_DATAFLOW);
        CHECK(std::fabs(x / 65536.0 - 113.5) < 1e-3 && y == ((50 << 16) | 0x8000));

        /* tolerance clamps movement beyond the guest rounding pixel */
        pgxp_set_tolerance(0.25f);
        CHECK(lookup(ADDR_A, mid, 112, 50, &x, &y, &z) == PGXP_SRC_NATIVE);
        pgxp_set_tolerance(1.0f);

        /* a scalar does not survive a write that does not produce one: the
         * register reloaded from memory stores imprecise */
        psx_pgxp_load(nullptr, LW(17, 5), TBL + 0x100, 112u);
        psx_pgxp_store(nullptr, SH(17, 5), TBL + 0x58, 112u);
        CHECK(pgxp_debug_shadow(0, TBL + 0x58, &lv, &value, &flags, &sx, &sy, &sz));
        CHECK((flags & 1u) == 0);

        /* the lwl/lwr + swl/swr word-copy idiom on an aligned address moves
         * whole words, so it carries the vertex (Ape Escape's terrain grid
         * copies its projected corners this way, 0x80044B00) */
        {
            const uint32_t LWL = enc_i(0x22, 17, 11, 0), LWR = enc_i(0x26, 17, 11, 0);
            const uint32_t SWL = enc_i(0x2A, 17, 11, 0), SWR = enc_i(0x2E, 17, 11, 0);
            psx_pgxp_load(nullptr, LWL, TBL + 3, w0);
            psx_pgxp_load(nullptr, LWR, TBL + 0, w0);
            psx_pgxp_store(nullptr, SWL, TBL + 0x103, w0);
            psx_pgxp_store(nullptr, SWR, TBL + 0x100, w0);
            int32_t cx, cy; uint16_t cz;
            CHECK(lookup(TBL + 0x100, w0, 100, 50, &cx, &cy, &cz) == PGXP_SRC_DATAFLOW);
            CHECK(cx == ((100 << 16) | 0xE666) && cy == ((50 << 16) | 0x8000));
            /* a genuinely unaligned piece still drops the word */
            psx_pgxp_store(nullptr, enc_i(0x2A, 17, 11, 0), TBL + 0x101, w0);
            CHECK(lookup(TBL + 0x100, w0, 100, 50, &cx, &cy, &cz) != PGXP_SRC_DATAFLOW);
        }

        /* a MAC1 that overflowed (FLAG bit 27, negative) carries nothing;
         * MAC2's flags (bits 29/26) leave MAC1 alone */
        {
            PGXPMvmva o2 = op;
            o2.flag = 1u << 27;
            pgxp_gte_mvmva(&o2);
            psx_pgxp_cop2(nullptr, MFC2(5, 25), 56300u, 0);
            psx_pgxp_muldiv(nullptr, enc_r(5, 2, 0, 0, 0x1A), 300u, 112u, 56300u, 500u);
            psx_pgxp_alu(nullptr, enc_r(0, 0, 5, 0, 0x12), 112u, 112u, 0u);
            psx_pgxp_store(nullptr, SH(17, 5), TBL + 0x68, 112u);
            CHECK(pgxp_debug_shadow(0, TBL + 0x68, &lv, &value, &flags, &sx, &sy, &sz));
            CHECK((flags & 1u) == 0);
            o2.flag = (1u << 29) | (1u << 26);
            pgxp_gte_mvmva(&o2);
            psx_pgxp_cop2(nullptr, MFC2(5, 25), 56300u, 0);
            psx_pgxp_muldiv(nullptr, enc_r(5, 2, 0, 0, 0x1A), 300u, 112u, 56300u, 500u);
            psx_pgxp_alu(nullptr, enc_r(0, 0, 5, 0, 0x12), 112u, 112u, 0u);
            psx_pgxp_store(nullptr, SH(17, 5), TBL + 0x70, 112u);
            CHECK(pgxp_debug_shadow(0, TBL + 0x70, &lv, &value, &flags, &sx, &sy, &sz));
            CHECK((flags & 1u) != 0);
        }

        /* the scalar tier is cpu-mode only */
        pgxp_set_cpu_mode(0);
        psx_pgxp_cop2(nullptr, MFC2(5, 25), 56300u, 0);
        psx_pgxp_store(nullptr, SH(17, 5), TBL + 0x60, 112u);
        CHECK(pgxp_debug_shadow(0, TBL + 0x60, &lv, &value, &flags, &sx, &sy, &sz));
        CHECK((flags & 1u) == 0);
        pgxp_set_tolerance(-1.0f);
    }

    /* --- stats sanity: dataflow hits were counted --- */
    {
        PGXPStats st;
        pgxp_get_stats(&st);
        CHECK(st.lookups > 0);
        CHECK(st.dataflow_hit > 0);
        CHECK(st.native > 0);
        CHECK(st.fallback_hit > 0);
        CHECK(st.value_mismatch > 0);
    }

    if (g_failures) {
        std::fprintf(stderr, "test_pgxp: %d FAILURES\n", g_failures);
        return 1;
    }
    std::printf("test_pgxp: all checks passed\n");
    return 0;
}
