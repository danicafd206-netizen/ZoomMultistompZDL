/*
 * fahhh.c -- Fahhh: a pick-triggered sampler, MS-70CDR (ZDL).
 *
 * In the spirit of Zoom's stock Bomber: every picked note fires a recording,
 * here the "Fahhh" shout embedded in fahhh_sample.h. With Pitch on Note it is
 * retuned to the note you picked; on Orig it plays at its own pitch. The
 * sample, and only the sample, then runs through its own delay.
 *
 *   in ─┬──────────────────────────────────────────────┬─ (1-Mix) ─┐
 *       │                                              │           + ─ out L/R
 *       ├─ onset (fast env vs slow env, Sens floor) ─ trigger      │
 *       └─ LP ÷4 ─ ring ─ YIN after the pick ─ rate ─┐ │           │
 *                                                    ▼ ▼           │
 *              sample (2 voices, Hermite) ─ LP ─┬─────────── + ─ Mix ┘
 *                                               └─ delay ◄─ fb ┘
 *
 * Retuning is varispeed, like a sampler: a lower note plays the recording
 * slower and longer. The ratio is folded by octaves into 0.5..2 (B2..B4
 * against the recording's B3), so the lowest strings do not drag it out to
 * 7 s and the top frets do not turn it into a chirp.
 *
 * The pitch is not known at the pick: YIN needs ~50 ms of the note after the
 * attack. The recording opens with the unpitched "F", so playback starts at
 * once, at the last note's speed, and glides to the detected one before the
 * vowel arrives. The YIN search is spread over ~30 blocks (5 lags each) so no
 * single 8-sample block carries it.
 *
 * Safe-DSP: no math library (Newton reciprocal, polynomial soft clip), no
 * runtime divide, no switch, no stack arrays, no static writable state.
 * Everything persistent, including the decoded sample and the delay line,
 * lives in the ctx[3] arena and is prepared there in chunks over the first
 * ~130 blocks (~25 ms). ctx[11]/ctx[12] magic shuttle preserved.
 */

#include <stdint.h>

#include "../../airwindows/common/zoom_params.h"
#include "fahhh_params.h"
#include "fahhh_sample.h"

#ifndef FAHHH_AUDIO_FUNC
#define FAHHH_AUDIO_FUNC Fx_SFX_Fahhh
#endif

#define FAHHH_DO_PRAGMA(x) _Pragma(#x)
#define FAHHH_EXPAND_PRAGMA(x) FAHHH_DO_PRAGMA(x)
#define FAHHH_CODE_SECTION(f) FAHHH_EXPAND_PRAGMA(CODE_SECTION(f, ".audio"))

FAHHH_CODE_SECTION(FAHHH_AUDIO_FUNC)

#define ZDL_PTR(type, word) ((type)(uintptr_t)(word))

#define FH_MAGIC   0x46414848u   /* 'FAHH' */
#define FH_VERSION 1u

#define FH_RATE_BASE FAH_RATE_BASE      /* sample frames per output sample at its own pitch */

/* Onset: a fast envelope jumping well above a slow one, over a floor. */
#define FH_ENV_ATK   0.0224f    /* 1 ms */
#define FH_ENV_REL   0.00076f   /* 30 ms */
#define FH_SLOW      0.00015f   /* 150 ms */
#define FH_RATIO     1.8f       /* fast/slow jump that counts as a pick */
#define FH_REARM     1.25f      /* re-arm once the jump has settled */
#define FH_HOLDOFF   3528       /* 80 ms between triggers */

/* Pitch: input low-passed and decimated by 4 (11025 Hz) into a ring, then YIN. */
#define FH_DET_LP    0.133f     /* ~1 kHz, twice */
#define FH_RING      1024u
#define FH_RMASK     1023u
#define FH_SKIP      88u        /* skip 8 ms of pick attack */
#define FH_W         256u       /* 23 ms window */
#define FH_TMIN      11         /* 1002 Hz */
#define FH_TMAX      150        /* 73.5 Hz */
#define FH_LAGS      5          /* lags computed per block */
#define FH_YIN_THR   0.15f
#define FH_YIN_MAXAP 0.45f
#define FH_DEC_RATE  11025.0f

