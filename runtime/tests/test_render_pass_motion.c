/* render_pass_motion.c and render_pass_frame.c against a mock guest RAM and
 * mock render-pass / mod APIs. */
#include "mod_plugins.h"
#include "render_pass_motion.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond, msg) do { if (!(cond)) { failures++; printf("FAIL: %s\n", msg); } } while (0)

/* ---- mock guest RAM ------------------------------------------------------ */
static uint8_t s_ram[0x10000];
static uint32_t off(uint32_t a) { return a & 0xFFFFu; }
uint8_t psx_mod_read_byte(uint32_t a) { return s_ram[off(a)]; }
uint16_t psx_mod_read_half(uint32_t a) { uint16_t v; memcpy(&v, s_ram + off(a), 2); return v; }
uint32_t psx_mod_read_word(uint32_t a) { uint32_t v; memcpy(&v, s_ram + off(a), 4); return v; }
void psx_mod_write_byte(uint32_t a, uint8_t v) { s_ram[off(a)] = v; }
void psx_mod_write_half(uint32_t a, uint16_t v) { memcpy(s_ram + off(a), &v, 2); }
void psx_mod_write_word(uint32_t a, uint32_t v) { memcpy(s_ram + off(a), &v, 4); }

/* ---- mock mod / pass APIs (render_pass_frame.c) --------------------------- */
static char s_option[16] = "display";
static uint32_t s_fps = 99, s_source = 99, s_blend = 99, s_flip = 99, s_blend_calls;
static uint32_t s_status, s_phases, s_period, s_shown, s_passes_run;
int psx_mod_option_value(const char* p, const char* f, const char* o, char* out, uint32_t n) {
    (void)p; (void)f; (void)o;
    snprintf(out, n, "%s", s_option);
    return 1;
}
int psx_mod_set_frame_interpolation(uint32_t fps) { s_fps = fps; return 1; }
int psx_mod_set_frame_interpolation_source(uint32_t s) { s_source = s; return 1; }
int psx_mod_set_frame_interpolation_blend(uint32_t b) { s_blend = b; s_blend_calls++; return 1; }
int psx_mod_set_render_pass_flip(uint32_t m) { s_flip = m; return 1; }
uint32_t psx_mod_render_pass_status(void) { return s_status; }
uint32_t psx_mod_render_pass_plan(uint32_t period, uint32_t shown, uint32_t* a, uint32_t max) {
    s_period = period; s_shown = shown;
    if (s_status != PSX_MOD_RENDER_PASS_READY) return 0;
    for (uint32_t i = 0; i < s_phases && i < max; i++) a[i] = (i + 1) * 65536u / (s_phases + 1);
    return s_phases < max ? s_phases : max;
}
int psx_mod_render_pass(struct CPUState* cpu, const PSXModRenderPass* p,
                        PSXModRenderPassFn fn, void* user) {
    s_passes_run++;
    return fn(cpu, user, p->alpha_q16);
}

