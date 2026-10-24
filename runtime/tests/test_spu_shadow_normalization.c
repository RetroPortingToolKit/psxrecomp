/* Synthetic voice taps only; exercises the real shadow mixer and verifier. */
#include "spu.h"
#include "spu_shadow.h"
#undef NDEBUG
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static SpuShadowFrameTapPub taps[2048];
static uint16_t ctrl;
void spu_get_global_state(SpuGlobalState* out) { memset(out,0,sizeof(*out));out->ctrl=ctrl; }
const void *spu_shadow_tap_buffer(void) { return taps; }
int spu_shadow_tap_count(void) { return 2048; }

static void feed(unsigned block, int mismatch, int check_output) {
    int16_t canon[4096], reference[4096];
    memset(taps, 0, sizeof(taps));
    for (unsigned i = 0; i < 2048; ++i) {
        int16_t sample = (int16_t)(12000.0 * sin((block * 2048.0 + i) * .0003));
        SpuShadowVoiceTapPub *voice = &taps[i].voice[0];
        for (int j = 0; j < 4; ++j) voice->s[j] = sample;
        voice->env = 32767; voice->active = 1;
        voice->vol_l = voice->vol_r = 16384;
        taps[i].main_l = taps[i].main_r = 16384; taps[i].enabled = 1;
        canon[2*i] = canon[2*i+1] = (int16_t)((float)sample * (32767.0f / 32768.0f) /
                                           (mismatch ? 32.0f : 1.0f));
    }
    memcpy(reference, canon, sizeof(canon));
    spu_shadow_process(canon, 2048);
    if (check_output == 2) assert(memcmp(canon, reference, sizeof(canon)) == 0);
    else if (check_output)
        for (unsigned i = 0; i < 4096; ++i) assert(abs(canon[i] - reference[i]) <= 1);
}

int main(void) {
#ifdef _WIN32
    _putenv_s("PSX_AUDIO_SHADOW", "");
#else
    unsetenv("PSX_AUDIO_SHADOW");
#endif
    spu_shadow_set_enabled(0); spu_shadow_reset();
    feed(0, 0, 2);  /* Disabled path preserves canonical audio byte-for-byte. */
    spu_shadow_set_enabled(1); spu_shadow_reset();
    for (unsigned block = 0; block < 48; ++block) feed(block, 0, 1);
    SpuShadowInfo info;
    spu_shadow_get_info(&info);
    assert(info.enabled && info.proven);
    assert(info.last_ratio > .99f && info.last_ratio < 1.01f);
    assert(info.last_r > .99f);
    /* A proven voice-only shadow must not replace newly starting XA/CD.
     * Synthetic CD samples, no original audio/game state. */
    ctrl=1;
    for(unsigned block=0;block<8;++block) {
        int16_t canon[4096], reference[4096];
        memset(taps,0,sizeof(taps));
        for(unsigned i=0;i<4096;++i)canon[i]=(int16_t)(4000+(i%511));
        memcpy(reference,canon,sizeof(canon));
        spu_shadow_process(canon,2048);
        assert(memcmp(canon,reference,sizeof(canon))==0);
        spu_shadow_get_info(&info);assert(!info.proven);
    }
    ctrl=0;
    feed(48,0,2); /* CD exit cannot reuse the pre-CD proof. */
    for(unsigned block=49;block<97;++block)feed(block,0,1);
    spu_shadow_get_info(&info);assert(info.proven);
    for (unsigned block = 97; block < 289; ++block) feed(block, 1, 0);
    spu_shadow_get_info(&info);
    assert(!info.proven && info.pauses > 0); /* Real guard still rejects mismatch. */
    spu_shadow_set_enabled(0); spu_shadow_reset();
    feed(240, 1, 2);
    puts("SPU shadow: normalized proof, output level, immediate CD passthrough, reproof, mismatch fallback and off passed");
    return 0;
}