/* Playback. */
#define FH_GLIDE     0.005f     /* rate glide, ~4.5 ms */
#define FH_FADE      0.00227f   /* 10 ms fade of a retriggered voice */
#define FH_OUT_LP    0.88f      /* ~15 kHz: the 32 kHz sample reaches 16 kHz; this only
                                   softens the interpolation images above it */
#define FH_GAIN      0.16f      /* not by ear yet: sets the sample's RMS to ~1.5x the
                                   guitar's on previews/audio/dry_guitar.wav at 25% */

/* Wet delay. */
#define FH_DLEN      65536u
#define FH_DMASK     65535u
#define FH_D_SMOOTH  0.0004f    /* ~57 ms glide of the delay time */
#define FH_FB_MAX    0.92f
#define FH_FB_LP     0.35f
#define FH_ECHO_RAMP 0.002f

/* Arena preparation, per block. */
#define FH_ZERO_CHUNK   2048u
#define FH_DECODE_CHUNK 256u

enum { FH_DET_IDLE = 0, FH_DET_WAIT = 1, FH_DET_RUN = 2 };

typedef struct FahState {
    uint32_t magic;
    uint32_t version;
    uint32_t initialized;
    uint32_t zeroed;       /* delay-line floats cleared so far */
    uint32_t decoded;      /* sample frames decoded so far */
    int32_t  adpcmPred;
    int32_t  adpcmIdx;

    /* onset */
    float envFast;
    float envSlow;
    int32_t holdoff;
    int32_t armed;

    /* pitch detection */
    float lp1;
    float lp2;
    uint32_t decPhase;
    uint32_t dCount;
    uint32_t detStart;
    int32_t detState;
    int32_t detLag;
    float lastRatio;

    /* voices: [0] the newest, [1] the one fading out */
    float pos[2];
    float rate[2];
    float target[2];
    float gain[2];
    int32_t active[2];

    float outLp;

    /* delay */
    uint32_t wpos;
    float dSmooth;
    float fbLp;
    float echo;
} FahState;

#define FH_ALIGN(x) (((x) + 7u) & ~7u)
#define FH_OFF_RING   FH_ALIGN((uint32_t)sizeof(FahState))
#define FH_OFF_YIN    (FH_OFF_RING + FH_RING * 4u)
#define FH_OFF_SAMPLE FH_ALIGN(FH_OFF_YIN + (uint32_t)(FH_TMAX + 2) * 4u)
#define FH_SAMPLE_LEN ((uint32_t)FAH_SAMPLES + 4u)   /* one zero before, three after */
#define FH_OFF_DELAY  FH_ALIGN(FH_OFF_SAMPLE + FH_SAMPLE_LEN * 4u)
#define FH_ARENA_NEED (FH_OFF_DELAY + FH_DLEN * 4u)

static const short FH_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767
};

/* 1/x via a bit-trick seed + Newton steps (avoids __c6xabi_divf). x > 0.
 * Every helper is pinned inline: an outlined one is a CALL out of .audio,
 * which freezes the DSP (see zoom_params.h). */
ZOOM_ALWAYS_INLINE(fh_recip)
static inline float fh_recip(float x)
{
    union { float f; uint32_t u; } v, y;
    v.f = x;
    y.u = 0x7EF311C3u - v.u;
    y.f = y.f * (2.0f - x * y.f);
    y.f = y.f * (2.0f - x * y.f);
    y.f = y.f * (2.0f - x * y.f);
    return y.f;
}

ZOOM_ALWAYS_INLINE(fh_sanitize)
static inline float fh_sanitize(float x)
{
    return (x <= 4.0f && x >= -4.0f) ? x : 0.0f;
}

/* Cubic soft clip: unity slope at 0, flat at +-1.5 -> +-1. */
ZOOM_ALWAYS_INLINE(fh_soft)
static inline float fh_soft(float x)
{
    if (x > 1.5f) x = 1.5f;
    else if (x < -1.5f) x = -1.5f;
    return x - x * x * x * 0.148148f;
}

