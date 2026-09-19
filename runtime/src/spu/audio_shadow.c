/* audio_shadow.c — see audio_shadow.h.
 *
 * Ported (C) from JRickey/gba-recomp crates/gba-core/src/shadow.rs via the
 * gbarecomp C++ port and the snesrecomp C port, © Jrickey, MIT OR Apache-2.0,
 * used with permission. The algorithm is engine-agnostic and unchanged from
 * the SNES/GBA ports; only the include guard / namespace is PSX-local.
 */

#include "audio_shadow.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* One decimated envelope entry per 64 grid samples (~1 ms); 1 s window. */
static const float kEnvAlpha = 0.0075f;     /* ~80 Hz follower at the grid rate */
static const double kMinLevel = 0.002;      /* below this the window has no verdict */
static const float kMinR = 0.5f;            /* sustained correlation gate */
static const uint32_t kMaxStrikes = 3u;

void shadow_verifier_init(ShadowVerifier* v) {
  memset(v, 0, sizeof(*v));
  v->gain = 1.0f;
  v->prove_need = 1u;
}

/* Returns true at window ends that produced a verdict, filling best_r/ratio. */
static bool selfcheck_push(ShadowSelfCheck* s, float canon_l, float canon_r,
                           float shadow_l, float shadow_r, float* best_r,
                           float* level_ratio) {
  s->lp_x[0] += kEnvAlpha * (fabsf(canon_l) - s->lp_x[0]);
  s->lp_x[1] += kEnvAlpha * (fabsf(canon_r) - s->lp_x[1]);
  s->lp_y[0] += kEnvAlpha * (fabsf(shadow_l) - s->lp_y[0]);
  s->lp_y[1] += kEnvAlpha * (fabsf(shadow_r) - s->lp_y[1]);
  if (++s->phase < SHADOW_DECIM) return false;
  s->phase = 0;
  s->ex[s->count][0] = s->lp_x[0];
  s->ex[s->count][1] = s->lp_x[1];
  s->ey[s->count][0] = s->lp_y[0];
  s->ey[s->count][1] = s->lp_y[1];
  if (++s->count < SHADOW_WINDOW) return false;

  const uint32_t n = SHADOW_WINDOW - SHADOW_MAX_LAG;
  double m0 = 0.0, m1 = 0.0, my0 = 0.0, my1 = 0.0;
  for (uint32_t i = 0; i < SHADOW_WINDOW; ++i) {
    m0 += s->ex[i][0];  m1 += s->ex[i][1];
    my0 += s->ey[i][0]; my1 += s->ey[i][1];
  }
  m0 /= SHADOW_WINDOW; m1 /= SHADOW_WINDOW;
  my0 /= SHADOW_WINDOW; my1 /= SHADOW_WINDOW;
  double canon_level = (m0 + m1) / 2.0;
  double shadow_level = (my0 + my1) / 2.0;

  bool have_verdict = false;
  if (canon_level >= kMinLevel) {
    float ratio = (float)(shadow_level / canon_level);
    /* Does the canon envelope carry structure this window (vs a held level)? */
    double canon_var = 0.0;
    for (uint32_t i = 0; i < SHADOW_WINDOW; ++i) {
      double a = s->ex[i][0] - m0, b = s->ex[i][1] - m1;
      canon_var += a * a + b * b;
    }
    canon_var /= SHADOW_WINDOW;

    float best = 0.0f;
    if (canon_var <= 1e-10) {
      best = 1.0f;  /* flat canon: nothing to correlate — ratio is the verdict */
    } else {
      bool any = false;
      for (uint32_t lag = 0; lag <= SHADOW_MAX_LAG; lag += 2) {
        double sum = 0.0;
        int sides = 0;
        for (int side = 0; side < 2; ++side) {
          double mx = 0.0, myy = 0.0;
          for (uint32_t i = 0; i < n; ++i) {
            mx += s->ex[SHADOW_MAX_LAG + i][side];
            myy += s->ey[SHADOW_MAX_LAG - lag + i][side];
          }
          mx /= n; myy /= n;
          double cov = 0.0, vx = 0.0, vy = 0.0;
          for (uint32_t i = 0; i < n; ++i) {
            double a = s->ex[SHADOW_MAX_LAG + i][side] - mx;
            double b = s->ey[SHADOW_MAX_LAG - lag + i][side] - myy;
            cov += a * b; vx += a * a; vy += b * b;
          }
          if (vx > 1e-12 && vy > 1e-12) {
            sum += cov / sqrt(vx * vy);
            ++sides;
          }
        }
        if (sides > 0) {
          float r = (float)(sum / sides);
          if (!any || r > best) best = r;
          any = true;
        }
      }
      if (!any) best = 0.0f;
    }
    *best_r = best;
    *level_ratio = ratio;
    have_verdict = true;
  }
  s->count = 0;
  return have_verdict;
}

static void verifier_pause(ShadowVerifier* v, const char* reason) {
  v->proven = false;
  v->consec_pass = 0;
  v->check.strikes = 0;
  v->prove_need = (v->prove_need * 2u < 16u) ? v->prove_need * 2u : 16u;
  ++v->pauses;
  snprintf(v->reverted, sizeof(v->reverted), "%s", reason);
}

ShadowJudgement shadow_verifier_judge(ShadowVerifier* v, float canon_l,
                                      float canon_r, float chk_l, float chk_r) {
  float r = 0.0f, ratio = 0.0f;
  if (!selfcheck_push(&v->check, canon_l, canon_r, chk_l, chk_r, &r, &ratio)) {
    return SHADOW_JUDGE_NONE;
  }
  v->last_r = r;
  v->last_ratio = ratio;

  /* Probation auto-calibration: strong structure with a stable off-band level
   * means the shadow scales by a constant — adopt it instead of striking. */
  if (!v->calibrated && r >= 0.7f && !(ratio >= 0.85f && ratio <= 1.15f) &&
      (ratio >= 0.2f && ratio <= 5.0f) && v->ratio_n < 6u) {
    v->ratio_hist[v->ratio_n % 3u] = ratio;
    ++v->ratio_n;
    if (v->ratio_n >= 3u) {
      float m = (v->ratio_hist[0] + v->ratio_hist[1] + v->ratio_hist[2]) / 3.0f;
      bool stable = true;
      for (int i = 0; i < 3; ++i) {
        if (fabsf(v->ratio_hist[i] / m - 1.0f) >= 0.1f) stable = false;
      }
      if (stable) {
        float g = v->gain / m;
        v->gain = g < 0.25f ? 0.25f : (g > 4.0f ? 4.0f : g);
        v->calibrated = true;
        v->check.strikes = 0;
      }
    }
    return SHADOW_JUDGE_NONE;
  }

  bool level_ok = (ratio >= 0.55f && ratio <= 1.6f);
  bool window_ok = (r >= kMinR && level_ok);
  if (v->proven) {
    if (window_ok) {
      if (v->check.strikes) --v->check.strikes;
    } else {
      ++v->check.strikes;
      if (v->check.strikes >= kMaxStrikes) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "shadow/canon correlation %.2f, level ratio %.2f "
                 "(driver variant or unsupported feature)", (double)r,
                 (double)ratio);
        verifier_pause(v, buf);
      }
    }
  } else if (window_ok) {
    ++v->consec_pass;
    if (v->consec_pass >= v->prove_need) {
      v->proven = true;
      v->check.strikes = 0;
    }
  } else {
    v->consec_pass = 0;
  }
  v->fail_structural = !window_ok && (r < kMinR && level_ok);
  return window_ok ? SHADOW_JUDGE_PASS : SHADOW_JUDGE_FAIL;
}
