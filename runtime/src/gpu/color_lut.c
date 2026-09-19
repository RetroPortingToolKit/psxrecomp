/* color_lut.c — see color_lut.h. Present-time only.
 *
 * Ported (C) from JRickey/gba-recomp (crates/screen/src/{color,profile,lut}.rs)
 * via the gbarecomp C++ port (src/runtime/color_lut.cpp), © Jrickey,
 * MIT OR Apache-2.0, used with permission. The C re-implementation and the
 * CRT/composite panel models below are ours. See THIRD_PARTY_ATTRIBUTION.md.
 *
 * The CIE colour-science core (xy->XYZ, primaries->matrix, Bradford, sRGB
 * OETF) is the same first-principles math as the GBA LUT; only the panel
 * model is swapped from a handheld LCD to a CRT/composite display, which is
 * the right physical model for a console scanned out to a TV.
 */

#include "color_lut.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── CIE colorimetry (all double; runs only at LUT-build time) ───────── */
typedef struct { double x, y; } Xy;
typedef struct { Xy red, green, blue, white; } Primaries;
typedef struct { double m[3][3]; } Mat3;

/* D65 white = {0.3127, 0.3290}, inlined into the primaries literals below. */
static const Primaries kSrgb =
    {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, {0.3127, 0.3290}};
static const Primaries kDisplayP3 =
    {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}, {0.3127, 0.3290}};

/* SMPTE-C / EBU-ish consumer-CRT phosphor gamut (the "NTSC TV" most PS1
 * games were authored against), and a late near-sRGB Trinitron-class tube. */
static const Primaries kPhosphorSmpteC =
    {{0.630, 0.340}, {0.310, 0.595}, {0.155, 0.070}, {0.3127, 0.3290}};
static const Primaries kPhosphorTrinitron =
    {{0.625, 0.340}, {0.280, 0.595}, {0.155, 0.070}, {0.3127, 0.3290}};

static bool xy_eq(Xy a, Xy b) { return a.x == b.x && a.y == b.y; }

static void mat_apply(const Mat3* a, const double v[3], double out[3]) {
  for (int i = 0; i < 3; ++i)
    out[i] = a->m[i][0] * v[0] + a->m[i][1] * v[1] + a->m[i][2] * v[2];
}

static Mat3 mat_mul(const Mat3* a, const Mat3* b) {
  Mat3 r;
  memset(&r, 0, sizeof(r));
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) r.m[i][j] += a->m[i][k] * b->m[k][j];
  return r;
}

static double mat_cof(const Mat3* a, int rr, int cc) {
  const double (*m)[3] = a->m;
  int r1 = (rr + 1) % 3, r2 = (rr + 2) % 3;
  int c1 = (cc + 1) % 3, c2 = (cc + 2) % 3;
  return m[r1][c1] * m[r2][c2] - m[r1][c2] * m[r2][c1];
}

static Mat3 mat_inverse(const Mat3* a) {
  double det = a->m[0][0] * mat_cof(a, 0, 0) + a->m[0][1] * mat_cof(a, 0, 1) +
               a->m[0][2] * mat_cof(a, 0, 2);
  Mat3 out;
  memset(&out, 0, sizeof(out));
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out.m[i][j] = mat_cof(a, j, i) / det;
  return out;
}

static void xy_to_xyz(Xy c, double out[3]) {
  out[0] = c.x / c.y;
  out[1] = 1.0;
  out[2] = (1.0 - c.x - c.y) / c.y;
}

/* Linear RGB -> CIE XYZ for a set of primaries (white maps to Y=1). */
static Mat3 rgb_to_xyz(const Primaries* p) {
  double r[3], g[3], b[3], w[3];
  xy_to_xyz(p->red, r);
  xy_to_xyz(p->green, g);
  xy_to_xyz(p->blue, b);
  xy_to_xyz(p->white, w);
  Mat3 m = {{{r[0], g[0], b[0]}, {r[1], g[1], b[1]}, {r[2], g[2], b[2]}}};
  Mat3 minv = mat_inverse(&m);
  double s[3];
  mat_apply(&minv, w, s);
  Mat3 out = m;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) out.m[i][j] *= s[j];
  return out;
}

static Mat3 bradford_adaptation(Xy from, Xy to) {
  const Mat3 kBradford = {{{0.8951, 0.2664, -0.1614},
                           {-0.7502, 1.7135, 0.0367},
                           {0.0389, -0.0685, 1.0296}}};
  double f[3], t[3], src[3], dst[3];
  xy_to_xyz(from, f);
  xy_to_xyz(to, t);
  mat_apply(&kBradford, f, src);
  mat_apply(&kBradford, t, dst);
  Mat3 scale = {{{dst[0] / src[0], 0, 0},
                 {0, dst[1] / src[1], 0},
                 {0, 0, dst[2] / src[2]}}};
  Mat3 binv = mat_inverse(&kBradford);
  Mat3 tmp = mat_mul(&binv, &scale);
  return mat_mul(&tmp, &kBradford);
}

static Mat3 rgb_to_rgb(const Primaries* src, const Primaries* dst) {
  Mat3 to_xyz = rgb_to_xyz(src);
  Mat3 dst_xyz = rgb_to_xyz(dst);
  Mat3 from_xyz = mat_inverse(&dst_xyz);
  if (xy_eq(src->white, dst->white)) return mat_mul(&from_xyz, &to_xyz);
  Mat3 adapt = bradford_adaptation(src->white, dst->white);
  Mat3 tmp = mat_mul(&from_xyz, &adapt);
  return mat_mul(&tmp, &to_xyz);
}

