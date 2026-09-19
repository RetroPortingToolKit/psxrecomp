/* spu_shadow.c — see spu_shadow.h.
 *
 * Float, better-than-hardware re-render of the SPU voice mix, verified against
 * the canon spu.c output via ShadowVerifier and substituted only while proven.
 *
 * The enhancement vs the canon model:
 *   - 4-point cubic (Catmull-Rom) interpolation between decoded samples instead
 *     of the canon's nearest-sample pick (the PS1 hardware itself uses a 4-tap
 *     Gaussian that deliberately muffles highs; the canon model approximates
 *     that as nearest-sample). The cubic keeps high frequencies clean.
 *   - everything stays in float through the per-voice envelope+volume scaling
 *     and the mix, with no int16 truncation until the final clamp — no
 *     intermediate requantization noise.
 *
 * ShadowVerifier attribution: see audio_shadow.h. */

#include "spu_shadow.h"
#include "spu.h"
#include "audio_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same normalization the SNES/GBA shadows use: map int16 full-scale to ~1.0. */
#define SHADOW_NORM (1.0f / 32768.0f)

static int            s_enabled = -1;   /* -1 = not yet resolved */
static int            s_cfg     = 0;    /* config/launcher gate (env overrides) */
static ShadowVerifier s_verifier;
static SpuShadowInfo  s_info;

void spu_shadow_set_enabled(int on) {
    s_cfg     = on ? 1 : 0;
    s_enabled = -1;  /* force re-resolution against env + new config */
}

bool spu_shadow_enabled(void) {
    if (s_enabled < 0) {
        /* Env (debug override) wins when present; else the config value. */
        const char* e = getenv("PSX_AUDIO_SHADOW");
        if (e) {
            s_enabled = (e[0] == '1' || e[0] == 'y' || e[0] == 'Y' ||
                         e[0] == 't' || e[0] == 'T' || e[0] == 'o' ||
                         e[0] == 'O') ? 1 : 0;
        } else {
            s_enabled = s_cfg ? 1 : 0;
        }
    }
    return s_enabled != 0;
}

void spu_shadow_reset(void) {
    shadow_verifier_init(&s_verifier);
    memset(&s_info, 0, sizeof(s_info));
    s_info.enabled = spu_shadow_enabled() ? 1 : 0;
    s_info.gain = 1.0f;
}

/* Catmull-Rom cubic through s1,s2 with neighbours s0,s3 at fraction t. */
static float catmull_rom(const int16_t s[4], float t) {
    float s0 = (float)s[0], s1 = (float)s[1], s2 = (float)s[2], s3 = (float)s[3];
    float a0 = -0.5f * s0 + 1.5f * s1 - 1.5f * s2 + 0.5f * s3;
    float a1 = s0 - 2.5f * s1 + 2.0f * s2 - 0.5f * s3;
    float a2 = -0.5f * s0 + 0.5f * s2;
    float a3 = s1;
    return ((a0 * t + a1) * t + a2) * t + a3;
}

void spu_shadow_process(int16_t* canon, int frames) {
    if (!spu_shadow_enabled() || !canon || frames <= 0) return;

    const SpuShadowFrameTapPub* tap =
        (const SpuShadowFrameTapPub*)spu_shadow_tap_buffer();
    int tap_n = spu_shadow_tap_count();
    if (!tap || tap_n <= 0) return;
    if (tap_n > frames) tap_n = frames;

    /* Render the float mix into a scratch buffer first; substitute only after
     * the verifier (fed the whole block) is still proven. Cap matches the
     * tap buffer cap in spu.c (2048). */
    static float shadow_l[2048];
    static float shadow_r[2048];

    const float gain = shadow_verifier_gain(&s_verifier);

    for (int f = 0; f < tap_n; ++f) {
        const SpuShadowFrameTapPub* fr = &tap[f];
        float ml = 0.0f, mr = 0.0f;
        if (fr->enabled) {
            for (int v = 0; v < SPU_SHADOW_MAX_VOICES; ++v) {
                const SpuShadowVoiceTapPub* t = &fr->voice[v];
                if (!t->active) continue;
                /* Cubic-interpolated decoded sample (float), then the canon's
                 * own envelope + per-voice volume, kept in float. */
                float s = catmull_rom(t->s, t->frac);
                float env = (float)t->env * (1.0f / 32768.0f);
                float sv = s * env;
                ml += sv * (float)t->vol_l * (1.0f / 16384.0f);
                mr += sv * (float)t->vol_r * (1.0f / 16384.0f);
            }
            ml *= (float)fr->main_l * (1.0f / 16384.0f);
            mr *= (float)fr->main_r * (1.0f / 16384.0f);
        }
        shadow_l[f] = ml * gain;
        shadow_r[f] = mr * gain;
    }

    /* Feed (canon, shadow) to the verifier per output frame, in the canon's
     * normalized domain. The verifier owns prove/strike/pause + auto-gain. */
    bool was_proven = shadow_verifier_proven(&s_verifier);
    for (int f = 0; f < tap_n; ++f) {
        float cl = (float)canon[f * 2 + 0] * SHADOW_NORM;
        float cr = (float)canon[f * 2 + 1] * SHADOW_NORM;
        shadow_verifier_judge(&s_verifier, cl, cr, shadow_l[f], shadow_r[f]);
    }

    s_info.enabled    = 1;
    s_info.proven     = shadow_verifier_proven(&s_verifier) ? 1 : 0;
    s_info.pauses     = s_verifier.pauses;
    s_info.last_r     = s_verifier.last_r;
    s_info.last_ratio = s_verifier.last_ratio;
    s_info.gain       = shadow_verifier_gain(&s_verifier);

    /* Revert LOUD: if we were proven and just paused, log DEGRADED once. */
    if (was_proven && !s_info.proven && s_verifier.reverted[0]) {
        snprintf(s_info.last_revert, sizeof(s_info.last_revert), "%s",
                 s_verifier.reverted);
        fprintf(stderr, "[spu_shadow] DEGRADED: reverting to canon SPU mix — %s\n",
                s_verifier.reverted);
        s_verifier.reverted[0] = '\0';
    }

    /* Substitute the float mix ONLY while proven. Otherwise leave the canon
     * output untouched (it is the authoritative output). */
    if (!s_info.proven) return;

    for (int f = 0; f < tap_n; ++f) {
        float l = shadow_l[f] * 32768.0f;
        float r = shadow_r[f] * 32768.0f;
        if (l > 32767.0f) l = 32767.0f; else if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f; else if (r < -32768.0f) r = -32768.0f;
        canon[f * 2 + 0] = (int16_t)l;
        canon[f * 2 + 1] = (int16_t)r;
    }
}

void spu_shadow_get_info(SpuShadowInfo* out) {
    if (!out) return;
    *out = s_info;
}