/* ---- helpers --------------------------------------------------------------- */
static void put_matrix(uint32_t a, double yaw, int32_t x, int32_t y, int32_t z) {
    PSXMotionMatrix m;
    const double c = cos(yaw), s = sin(yaw);
    const double r[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
    for (int i = 0; i < 9; i++) m.r[i] = (int16_t)lround(r[i] * 4096.0);
    m.t[0] = x; m.t[1] = y; m.t[2] = z;
    psx_motion_write_matrix(a, &m, 1);
}
static double yaw_of(uint32_t a) {
    PSXMotionMatrix m;
    psx_motion_read_matrix(a, &m);
    return atan2(m.r[2] / 4096.0, m.r[0] / 4096.0);
}

static void test_matrix_slerp(void) {
    PSXMotionSet* s = psx_motion_set_create(16);
    PSXMotionLimits lim = {1000.0, 0};
    PSXMotionStats st;
    PSXMotionMatrix m;
    put_matrix(0x100, 0.0, 0, 0, 0);
    psx_motion_begin(s, 10);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x100, 7);
    CHECK(psx_motion_prepare(s, &lim, &st) == 0 && st.ticks == 0, "first frame has no history");
    put_matrix(0x100, 1.2, 800, 0, -400);
    psx_motion_begin(s, 12);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x100, 7);
    CHECK(psx_motion_prepare(s, &lim, &st) == 1 && st.ticks == 2 && st.blended == 1,
          "the same object two ticks later blends");
    psx_motion_apply(s, 0.5);
    psx_motion_read_matrix(0x100, &m);
    CHECK(fabs(yaw_of(0x100) - 0.6) < 0.002, "rotation slerps to the half-way angle");
    CHECK(m.t[0] == 400 && m.t[2] == -200, "translation blends linearly");
    {
        double len = 0;
        for (int j = 0; j < 3; j++) len += (double)m.r[j] * m.r[j];
        CHECK(fabs(sqrt(len) - 4096.0) < 3.0, "the blended rotation stays a rotation");
    }
    CHECK(psx_motion_blend_at(s, PSX_MOTION_MATRIX, 0x100, 1.0, &m) == 1 &&
          m.t[0] == 800, "blend_at at t=1 is the current value");
    psx_motion_set_destroy(s);
}

/* A mirrored basis (determinant -1, THPS2's skater) blends as a rotation
 * after one fixed reflection; a change of handedness snaps. */
static void make_yaw(double yaw, int mirrored, PSXMotionMatrix* m) {
    const double c = cos(yaw), s = sin(yaw);
    const double r[9] = {c, 0, s, 0, mirrored ? -1.0 : 1.0, 0, -s, 0, c};
    memset(m, 0, sizeof *m);
    for (int i = 0; i < 9; i++) m->r[i] = (int16_t)lround(r[i] * 4096.0);
}
static double det_of(const PSXMotionMatrix* m) {
    const int16_t* r = m->r;
    return ((double)r[0] * ((double)r[4] * r[8] - (double)r[5] * r[7]) -
            (double)r[1] * ((double)r[3] * r[8] - (double)r[5] * r[6]) +
            (double)r[2] * ((double)r[3] * r[7] - (double)r[4] * r[6])) / (4096.0 * 4096.0 * 4096.0);
}
static void test_mirrored_basis(void) {
    PSXMotionMatrix a, b, o;
    make_yaw(0.2, 1, &a);
    make_yaw(1.0, 1, &b);
    CHECK(psx_motion_blend_matrix(&a, &b, 0.5, 0, &o) == 1, "a mirrored pair blends");
    CHECK(det_of(&o) < -0.99, "the blend stays mirrored");
    CHECK(fabs(atan2(o.r[2], o.r[0]) - 0.6) < 0.002, "a mirrored basis turns to the half-way angle");
    make_yaw(1.0, 0, &b);
    CHECK(psx_motion_blend_matrix(&a, &b, 0.5, 0, &o) == 0 &&
          memcmp(o.r, b.r, sizeof o.r) == 0, "a change of handedness snaps to the new basis");
}