static double srgb_oetf(double v) {
  return v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

/* ── CRT optical model ──────────────────────────────────────────────── */
typedef struct {
  Primaries primaries;
  double gamma, luminance, black_floor;
} PanelModel;

static double clampd(double v, double lo, double hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

static double effective_gamma(double darken) {
  darken = clampd(darken, 0.0, 1.0);
  /* CRTs sit near 2.4–2.5; composite viewing in a lit room reads darker. */
  return 2.4 + 0.4 * darken;
}

static double default_darken(ScreenKind s) {
  switch (s) {
    case SCREEN_COMPOSITE: return 0.25;
    default:               return 0.0;
  }
}

/* Returns false for SCREEN_RAW (handled separately). */
static bool panel_model(ScreenKind s, double darken, PanelModel* out) {
  switch (s) {
    case SCREEN_CRT:
      out->primaries = kPhosphorSmpteC;
      out->gamma = effective_gamma(darken);
      out->luminance = 0.92;
      out->black_floor = 0.004;  /* small tube black lift */
      return true;
    case SCREEN_COMPOSITE:
      out->primaries = kPhosphorSmpteC;
      out->gamma = effective_gamma(darken);
      out->luminance = 0.90;
      out->black_floor = 0.012;  /* composite/IRE pedestal-ish lift */
      return true;
    case SCREEN_TRINITRON:
      out->primaries = kPhosphorTrinitron;
      out->gamma = 2.35;
      out->luminance = 0.95;
      out->black_floor = 0.002;
      return true;
    default:
      return false;
  }
}

static uint8_t quantize(double v) {
  v = clampd(v, 0.0, 1.0);
  return (uint8_t)(v * 255.0 + 0.5);
}

static const Primaries* target_primaries(DisplayTarget t) {
  return t == DISPLAY_P3 ? &kDisplayP3 : &kSrgb;
}

/* ── Public LUT ─────────────────────────────────────────────────────── */
struct ColorLut {
  uint8_t table[32768][3];  /* table[bgr555] = {R,G,B} */
  bool passthrough;
};

bool screen_kind_from_name(const char* name, ScreenKind* out) {
  if (!name || !out) return false;
  if (!strcmp(name, "raw"))       { *out = SCREEN_RAW;       return true; }
  if (!strcmp(name, "crt"))       { *out = SCREEN_CRT;       return true; }
  if (!strcmp(name, "composite")) { *out = SCREEN_COMPOSITE; return true; }
  if (!strcmp(name, "trinitron")) { *out = SCREEN_TRINITRON; return true; }
  return false;
}

ColorLut* color_lut_create(const ColorSettings* settings) {
  ColorLut* lut = (ColorLut*)calloc(1, sizeof(ColorLut));
  if (!lut) return NULL;

  ColorSettings s;
  if (settings) {
    s = *settings;
  } else {
    s.screen = SCREEN_RAW;
    s.darken = -1.0;
    s.target = DISPLAY_SRGB;
  }

  if (s.screen == SCREEN_RAW) {
    lut->passthrough = true;
    for (int px = 0; px < 32768; ++px) {
      uint8_t r = px & 31, g = (px >> 5) & 31, b = (px >> 10) & 31;
      lut->table[px][0] = (uint8_t)(r << 3 | r >> 2);
      lut->table[px][1] = (uint8_t)(g << 3 | g >> 2);
      lut->table[px][2] = (uint8_t)(b << 3 | b >> 2);
    }
    return lut;
  }

  double darken = s.darken < 0.0 ? default_darken(s.screen) : s.darken;
  PanelModel model;
  memset(&model, 0, sizeof(model));
  panel_model(s.screen, darken, &model);
  Mat3 to_display = rgb_to_rgb(&model.primaries, target_primaries(s.target));

  for (int px = 0; px < 32768; ++px) {
    double c[3] = {(px & 31) / 31.0, ((px >> 5) & 31) / 31.0,
                   ((px >> 10) & 31) / 31.0};
    double lin[3];
    for (int i = 0; i < 3; ++i) {
      double v = pow(c[i], model.gamma) * model.luminance;
      lin[i] = v > 1.0 ? 1.0 : v;
    }
    double out[3];
    mat_apply(&to_display, lin, out);
    for (int i = 0; i < 3; ++i) {
      double v = clampd(out[i], 0.0, 1.0);
      double lifted = model.black_floor + (1.0 - model.black_floor) * v;
      out[i] = srgb_oetf(lifted);
    }
    lut->table[px][0] = quantize(out[0]);
    lut->table[px][1] = quantize(out[1]);
    lut->table[px][2] = quantize(out[2]);
  }
  return lut;
}

void color_lut_destroy(ColorLut* lut) { free(lut); }

bool color_lut_is_passthrough(const ColorLut* lut) {
  return lut && lut->passthrough;
}

void color_lut_map555(const ColorLut* lut, uint16_t bgr555,
                      uint8_t* r, uint8_t* g, uint8_t* b) {
  uint32_t idx = bgr555 & 0x7FFFu;
  *r = lut->table[idx][0];
  *g = lut->table[idx][1];
  *b = lut->table[idx][2];
}