void FAHHH_AUDIO_FUNC(unsigned int *ctx)
{
    float *params = ZDL_PTR(float *, ctx[1]);
    float *fxBuf  = ZDL_PTR(float *, ctx[5]);

    unsigned int *magicSrc = ZDL_PTR(unsigned int *, ctx[12]);
    unsigned int *magicDst = ZDL_PTR(unsigned int *, *(unsigned int *)ZDL_PTR(unsigned int *, ctx[11]));
    *magicDst = *magicSrc;

    if (params[0] < 0.5f) return;

    volatile unsigned int *desc = ZDL_PTR(volatile unsigned int *, ctx[3]);
    if (!desc) return;
    uintptr_t base = (uintptr_t)desc[0];
    uintptr_t end  = (uintptr_t)desc[1];
    if (base == 0u || end <= base) return;
    if ((base & 7u) != 0u) return;
    if (end - base < (uintptr_t)FH_ARENA_NEED) return;

    FahState *st = (FahState *)base;
    float *ring   = (float *)(base + FH_OFF_RING);
    float *yin    = (float *)(base + FH_OFF_YIN);
    float *smp    = (float *)(base + FH_OFF_SAMPLE);
    float *dline  = (float *)(base + FH_OFF_DELAY);

    if (st->magic != FH_MAGIC || st->version != FH_VERSION || !st->initialized) {
        /* State, ring and YIN table now (~5 KB); the sample is written by the
         * decoder and the delay line is cleared in chunks, so no block carries
         * the whole ~360 KB. */
        uint32_t i;
        uint32_t words = FH_OFF_SAMPLE >> 2;
        uint32_t *w = (uint32_t *)base;
        for (i = 0; i < words; i++) w[i] = 0u;
        smp[0] = 0.0f;
        smp[FAH_SAMPLES + 1] = 0.0f;
        smp[FAH_SAMPLES + 2] = 0.0f;
        smp[FAH_SAMPLES + 3] = 0.0f;
        st->lastRatio = 1.0f;
        st->rate[0] = st->rate[1] = FH_RATE_BASE;
        st->target[0] = st->target[1] = FH_RATE_BASE;
        st->dSmooth = 882.0f;
        st->armed = 1;
        st->magic = FH_MAGIC;
        st->version = FH_VERSION;
        st->initialized = 1u;
    }

    /* ---- Arena preparation: clear the delay line, then decode the sample. ---- */
    if (st->zeroed < FH_DLEN) {
        uint32_t i, stop = st->zeroed + FH_ZERO_CHUNK;
        for (i = st->zeroed; i < stop; i++) dline[i] = 0.0f;
        st->zeroed = stop;
    } else if (st->decoded < (uint32_t)FAH_SAMPLES) {
        uint32_t i = st->decoded;
        uint32_t stop = i + FH_DECODE_CHUNK;
        int32_t pred = st->adpcmPred, idx = st->adpcmIdx;
        if (stop > (uint32_t)FAH_SAMPLES) stop = (uint32_t)FAH_SAMPLES;
        for (; i < stop; i++) {
            int32_t code = (FAH_ADPCM[i >> 1] >> ((i & 1u) << 2)) & 15;
            int32_t step = FH_STEP[idx];
            int32_t delta = step >> 3;
            if (code & 4) delta += step;
            if (code & 2) delta += step >> 1;
            if (code & 1) delta += step >> 2;
            pred += (code & 8) ? -delta : delta;
            if (pred > 32767) pred = 32767;
            else if (pred < -32768) pred = -32768;
            idx += (code & 4) ? (((code & 3) + 1) << 1) : -1;
            if (idx < 0) idx = 0;
            else if (idx > 88) idx = 88;
            smp[i + 1u] = (float)pred * 3.0517578125e-05f;
        }
        st->decoded = stop;
        st->adpcmPred = pred;
        st->adpcmIdx = idx;
    }
    int ready = st->decoded >= (uint32_t)FAH_SAMPLES;

    /* ---- Controls ---- */
    /* A 0..1 selector arrives as value * 0.01, like Stasis's Capture. */
    int noteMode = (int)(zoom_param_norm01(params[FAHHH_PITCH_SLOT], FAHHH_PITCH_DEFAULT_NORM * 0.01f) * 100.0f + 0.5f) >= 1;
    float timeN = zoom_param_norm01(params[FAHHH_TIME_SLOT], FAHHH_TIME_DEFAULT_NORM);
    float fbN   = zoom_param_norm01(params[FAHHH_FDBK_SLOT], FAHHH_FDBK_DEFAULT_NORM);
    float sens  = zoom_param_norm01(params[FAHHH_SENS_SLOT], FAHHH_SENS_DEFAULT_NORM);
    float mix   = zoom_param_norm01(params[FAHHH_MIX_SLOT], FAHHH_MIX_DEFAULT_NORM);

    float ns = 1.0f - sens;
    float thr = 0.002f + 0.06f * ns * ns;          /* -54 dB .. -24 dB; 50 -> -36 dB */
    float echoOn = timeN > 0.005f ? 1.0f : 0.0f;
    float dTarget = (20.0f + 1180.0f * timeN * timeN) * 44.1f;
    float fb = FH_FB_MAX * fbN;
    float wetG = mix;
    float dryG = 1.0f - mix;

    if (!noteMode) {
        st->target[0] = FH_RATE_BASE;
        st->detState = FH_DET_IDLE;
    }

    float envFast = st->envFast, envSlow = st->envSlow;
    int32_t holdoff = st->holdoff, armed = st->armed;
    float lp1 = st->lp1, lp2 = st->lp2;
    uint32_t decPhase = st->decPhase, dCount = st->dCount;
    float outLp = st->outLp;
    uint32_t wpos = st->wpos;
    float dSmooth = st->dSmooth, fbLp = st->fbLp, echo = st->echo;

    int f;
    for (f = 0; f < 8; f++) {
        float inL = fh_sanitize(fxBuf[f]);
        float inR = fh_sanitize(fxBuf[f + 8]);
        float m = 0.5f * (inL + inR);

        /* Onset. */
        float a = m < 0.0f ? -m : m;
        envFast += (a > envFast ? FH_ENV_ATK : FH_ENV_REL) * (a - envFast);
        envSlow += FH_SLOW * (envFast - envSlow);
        if (holdoff > 0) holdoff--;
        if (!armed && envFast < FH_REARM * envSlow) armed = 1;
        if (ready && armed && holdoff == 0 && envFast > thr && envFast > FH_RATIO * envSlow) {
            if (st->active[0]) {
                st->pos[1] = st->pos[0];
                st->rate[1] = st->rate[0];
                st->target[1] = st->rate[0];
                st->gain[1] = st->gain[0];
                st->active[1] = 1;
            }
            st->pos[0] = 0.0f;
            st->rate[0] = (noteMode ? st->lastRatio : 1.0f) * FH_RATE_BASE;
            st->target[0] = st->rate[0];
            st->gain[0] = 1.0f;
            st->active[0] = 1;
            if (noteMode) {
                st->detState = FH_DET_WAIT;
                st->detStart = dCount + FH_SKIP;
            }
            holdoff = FH_HOLDOFF;
            armed = 0;
        }

        /* Pitch-detection feed: ~1 kHz low-pass, every 4th sample into the ring. */
        lp1 += FH_DET_LP * (m - lp1);
        lp2 += FH_DET_LP * (lp1 - lp2);
        if (++decPhase >= 4u) {
            decPhase = 0u;
            ring[dCount & FH_RMASK] = lp2;
            dCount++;
        }

        /* Voices. */
        float v = 0.0f;
        int k;
        for (k = 0; k < 2; k++) {
            if (!st->active[k]) continue;
            float pos = st->pos[k];
            float rate = st->rate[k];
            rate += FH_GLIDE * (st->target[k] - rate);
            int32_t i = (int32_t)pos;
            float t = pos - (float)i;
            float xm1 = smp[i], x0 = smp[i + 1], x1 = smp[i + 2], x2 = smp[i + 3];
            float c1 = 0.5f * (x1 - xm1);
            float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
            float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            float y = ((c3 * t + c2) * t + c1) * t + x0;
            float g = st->gain[k];
            v += y * g;
            pos += rate;
            if (k == 1) {
                g -= FH_FADE;
                if (g <= 0.0f) st->active[1] = 0;
                st->gain[1] = g;
            }
            if (pos >= (float)(FAH_SAMPLES - 1)) st->active[k] = 0;
            st->pos[k] = pos;
            st->rate[k] = rate;
        }
        outLp += FH_OUT_LP * (v - outLp);
        float fah = outLp * FH_GAIN;

        /* Delay on the sample only. */
        dSmooth += FH_D_SMOOTH * (dTarget - dSmooth);
        echo += FH_ECHO_RAMP * (echoOn - echo);
        int32_t di = (int32_t)dSmooth;
        float dfr = dSmooth - (float)di;
        float d0 = dline[(wpos - (uint32_t)di) & FH_DMASK];
        float d1 = dline[(wpos - (uint32_t)di - 1u) & FH_DMASK];
        float delayed = d0 + dfr * (d1 - d0);
        fbLp += FH_FB_LP * (delayed - fbLp);
        dline[wpos] = fh_soft(fah + fb * echo * fbLp);
        wpos = (wpos + 1u) & FH_DMASK;

        float wet = fah + echo * delayed;
        fxBuf[f]     = dryG * inL + wetG * wet;
        fxBuf[f + 8] = dryG * inR + wetG * wet;
    }

    /* ---- YIN, a few lags per block, on the note just picked. ---- */
    if (st->detState == FH_DET_WAIT) {
        if ((int32_t)(dCount - st->detStart) >= (int32_t)(FH_W + FH_TMAX + 1u)) {
            st->detState = FH_DET_RUN;
            st->detLag = 1;
        }
    } else if (st->detState == FH_DET_RUN) {
        int32_t lag = st->detLag;
        int32_t n;
        uint32_t s0 = st->detStart;
        for (n = 0; n < FH_LAGS && lag <= FH_TMAX; n++, lag++) {
            float acc = 0.0f;
            uint32_t j;
            for (j = 0; j < FH_W; j++) {
                float dd = ring[(s0 + j) & FH_RMASK] - ring[(s0 + j + (uint32_t)lag) & FH_RMASK];
                acc += dd * dd;
            }
            yin[lag] = acc;
        }
        st->detLag = lag;
        if (lag > FH_TMAX) {
            /* Cumulative-mean-normalised difference, in place. */
            float cum = 0.0f;
            int32_t t, best = 0;
            for (t = 1; t <= FH_TMAX; t++) {
                cum += yin[t];
                yin[t] = cum > 1e-9f ? yin[t] * (float)t * fh_recip(cum) : 1.0f;
            }
            for (t = FH_TMIN; t <= FH_TMAX; t++) {
                if (yin[t] < FH_YIN_THR) {
                    while (t < FH_TMAX && yin[t + 1] < yin[t]) t++;
                    best = t;
                    break;
                }
            }
            if (!best) {
                float lo = FH_YIN_MAXAP;
                for (t = FH_TMIN; t <= FH_TMAX; t++) {
                    if (yin[t] < lo) { lo = yin[t]; best = t; }
                }
            }
            if (best && cum > 1e-6f) {
                float tau = (float)best;
                if (best > FH_TMIN && best < FH_TMAX) {
                    float ym = yin[best - 1], y0 = yin[best], yp = yin[best + 1];
                    float den = ym - 2.0f * y0 + yp;
                    if (den > 1e-6f) {
                        float sh = 0.5f * (ym - yp) * fh_recip(den);
                        if (sh > 1.0f) sh = 1.0f;
                        else if (sh < -1.0f) sh = -1.0f;
                        tau += sh;
                    }
                }
                float r = FH_DEC_RATE * FAH_REF_INV * fh_recip(tau);
                if (r > 2.0f) r *= 0.5f;
                if (r > 2.0f) r *= 0.5f;
                if (r > 2.0f) r *= 0.5f;
                if (r < 0.5f) r *= 2.0f;
                if (r < 0.5f) r *= 2.0f;
                st->lastRatio = r;
                st->target[0] = r * FH_RATE_BASE;
            }
            st->detState = FH_DET_IDLE;
        }
    }

    st->envFast = envFast;
    st->envSlow = envSlow;
    st->holdoff = holdoff;
    st->armed = armed;
    st->lp1 = lp1;
    st->lp2 = lp2;
    st->decPhase = decPhase;
    st->dCount = dCount;
    st->outLp = outLp;
    st->wpos = wpos;
    st->dSmooth = dSmooth;
    st->fbLp = fbLp;
    st->echo = echo;
}