static void test_identity_and_limits(void) {
    PSXMotionSet* s = psx_motion_set_create(16);
    PSXMotionLimits lim = {1000.0, 1.0};
    PSXMotionStats st;
    int32_t v[3];
    put_matrix(0x200, 0.0, 0, 0, 0);       /* moves within limits */
    put_matrix(0x300, 0.0, 0, 0, 0);       /* reused by another object */
    put_matrix(0x400, 0.0, 0, 0, 0);       /* teleports */
    put_matrix(0x500, 0.0, 0, 0, 0);       /* spins past the turn limit */
    psx_motion_begin(s, 1);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x200, 1);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x300, 1);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x400, 1);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x500, 1);
    psx_motion_prepare(s, &lim, &st);
    put_matrix(0x200, 0.0, 1500, 0, 0);
    put_matrix(0x300, 0.0, 10, 0, 0);
    put_matrix(0x400, 0.0, 5000, 0, 0);
    put_matrix(0x500, 2.0, 0, 0, 0);
    psx_motion_begin(s, 3);                  /* two ticks: 2000 units allowed */
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x200, 1);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x300, 2);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x400, 1);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x500, 1);
    psx_motion_track(s, PSX_MOTION_VECTOR, 0x600, 1);   /* new this frame */
    CHECK(psx_motion_prepare(s, &lim, &st) == 1, "only the moving object blends");
    CHECK(st.placed == 2 && st.unmatched == 2 && st.tracked == 5,
          "teleport and spin are placed; reuse and new are unmatched");
    psx_motion_apply(s, 0.5);
    CHECK((int32_t)psx_mod_read_word(0x300 + 0x14) == 10, "a reused address keeps its new object");
    CHECK((int32_t)psx_mod_read_word(0x400 + 0x14) == 5000, "a teleport is drawn where it landed");
    CHECK((int32_t)psx_mod_read_word(0x200 + 0x14) == 750, "the mover is half-way");
    CHECK(psx_motion_blend_at(s, PSX_MOTION_VECTOR, 0x600, 0.5, v) == 0, "no history: current value");
    CHECK(psx_motion_blend_at(s, PSX_MOTION_VECTOR, 0x700, 0.5, v) == -1, "untracked: refused");

    /* A gap (a frame not captured) or too many ticks: nothing blends. */
    psx_motion_begin(s, 20);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x200, 1);
    CHECK(psx_motion_prepare(s, &lim, &st) == 0 && st.ticks == 0, "17 ticks apart is not one motion");
    psx_motion_invalidate(s);
    psx_motion_begin(s, 21);
    psx_motion_track(s, PSX_MOTION_MATRIX, 0x200, 1);
    CHECK(psx_motion_prepare(s, &lim, &st) == 0, "after invalidate there is no history");
    psx_motion_set_destroy(s);
}

static void test_angles_vectors_scalars(void) {
    PSXMotionSet* s = psx_motion_set_create(16);
    PSXMotionLimits lim = {100.0, 0};
    PSXMotionStats st;
    psx_mod_write_half(0x800, 4000); psx_mod_write_half(0x802, 100); psx_mod_write_half(0x804, 0);
    psx_mod_write_half(0x810, (uint16_t)-50); psx_mod_write_half(0x812, 0); psx_mod_write_half(0x814, 0);
    psx_mod_write_word(0x820, 1000);
    psx_motion_begin(s, 5);
    psx_motion_track(s, PSX_MOTION_ANGLES, 0x800, 1);
    psx_motion_track(s, PSX_MOTION_SVECTOR, 0x810, 1);
    psx_motion_track(s, PSX_MOTION_SCALAR, 0x820, 1);
    psx_motion_prepare(s, &lim, &st);
    psx_mod_write_half(0x800, 96); psx_mod_write_half(0x802, 300); psx_mod_write_half(0x804, 2048);
    psx_mod_write_half(0x810, 50);
    psx_mod_write_word(0x820, 1100);
    psx_motion_begin(s, 6);
    psx_motion_track(s, PSX_MOTION_ANGLES, 0x800, 1);
    psx_motion_track(s, PSX_MOTION_SVECTOR, 0x810, 1);
    psx_motion_track(s, PSX_MOTION_SCALAR, 0x820, 1);
    CHECK(psx_motion_prepare(s, &lim, &st) == 3, "angles, svector and scalar blend");
    psx_motion_apply(s, 0.5);
    CHECK(psx_mod_read_half(0x800) == 0, "4000 -> 96 crosses 4096 the short way (0)");
    CHECK(psx_mod_read_half(0x802) == 200, "100 -> 300 is 200");
    CHECK(psx_mod_read_half(0x804) == 1024, "0 -> 2048 half turn blends to 1024");
    CHECK((int16_t)psx_mod_read_half(0x810) == 0, "signed svector -50 -> 50 is 0");
    CHECK(psx_mod_read_word(0x820) == 1050, "scalar blends");
    psx_motion_set_destroy(s);
}

static void test_rotation_only(void) {
    PSXMotionSet* s = psx_motion_set_create(4);
    PSXMotionLimits lim = {100.0, 0};
    PSXMotionStats st;
    /* A bare rotation (as in a pose record) with other data right after it. */
    put_matrix(0xA00, 0.0, 0, 0, 0);
    psx_mod_write_word(0xA00 + 0x12, 0xDEADBEEFu);
    psx_motion_begin(s, 1);
    psx_motion_track(s, PSX_MOTION_ROTATION, 0xA00, 3);
    psx_motion_prepare(s, &lim, &st);
    put_matrix(0xA00, 1.0, 0, 0, 0);
    psx_mod_write_word(0xA00 + 0x12, 0xDEADBEEFu);
    psx_motion_begin(s, 2);
    psx_motion_track(s, PSX_MOTION_ROTATION, 0xA00, 3);
    CHECK(psx_motion_prepare(s, &lim, &st) == 1, "a bare rotation blends");
    psx_motion_apply(s, 0.5);
    CHECK(fabs(yaw_of(0xA00) - 0.5) < 0.002, "rotation-only slerps");
    CHECK(psx_mod_read_word(0xA00 + 0x12) == 0xDEADBEEFu, "nothing past the 18 bytes is written");
    psx_motion_set_destroy(s);
}

static int s_fn_calls;
static double s_fn_last;
static int frame_fn(struct CPUState* cpu, void* user, uint32_t alpha) {
    (void)cpu; (void)user;
    s_fn_calls++;
    s_fn_last = alpha / 65536.0;
    return 1;
}

static void test_frame_driver(void) {
    PSXModRenderPassFrame f;
    char cpu_storage[4096];
    struct CPUState* cpu = (struct CPUState*)cpu_storage;
    strcpy(s_option, "165");
    CHECK(psx_mod_activate_render_pass_rate("p", "f", "rate", PSX_MOD_RENDER_PASS_FLIP_PENDING) &&
          s_fps == 165 && s_source == PSX_MOD_FRAME_SOURCE_FLIP &&
          s_blend == PSX_MOD_FRAME_INTERPOLATION_HOLD &&
          s_flip == PSX_MOD_RENDER_PASS_FLIP_PENDING, "activation selects rate, FLIP, HOLD, flip mode");
    strcpy(s_option, "display");
    psx_mod_activate_render_pass_rate("p", "f", "rate", PSX_MOD_RENDER_PASS_FLIP_SHOWN);
    CHECK(s_fps == 0 && s_flip == PSX_MOD_RENDER_PASS_FLIP_SHOWN, "display refresh is rate 0");

    memset(&f, 0, sizeof f);
    f.struct_size = sizeof f;
    f.period_vblanks = 2; f.w = 320; f.h = 240;
    s_status = PSX_MOD_RENDER_PASS_READY;
    s_phases = 3;
    s_blend_calls = 0;
    CHECK(psx_mod_render_pass_frame(cpu, &f, frame_fn, NULL) == 3 && s_fn_calls == 3 &&
          s_period == 2 && s_fn_last > 0.7, "every planned phase runs");
    CHECK(s_blend_calls == 0, "READY keeps HOLD without re-setting it");
    s_status = PSX_MOD_RENDER_PASS_BACKEND;
    CHECK(psx_mod_render_pass_frame(cpu, &f, frame_fn, NULL) == 0 &&
          s_blend == PSX_MOD_FRAME_INTERPOLATION_MOTION_ADAPTIVE, "lasting refusal crossfades");
    s_status = PSX_MOD_RENDER_PASS_SESSION;
    psx_mod_render_pass_frame(cpu, &f, frame_fn, NULL);
    CHECK(s_blend == PSX_MOD_FRAME_INTERPOLATION_HOLD, "a transient reason returns to HOLD");
    f.struct_size = 4;
    CHECK(psx_mod_render_pass_frame(cpu, &f, frame_fn, NULL) == 0, "a short struct is refused");
}

int main(void) {
    test_matrix_slerp();
    test_mirrored_basis();
    test_identity_and_limits();
    test_angles_vectors_scalars();
    test_rotation_only();
    test_frame_driver();
    printf(failures ? "FAILED (%d)\n" : "ALL PASS\n", failures);
    return failures ? 1 : 0;
}
