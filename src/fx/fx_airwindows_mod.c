// fx_airwindows_mod.c -- Airwindows modulation/delay ports: Ensemble, StereoChorus, StereoDoubler, TapeDelay2.
// Moved verbatim out of fx_chain.c; each effect's run_fx_*() is called by process_fx_chain() via
// fx_effects.h, and its state struct stays private to this file.
#include "fx/fx_effects.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- FX_TYPE_ENSEMBLE: ported from Airwindows StereoEnsemble (MIT license) ---
// A 12-tap fixed-delay ensemble/chorus: 6 delay pairs (A/B, C/D, ... K/L), one tap of each pair
// reading Left and the other reading Right, each pair's two lengths deliberately mismatched
// (e.g. 7481 vs 7523 samples) so the two channels decorrelate into a wide stereo image, summed
// with fixed per-tap gain weights. No feedback loop -- this is NOT a reverb.
//
// Buffer sizes are each exactly (base tap length + 10), matching the original header, giving
// headroom over the largest length any tap can reach (base * ~0.9933, at Depth=1.0 -- see
// delayfactor below).
typedef struct {
    float dA[7491], dB[7533];
    float dC[5789], dD[5747];
    float dE[4841], dF[4871];
    float dG[3119], dH[3089];
    float dI[2213], dJ[2223];
    float dK[1337], dL[1331];

    int oneA, oneB, oneC, oneD, oneE, oneF;
    int oneG, oneH, oneI, oneJ, oneK, oneL;
    int maxdelayA, maxdelayB, maxdelayC, maxdelayD, maxdelayE, maxdelayF;
    int maxdelayG, maxdelayH, maxdelayI, maxdelayJ, maxdelayK, maxdelayL;

    // Downsampling ladder: the original plugin only actually renders a fresh ensemble sample
    // every cycle_end samples (1 at 44.1/48k, up to 4 at 176/192k) to save CPU at high sample
    // rates, interpolating the skipped samples from lastRef*. cycle_end is derived once from
    // SAMPLE_RATE (a compile-time constant here) and never changes again for this slot's life.
    float lastRefL[7], lastRefR[7];
    int cycle;
    int cycle_end;

    uint32_t fpdL, fpdR;
} FxEnsembleState;

void run_fx_ensemble(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxEnsembleState *st = (FxEnsembleState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxEnsembleState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->oneA = st->oneB = st->oneC = st->oneD = st->oneE = st->oneF = 1;
        st->oneG = st->oneH = st->oneI = st->oneJ = st->oneK = st->oneL = 1;
        int cycle_end = SAMPLE_RATE / 44100; if (cycle_end < 1) cycle_end = 1; if (cycle_end > 4) cycle_end = 4;
        st->cycle_end = cycle_end;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    // param[0] = Depth, param[1] = FX Level -- both 0..1 (see the FX_TYPE_ENSEMBLE comment above).
    float depth = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    float outlevel = fmaxf(0.0f, fminf(1.0f, params->param[1]));

    float delayfactor = 0.66f + (depth / 3.0f);
    float dryL = 1.0f - (outlevel * 0.65f);
    float dryR = dryL;

    st->maxdelayA = (int)(7481.0f * delayfactor); st->maxdelayB = (int)(7523.0f * delayfactor);
    st->maxdelayC = (int)(5779.0f * delayfactor); st->maxdelayD = (int)(5737.0f * delayfactor);
    st->maxdelayE = (int)(4831.0f * delayfactor); st->maxdelayF = (int)(4861.0f * delayfactor);
    st->maxdelayG = (int)(3109.0f * delayfactor); st->maxdelayH = (int)(3079.0f * delayfactor);
    st->maxdelayI = (int)(2203.0f * delayfactor); st->maxdelayJ = (int)(2213.0f * delayfactor);
    st->maxdelayK = (int)(1327.0f * delayfactor); st->maxdelayL = (int)(1321.0f * delayfactor);
    if (st->cycle > st->cycle_end - 1) st->cycle = st->cycle_end - 1;

    float inputSampleL = *l, inputSampleR = *r;
    // Anti-denormal: substitute a tiny noise floor instead of an actual all-zero/near-zero
    // sample, so the delay lines never carry a denormal float (which can be very slow on some
    // CPUs without flush-to-zero enabled).
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    st->cycle++;
    if (st->cycle == st->cycle_end) {
        float drySampleL = inputSampleL, drySampleR = inputSampleR;
        float outL = 0.0f, outR = 0.0f, temp = 0.0f;

        st->dA[st->oneA] = inputSampleL; st->oneA--; if (st->oneA < 0 || st->oneA > st->maxdelayA) st->oneA = st->maxdelayA;
        temp = st->dA[st->oneA]; outL += temp * 0.75f;
        st->dB[st->oneB] = inputSampleR; st->oneB--; if (st->oneB < 0 || st->oneB > st->maxdelayB) st->oneB = st->maxdelayB;
        temp = st->dB[st->oneB]; outR += temp * 0.15f;

        st->dC[st->oneC] = inputSampleL; st->oneC--; if (st->oneC < 0 || st->oneC > st->maxdelayC) st->oneC = st->maxdelayC;
        temp = st->dC[st->oneC]; outL += temp * 0.15f;
        st->dD[st->oneD] = inputSampleR; st->oneD--; if (st->oneD < 0 || st->oneD > st->maxdelayD) st->oneD = st->maxdelayD;
        temp = st->dD[st->oneD]; outR += temp * 0.75f;

        st->dE[st->oneE] = inputSampleL; st->oneE--; if (st->oneE < 0 || st->oneE > st->maxdelayE) st->oneE = st->maxdelayE;
        temp = st->dE[st->oneE]; outL += temp * 0.63f;
        st->dF[st->oneF] = inputSampleR; st->oneF--; if (st->oneF < 0 || st->oneF > st->maxdelayF) st->oneF = st->maxdelayF;
        temp = st->dF[st->oneF]; outR += temp * 0.27f;

        st->dG[st->oneG] = inputSampleL; st->oneG--; if (st->oneG < 0 || st->oneG > st->maxdelayG) st->oneG = st->maxdelayG;
        temp = st->dG[st->oneG]; outL += temp * 0.27f;
        st->dH[st->oneH] = inputSampleR; st->oneH--; if (st->oneH < 0 || st->oneH > st->maxdelayH) st->oneH = st->maxdelayH;
        temp = st->dH[st->oneH]; outR += temp * 0.63f;

        st->dI[st->oneI] = inputSampleL; st->oneI--; if (st->oneI < 0 || st->oneI > st->maxdelayI) st->oneI = st->maxdelayI;
        temp = st->dI[st->oneI]; outL += temp * 0.48f;
        st->dJ[st->oneJ] = inputSampleR; st->oneJ--; if (st->oneJ < 0 || st->oneJ > st->maxdelayJ) st->oneJ = st->maxdelayJ;
        temp = st->dJ[st->oneJ]; outR += temp * 0.32f;

        st->dK[st->oneK] = inputSampleL; st->oneK--; if (st->oneK < 0 || st->oneK > st->maxdelayK) st->oneK = st->maxdelayK;
        temp = st->dK[st->oneK]; outL += temp * 0.32f;
        st->dL[st->oneL] = inputSampleR; st->oneL--; if (st->oneL < 0 || st->oneL > st->maxdelayL) st->oneL = st->maxdelayL;
        temp = st->dL[st->oneL]; outR += temp * 0.48f;

        inputSampleL = (outL * outlevel) + (drySampleL * dryL);
        inputSampleR = (outR * outlevel) + (drySampleR * dryR);

        if (st->cycle_end == 4) {
            st->lastRefL[0] = st->lastRefL[4];
            st->lastRefL[2] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[1] = (st->lastRefL[0] + st->lastRefL[2]) * 0.5f;
            st->lastRefL[3] = (st->lastRefL[2] + inputSampleL) * 0.5f;
            st->lastRefL[4] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[4];
            st->lastRefR[2] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[1] = (st->lastRefR[0] + st->lastRefR[2]) * 0.5f;
            st->lastRefR[3] = (st->lastRefR[2] + inputSampleR) * 0.5f;
            st->lastRefR[4] = inputSampleR;
        } else if (st->cycle_end == 3) {
            st->lastRefL[0] = st->lastRefL[3];
            st->lastRefL[2] = (st->lastRefL[0] + st->lastRefL[0] + inputSampleL) / 3.0f;
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL + inputSampleL) / 3.0f;
            st->lastRefL[3] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[3];
            st->lastRefR[2] = (st->lastRefR[0] + st->lastRefR[0] + inputSampleR) / 3.0f;
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR + inputSampleR) / 3.0f;
            st->lastRefR[3] = inputSampleR;
        } else if (st->cycle_end == 2) {
            st->lastRefL[0] = st->lastRefL[2];
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[2] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[2];
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[2] = inputSampleR;
        } else {
            st->lastRefL[0] = inputSampleL;
            st->lastRefR[0] = inputSampleR;
        }
        st->cycle = 0;
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    } else {
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    }

    // 32-bit-float dither, scaled relative to each sample's own magnitude (frexpf's exponent).
    int expon;
    frexpf(inputSampleL, &expon);
    st->fpdL ^= st->fpdL << 13; st->fpdL ^= st->fpdL >> 17; st->fpdL ^= st->fpdL << 5;
    inputSampleL += (((float)st->fpdL - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));
    frexpf(inputSampleR, &expon);
    st->fpdR ^= st->fpdR << 13; st->fpdR ^= st->fpdR >> 17; st->fpdR ^= st->fpdR << 5;
    inputSampleR += (((float)st->fpdR - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));

    *l = inputSampleL;
    *r = inputSampleR;
}

// --- FX_TYPE_STEREOCHORUS: ported from Airwindows StereoChorus (MIT license) ---
// Two independent LFO-swept delay reads (L and R start their sweep phase at different points --
// pi/2.7 vs pi -- so they never modulate in lockstep, widening the stereo image) into a single
// ring buffer quantized to roughly 24-bit integer resolution (the original stores samples as
// `int`, not float/double -- an intentional part of this effect's character, ported as-is), plus
// an "air" comb-filter stage that restores some of the top end a short modulated delay's linear
// interpolation naturally smears away.
typedef struct {
    int pL[65536], pR[65536];
    float sweepL, sweepR;
    int gcount;
    float airPrevL, airEvenL, airOddL, airFactorL;
    float airPrevR, airEvenR, airOddR, airFactorR;
    bool flip;
    float lastRefL[7], lastRefR[7];
    int cycle, cycle_end;
    uint32_t fpdL, fpdR;
} FxStereoChorusState;

void run_fx_stereochorus(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxStereoChorusState *st = (FxStereoChorusState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxStereoChorusState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->sweepL = (float)(M_PI / 2.7);
        st->sweepR = (float)M_PI;
        int cycle_end = SAMPLE_RATE / 44100; if (cycle_end < 1) cycle_end = 1; if (cycle_end > 4) cycle_end = 4;
        st->cycle_end = cycle_end;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }
    if (st->cycle > st->cycle_end - 1) st->cycle = st->cycle_end - 1;

    // param[0] Speed, param[1] Depth -- both 0..1 (see the FX_TYPE_STEREOCHORUS comment above).
    // This exact speed/depth curve is ported straight from the original: speed stays very small
    // across the whole knob range (a tenth-power curve), so depth (which divides by speed)
    // naturally gets large at low Speed settings -- that's intentional/original behavior, not a
    // bug, and stays safely within pL/pR's bounds (see the defensive clamp below regardless).
    float a = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    float b = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    float speed = powf(0.32f + (a / 6.0f), 10.0f);
    float depth = (b / 60.0f) / speed;
    float tupi = (float)(M_PI * 2.0);

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    st->cycle++;
    if (st->cycle == st->cycle_end) {
        st->airFactorL = st->airPrevL - inputSampleL;
        if (st->flip) { st->airEvenL += st->airFactorL; st->airOddL -= st->airFactorL; st->airFactorL = st->airEvenL; }
        else { st->airOddL += st->airFactorL; st->airEvenL -= st->airFactorL; st->airFactorL = st->airOddL; }
        st->airOddL = (st->airOddL - ((st->airOddL - st->airEvenL) / 256.0f)) / 1.0001f;
        st->airEvenL = (st->airEvenL - ((st->airEvenL - st->airOddL) / 256.0f)) / 1.0001f;
        st->airPrevL = inputSampleL;
        inputSampleL += st->airFactorL;

        st->airFactorR = st->airPrevR - inputSampleR;
        if (st->flip) { st->airEvenR += st->airFactorR; st->airOddR -= st->airFactorR; st->airFactorR = st->airEvenR; }
        else { st->airOddR += st->airFactorR; st->airEvenR -= st->airFactorR; st->airFactorR = st->airOddR; }
        st->airOddR = (st->airOddR - ((st->airOddR - st->airEvenR) / 256.0f)) / 1.0001f;
        st->airEvenR = (st->airEvenR - ((st->airEvenR - st->airOddR) / 256.0f)) / 1.0001f;
        st->airPrevR = inputSampleR;
        inputSampleR += st->airFactorR;
        st->flip = !st->flip;

        int tempL = 0, tempR = 0;
        if (st->gcount < 1 || st->gcount > 32760) st->gcount = 32760;

        int count = st->gcount;
        st->pL[count + 32760] = st->pL[count] = (int)(inputSampleL * 8388352.0f);
        float offsetL = depth + (depth * sinf(st->sweepL));
        count += (int)floorf(offsetL);
        if (count < 0) count = 0;
        if (count > 65533) count = 65533; // defensive: keep well inside pL[]/pR[]
        tempL += (int)((float)st->pL[count] * (1.0f - (offsetL - floorf(offsetL))));
        tempL += st->pL[count + 1];
        tempL += (int)((float)st->pL[count + 2] * (offsetL - floorf(offsetL)));
        tempL -= (int)(((float)(st->pL[count] - st->pL[count + 1]) - (float)(st->pL[count + 1] - st->pL[count + 2])) / 50.0f);

        count = st->gcount;
        st->pR[count + 32760] = st->pR[count] = (int)(inputSampleR * 8388352.0f);
        float offsetR = depth + (depth * sinf(st->sweepR));
        count += (int)floorf(offsetR);
        if (count < 0) count = 0;
        if (count > 65533) count = 65533;
        tempR += (int)((float)st->pR[count] * (1.0f - (offsetR - floorf(offsetR))));
        tempR += st->pR[count + 1];
        tempR += (int)((float)st->pR[count + 2] * (offsetR - floorf(offsetR)));
        tempR -= (int)(((float)(st->pR[count] - st->pR[count + 1]) - (float)(st->pR[count + 1] - st->pR[count + 2])) / 50.0f);

        st->sweepL += speed; st->sweepR += speed;
        if (st->sweepL > tupi) st->sweepL -= tupi;
        if (st->sweepR > tupi) st->sweepR -= tupi;
        st->gcount--;

        inputSampleL = (float)tempL / 16776704.0f;
        inputSampleR = (float)tempR / 16776704.0f;

        // Downsampling-ladder reconstruction -- see run_fx_ensemble's identical pattern/comment.
        // At this engine's fixed 48kHz, cycle_end is always 1, so this degenerates to
        // lastRefL[0]=inputSampleL every call; kept generic in case SAMPLE_RATE ever changes.
        if (st->cycle_end == 4) {
            st->lastRefL[0] = st->lastRefL[4];
            st->lastRefL[2] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[1] = (st->lastRefL[0] + st->lastRefL[2]) * 0.5f;
            st->lastRefL[3] = (st->lastRefL[2] + inputSampleL) * 0.5f;
            st->lastRefL[4] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[4];
            st->lastRefR[2] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[1] = (st->lastRefR[0] + st->lastRefR[2]) * 0.5f;
            st->lastRefR[3] = (st->lastRefR[2] + inputSampleR) * 0.5f;
            st->lastRefR[4] = inputSampleR;
        } else if (st->cycle_end == 3) {
            st->lastRefL[0] = st->lastRefL[3];
            st->lastRefL[2] = (st->lastRefL[0] + st->lastRefL[0] + inputSampleL) / 3.0f;
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL + inputSampleL) / 3.0f;
            st->lastRefL[3] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[3];
            st->lastRefR[2] = (st->lastRefR[0] + st->lastRefR[0] + inputSampleR) / 3.0f;
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR + inputSampleR) / 3.0f;
            st->lastRefR[3] = inputSampleR;
        } else if (st->cycle_end == 2) {
            st->lastRefL[0] = st->lastRefL[2];
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[2] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[2];
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[2] = inputSampleR;
        } else {
            st->lastRefL[0] = inputSampleL;
            st->lastRefR[0] = inputSampleR;
        }
        st->cycle = 0;
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    } else {
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    }

    int expon;
    frexpf(inputSampleL, &expon);
    st->fpdL ^= st->fpdL << 13; st->fpdL ^= st->fpdL >> 17; st->fpdL ^= st->fpdL << 5;
    inputSampleL += (((float)st->fpdL - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));
    frexpf(inputSampleR, &expon);
    st->fpdR ^= st->fpdR << 13; st->fpdR ^= st->fpdR >> 17; st->fpdR ^= st->fpdR << 5;
    inputSampleR += (((float)st->fpdR - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));

    *l = inputSampleL;
    *r = inputSampleR;
}

// --- FX_TYPE_STEREODOUBLER: ported from Airwindows StereoDoubler (MIT license) ---
// A granular micro-pitch detuner. 8 simultaneous delay-read "voices" per channel walk through a
// ring buffer at slightly different speeds (L and R detuned in opposite directions from Detune),
// each voice wrapping back to the buffer start at a different moment; whenever one wraps, this
// crossfade-splices to whichever OTHER voice is currently closest in value (`bestsplice`) rather
// than jumping straight back, hiding the seam that would otherwise click. The "air" comb-filter
// stage and downsampling-ladder reconstruction are the same techniques used in
// run_fx_stereochorus above.
typedef struct {
    float pL[5001], pR[5001];
    int gcountL, lastcountL, gcountR, lastcountR;
    float trackingL[8], tempL[8], positionL[8], lastpositionL[8];
    float trackingR[8], tempR[8], positionR[8], lastpositionR[8];
    int activeL, bestspliceL, activeR, bestspliceR;
    float airPrevL, airEvenL, airOddL, airFactorL;
    float airPrevR, airEvenR, airOddR, airFactorR;
    bool flip;
    float lastRefL[7], lastRefR[7];
    int cycle, cycle_end;
    uint32_t fpdL, fpdR;
} FxStereoDoublerState;

// This effect's `width` (the ring buffer span the 8 voices walk around) is a fixed constant in
// the original -- never actually configurable at runtime despite the original tracking a
// "did width change" flag for it -- so its one-time position-ladder setup happens here, at
// state-allocation time, instead of being checked on every call.
#define STEREODOUBLER_WIDTH 2300

void run_fx_stereodoubler(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxStereoDoublerState *st = (FxStereoDoublerState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxStereoDoublerState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        int w = STEREODOUBLER_WIDTH;
        st->positionL[0] = 0; st->positionL[1] = (float)(w / 3); st->positionL[2] = (float)((w / 3) * 2);
        st->positionL[3] = (float)(w / 5); st->positionL[4] = (float)((w / 5) * 2); st->positionL[5] = (float)((w / 5) * 3);
        st->positionL[6] = (float)((w / 5) * 4); st->positionL[7] = (float)(w / 2);
        st->positionR[0] = 0; st->positionR[1] = (float)(w / 3); st->positionR[2] = (float)((w / 3) * 2);
        st->positionR[3] = (float)(w / 5); st->positionR[4] = (float)((w / 5) * 2); st->positionR[5] = (float)((w / 5) * 3);
        st->positionR[6] = (float)((w / 5) * 4); st->positionR[7] = (float)(w / 2);
        int cycle_end = SAMPLE_RATE / 44100; if (cycle_end < 1) cycle_end = 1; if (cycle_end > 4) cycle_end = 4;
        st->cycle_end = cycle_end;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }
    if (st->cycle > st->cycle_end - 1) st->cycle = st->cycle_end - 1;
    int width = STEREODOUBLER_WIDTH;

    // param[0] Detune, param[1] Dry/Wet -- both 0..1 (see the FX_TYPE_STEREODOUBLER comment
    // above for why Dry/Wet stays its own control here, unlike Density3's).
    float a = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    float wet = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    float trim = (a * 2.0f) - 1.0f;
    trim *= fabsf(trim);
    trim /= 40.0f;
    float speedL = trim + 1.0f;
    float speedR = (-trim) + 1.0f;
    if (speedL < 0.0f) speedL = 0.0f;
    if (speedR < 0.0f) speedR = 0.0f;
    float adjust = 1100.0f;

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    st->cycle++;
    if (st->cycle == st->cycle_end) {
        float drySampleL = inputSampleL, drySampleR = inputSampleR;

        st->airFactorL = st->airPrevL - inputSampleL;
        if (st->flip) { st->airEvenL += st->airFactorL; st->airOddL -= st->airFactorL; st->airFactorL = st->airEvenL; }
        else { st->airOddL += st->airFactorL; st->airEvenL -= st->airFactorL; st->airFactorL = st->airOddL; }
        st->airOddL = (st->airOddL - ((st->airOddL - st->airEvenL) / 256.0f)) / 1.0001f;
        st->airEvenL = (st->airEvenL - ((st->airEvenL - st->airOddL) / 256.0f)) / 1.0001f;
        st->airPrevL = inputSampleL;
        inputSampleL += st->airFactorL;

        st->airFactorR = st->airPrevR - inputSampleR;
        if (st->flip) { st->airEvenR += st->airFactorR; st->airOddR -= st->airFactorR; st->airFactorR = st->airEvenR; }
        else { st->airOddR += st->airFactorR; st->airEvenR -= st->airFactorR; st->airFactorR = st->airOddR; }
        st->airOddR = (st->airOddR - ((st->airOddR - st->airEvenR) / 256.0f)) / 1.0001f;
        st->airEvenR = (st->airEvenR - ((st->airEvenR - st->airOddR) / 256.0f)) / 1.0001f;
        st->airPrevR = inputSampleR;
        inputSampleR += st->airFactorR;
        st->flip = !st->flip;

        st->gcountL++; st->gcountR++;
        for (int c = 0; c < 8; c++) { st->positionL[c] += speedL; st->positionR[c] += speedR; }

        int gplusL = st->gcountL + (int)adjust;
        int lastplusL = st->lastcountL + (int)adjust;
        if (gplusL > width) gplusL -= width;
        if (lastplusL > width) lastplusL -= width;

        int gplusR = st->gcountR + (int)adjust;
        int lastplusR = st->lastcountR + (int)adjust;
        if (gplusR > width) gplusR -= width;
        if (lastplusR > width) lastplusR -= width;

        if (st->trackingL[st->activeL] == 0.0f) {
            float posplusL = st->positionL[st->activeL] + adjust;
            float lastposplusL = st->lastpositionL[st->activeL] + adjust;
            if (posplusL > (float)width) posplusL -= (float)width;
            if (lastposplusL > (float)width) lastposplusL -= (float)width;
            if ((gplusL > st->positionL[st->activeL]) && (lastplusL < st->lastpositionL[st->activeL])) st->trackingL[st->activeL] = 1.0f;
            if ((posplusL > (float)st->gcountL) && (lastposplusL < (float)st->lastcountL)) st->trackingL[st->activeL] = 1.0f;
        }
        if (st->trackingR[st->activeR] == 0.0f) {
            float posplusR = st->positionR[st->activeR] + adjust;
            float lastposplusR = st->lastpositionR[st->activeR] + adjust;
            if (posplusR > (float)width) posplusR -= (float)width;
            if (lastposplusR > (float)width) lastposplusR -= (float)width;
            if ((gplusR > st->positionR[st->activeR]) && (lastplusR < st->lastpositionR[st->activeR])) st->trackingR[st->activeR] = 1.0f;
            if ((posplusR > (float)st->gcountR) && (lastposplusR < (float)st->lastcountR)) st->trackingR[st->activeR] = 1.0f;
        }

        for (int c = 0; c < 8; c++) {
            if (st->positionL[c] > (float)width) st->positionL[c] -= (float)width;
            if (st->positionR[c] > (float)width) st->positionR[c] -= (float)width;
            st->lastpositionL[c] = st->positionL[c];
            st->lastpositionR[c] = st->positionR[c];
        }
        if (st->gcountL < 0 || st->gcountL > width) st->gcountL -= width;
        st->lastcountL = st->gcountL;
        if (st->gcountR < 0 || st->gcountR > width) st->gcountR -= width;
        st->lastcountR = st->gcountR;
        // bcountL/bcountR are provably bounded to [0, width] by the wrap-check just above
        // (gcountL only ever grows by exactly 1 per cycle, caught and pulled back by `width` the
        // moment it exceeds it), so bcountL+width stays safely inside pL[5001] on its own; this
        // clamp is a true last-resort backstop only (matching the original's own array-size
        // headroom), never expected to actually trigger in normal operation.
        int bcountL = st->gcountL, bcountR = st->gcountR;
        if (bcountL < 0) bcountL = 0;
        if (bcountL > width) bcountL = width;
        if (bcountR < 0) bcountR = 0;
        if (bcountR > width) bcountR = width;

        st->pL[bcountL + width] = st->pL[bcountL] = inputSampleL;
        st->pR[bcountR + width] = st->pR[bcountR] = inputSampleR;

        for (int c = 0; c < 8; c++) {
            // positionL[c]/positionR[c] are wrapped into [0, width] just above too, so base+2
            // naturally stays well inside pL[5001]'s 2*width+401 headroom -- same backstop-only
            // rationale as bcountL/bcountR.
            int base = (int)floorf(st->positionL[c]);
            if (base < 0) base = 0;
            if (base > 4998) base = 4998;
            st->tempL[c] = st->pL[base] * (1.0f - (st->positionL[c] - (float)base));
            st->tempL[c] += st->pL[base + 1];
            st->tempL[c] += st->pL[base + 2] * (st->positionL[c] - (float)base);
            st->tempL[c] -= ((st->pL[base] - st->pL[base + 1]) - (st->pL[base + 1] - st->pL[base + 2])) / 50.0f;
            st->tempL[c] /= 2.0f;

            base = (int)floorf(st->positionR[c]);
            if (base < 0) base = 0;
            if (base > 4998) base = 4998;
            st->tempR[c] = st->pR[base] * (1.0f - (st->positionR[c] - (float)base));
            st->tempR[c] += st->pR[base + 1];
            st->tempR[c] += st->pR[base + 2] * (st->positionR[c] - (float)base);
            st->tempR[c] -= ((st->pR[base] - st->pR[base + 1]) - (st->pR[base + 1] - st->pR[base + 2])) / 50.0f;
            st->tempR[c] /= 2.0f;
        }

        if (st->trackingL[st->activeL] > 0.0f) {
            float crossfade = sinf(st->trackingL[st->bestspliceL] * 1.57f);
            inputSampleL = (st->tempL[st->activeL] * crossfade) + (st->tempL[st->bestspliceL] * (1.0f - crossfade));
            for (int c = 0; c < 8; c++) {
                float depthv = 0.5f - fabsf(st->tempL[st->activeL] - st->tempL[c]);
                if (depthv > 0.0f && c != st->activeL) { st->trackingL[c] -= (depthv / adjust); st->bestspliceL = c; }
            }
            float bestyetL = 1.0f;
            for (int c = 0; c < 8; c++) if (st->trackingL[c] < bestyetL && c != st->activeL) { st->bestspliceL = c; bestyetL = st->trackingL[c]; }
            if (st->trackingL[st->bestspliceL] < 0.0f) {
                for (int c = 0; c < 8; c++) st->trackingL[c] = 1.0f;
                st->activeL = st->bestspliceL;
                st->trackingL[st->activeL] = 0.0f;
            }
        } else {
            inputSampleL = st->tempL[st->activeL];
        }

        if (st->trackingR[st->activeR] > 0.0f) {
            float crossfade = sinf(st->trackingR[st->bestspliceR] * 1.57f);
            inputSampleR = (st->tempR[st->activeR] * crossfade) + (st->tempR[st->bestspliceR] * (1.0f - crossfade));
            for (int c = 0; c < 8; c++) {
                float depthv = 0.5f - fabsf(st->tempR[st->activeR] - st->tempR[c]);
                if (depthv > 0.0f && c != st->activeR) { st->trackingR[c] -= (depthv / adjust); st->bestspliceR = c; }
            }
            float bestyetR = 1.0f;
            for (int c = 0; c < 8; c++) if (st->trackingR[c] < bestyetR && c != st->activeR) { st->bestspliceR = c; bestyetR = st->trackingR[c]; }
            if (st->trackingR[st->bestspliceR] < 0.0f) {
                for (int c = 0; c < 8; c++) st->trackingR[c] = 1.0f;
                st->activeR = st->bestspliceR;
                st->trackingR[st->activeR] = 0.0f;
            }
        } else {
            inputSampleR = st->tempR[st->activeR];
        }

        inputSampleL = (inputSampleL * wet) + (drySampleL * (1.0f - wet));
        inputSampleR = (inputSampleR * wet) + (drySampleR * (1.0f - wet));

        // Downsampling-ladder reconstruction -- see run_fx_ensemble's identical pattern/comment.
        if (st->cycle_end == 4) {
            st->lastRefL[0] = st->lastRefL[4];
            st->lastRefL[2] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[1] = (st->lastRefL[0] + st->lastRefL[2]) * 0.5f;
            st->lastRefL[3] = (st->lastRefL[2] + inputSampleL) * 0.5f;
            st->lastRefL[4] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[4];
            st->lastRefR[2] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[1] = (st->lastRefR[0] + st->lastRefR[2]) * 0.5f;
            st->lastRefR[3] = (st->lastRefR[2] + inputSampleR) * 0.5f;
            st->lastRefR[4] = inputSampleR;
        } else if (st->cycle_end == 3) {
            st->lastRefL[0] = st->lastRefL[3];
            st->lastRefL[2] = (st->lastRefL[0] + st->lastRefL[0] + inputSampleL) / 3.0f;
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL + inputSampleL) / 3.0f;
            st->lastRefL[3] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[3];
            st->lastRefR[2] = (st->lastRefR[0] + st->lastRefR[0] + inputSampleR) / 3.0f;
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR + inputSampleR) / 3.0f;
            st->lastRefR[3] = inputSampleR;
        } else if (st->cycle_end == 2) {
            st->lastRefL[0] = st->lastRefL[2];
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[2] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[2];
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[2] = inputSampleR;
        } else {
            st->lastRefL[0] = inputSampleL;
            st->lastRefR[0] = inputSampleR;
        }
        st->cycle = 0;
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    } else {
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    }

    int expon;
    frexpf(inputSampleL, &expon);
    st->fpdL ^= st->fpdL << 13; st->fpdL ^= st->fpdL >> 17; st->fpdL ^= st->fpdL << 5;
    inputSampleL += (((float)st->fpdL - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));
    frexpf(inputSampleR, &expon);
    st->fpdR ^= st->fpdR << 13; st->fpdR ^= st->fpdR >> 17; st->fpdR ^= st->fpdR << 5;
    inputSampleR += (((float)st->fpdR - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));

    *l = inputSampleL;
    *r = inputSampleR;
}

// --- FX_TYPE_TAPEDELAY2: ported from Airwindows TapeDelay2 (MIT license) --- see this effect's
// own FX_TYPE_TAPEDELAY2 comment above for the full param map.
//
// Single-tap tape echo: a variable-speed (tape-flutter-modulated) delay line, feedback path run
// through a resonant lowpass ("regen" filter, both channels sharing one set of coefficients) so
// each successive repeat darkens progressively, then a second, slightly higher-resonance lowpass
// on the way OUT of the delay. Both filters are the same biquad topology (state-variable-style
// lowpass, coefficients recomputed from Freq/Reso every call -- cheap, and this dispatch is
// already per-sample, matching every other ported effect here) with their own persistent
// numerator/denominator state (index [7]/[8]) untouched by that recompute.
//
// The variable-speed write/read scheme (delayL/prevSampleL/increment below) is a real
// resampling-safe delay line, not a fixed integer tap: each sample, the write cursor walks
// backward by `speed` positions (fractional), linearly filling in the ring buffer between the
// previous and current cursor position so a later, differently-sped read never hits a stale or
// skipped sample -- this is what makes the tape-flutter pitch modulation sound smooth rather than
// steppy. Buffer size/wrap constant (88200) kept exactly as the original's own 44.1kHz-baseline
// sizing (matching FX_TYPE_ENSEMBLE's own precedent above of never rescaling a ported effect's
// fixed buffer sizes for SAMPLE_RATE) -- Time/Flutter's musical range shifts very slightly at our
// fixed 48kHz vs. the original's variable host rate, exactly the same "close enough, not worth a
// second set of tuning constants" tradeoff already accepted by every other port in this file.
typedef struct {
    float dL[88211], dR[88211];
    float prevSampleL, delayL, sweepL;
    float prevSampleR, delayR, sweepR;
    float regenFilterL[9], outFilterL[9], lastRefL[10];
    float regenFilterR[9], outFilterR[9], lastRefR[10];
    int cycle, cycle_end;
    uint32_t fpdL, fpdR;
} FxTapeDelay2State;

void run_fx_tapedelay2(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxTapeDelay2State *st = (FxTapeDelay2State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxTapeDelay2State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        int cycle_end = SAMPLE_RATE / 44100; if (cycle_end < 1) cycle_end = 1; if (cycle_end > 4) cycle_end = 4;
        st->cycle_end = cycle_end;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }
    if (st->cycle > st->cycle_end - 1) st->cycle = st->cycle_end - 1;

    // param[0..5] A-F, per this effect's own FX_TYPE_TAPEDELAY2 comment above.
    float a = fmaxf(0.0f, fminf(1.0f, params->param[0])); // Time (inverted -- see the comment above)
    float b = fmaxf(0.0f, fminf(1.0f, params->param[1])); // Regen
    float c = fmaxf(0.0f, fminf(1.0f, params->param[2])); // Freq
    float d = fmaxf(0.0f, fminf(1.0f, params->param[3])); // Reso
    float e = fmaxf(0.0f, fminf(1.0f, params->param[4])); // Flutter
    float f = fmaxf(0.0f, fminf(1.0f, params->param[5])); // Dry/Wet
    // param[6] Ping-Pong -- see this effect's own comment above and the cross-feed/divergence
    // just below. Off (0, the default) reproduces the pre-existing wiring exactly, bit for bit.
    // Stereo Ping-Pong (1) cross-feeds L/R's feedback AND diverges their delay time -- an
    // in-between "cross-feed only, same shared time" option existed briefly but was cut: it's a
    // strict subset of what Stereo Ping-Pong already does, and without the time divergence it's
    // the same easy-to-miss, barely-audible effect Ping-Pong originally shipped as (see this
    // effect's own comment history) -- no reason to keep a weaker redundant option once the
    // stronger one exists. Only decoded when the slot's Type is actually this effect, per this
    // codebase's usual "choice" param convention: a plain 0..1 fraction decoded into one of N
    // discrete options, same idiom used throughout this file.
    bool stereo_pingpong = ((int)(fmaxf(0.0f, fminf(1.0f, params->param[6])) * 1.999f)) != 0;

    float baseSpeed = (powf(a, 4.0f) * 25.0f) + 1.0f;
    float feedback = b * b;

    st->regenFilterL[0] = st->regenFilterR[0] = (powf(c, 3.0f) * 0.4f) + 0.0001f;
    st->regenFilterL[1] = st->regenFilterR[1] = (d * d) + 0.01f;
    float K = tanf((float)M_PI * st->regenFilterR[0]);
    float norm = 1.0f / (1.0f + K / st->regenFilterR[1] + K * K);
    st->regenFilterL[2] = st->regenFilterR[2] = K / st->regenFilterR[1] * norm;
    st->regenFilterL[4] = st->regenFilterR[4] = -st->regenFilterR[2];
    st->regenFilterL[5] = st->regenFilterR[5] = 2.0f * (K * K - 1.0f) * norm;
    st->regenFilterL[6] = st->regenFilterR[6] = (1.0f - K / st->regenFilterR[1] + K * K) * norm;

    st->outFilterL[0] = st->outFilterR[0] = st->regenFilterR[0];
    st->outFilterL[1] = st->outFilterR[1] = st->regenFilterR[1] * 1.618033988749894848f; // golden ratio, matching the original exactly
    K = tanf((float)M_PI * st->outFilterR[0]);
    norm = 1.0f / (1.0f + K / st->outFilterR[1] + K * K);
    st->outFilterL[2] = st->outFilterR[2] = K / st->outFilterR[1] * norm;
    st->outFilterL[4] = st->outFilterR[4] = -st->outFilterR[2];
    st->outFilterL[5] = st->outFilterR[5] = 2.0f * (K * K - 1.0f) * norm;
    st->outFilterL[6] = st->outFilterR[6] = (1.0f - K / st->outFilterR[1] + K * K) * norm;

    float vibSpeed = powf(e, 5.0f) * baseSpeed * ((st->regenFilterR[0] * 0.09f) + 0.025f);
    float wet = fmaxf(0.0f, fminf(1.0f, f * 2.0f));
    float dry = fmaxf(0.0f, fminf(1.0f, 2.0f - (f * 2.0f)));
    // This echo makes 50% full dry AND full wet, not crossfaded (see the FX_TYPE_TAPEDELAY2
    // comment above) -- so it can sit on a submix without cutting back the dry signal just
    // because this effect's own Dry/Wet got turned up. The slot's own generic Mix knob (applied
    // by process_fx_chain around this function's whole output) is a normal crossfade, layered on
    // top exactly like FX_TYPE_STEREODOUBLER's own kept Dry/Wet already is.

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;
    float drySampleL = inputSampleL, drySampleR = inputSampleR;

    st->cycle++;
    if (st->cycle == st->cycle_end) {
        float speedL = baseSpeed + (vibSpeed * (sinf(st->sweepL) + 1.0f));
        float speedR = baseSpeed + (vibSpeed * (sinf(st->sweepR) + 1.0f));
        st->sweepL += (0.05f * inputSampleL * inputSampleL); if (st->sweepL > 6.283185307179586f) st->sweepL -= 6.283185307179586f;
        st->sweepR += (0.05f * inputSampleR * inputSampleR); if (st->sweepR > 6.283185307179586f) st->sweepR -= 6.283185307179586f;

        // Stereo Ping-Pong: spread L/R symmetrically around the Time knob's own shared center
        // instead of the two channels always sharing one identical delay rate -- a fixed, tasteful
        // +/-15% (chosen by ear, not derived from anything) rather than a further knob, per the
        // user's own "easily integrate... as an option" ask. Genuinely new time divergence, not
        // just alternating the SAME echo between speakers -- both channels still ultimately feed
        // off the one Time/Regen/Freq/Reso setting, they just no longer land at the same moment.
        // Off: stays at 1.0x, unaffected.
        if (stereo_pingpong) {
            speedL *= 0.85f;
            speedR *= 1.15f;
        }

        // Belt-and-suspenders bounds guard: delayL/R drive every dL[]/dR[] index below via
        // floorf(), and casting a non-finite float to int is undefined behavior in C -- a wild
        // index is exactly the kind of thing that corrupts the heap now and aborts somewhere
        // unrelated later. process_fx_chain() already sanitizes non-finite audio at its own single
        // chokepoint, but this guards the persistent delayL/R state itself in case it was ever
        // poisoned some other way (extreme feedback/resonance self-oscillation, for instance).
        if (!isfinite(st->delayL) || st->delayL < 0.0f || st->delayL >= 88200.0f) st->delayL = 0.0f;
        if (!isfinite(st->delayR) || st->delayR < 0.0f || st->delayR >= 88200.0f) st->delayR = 0.0f;

        // Ping-Pong cross-feed point: capture each channel's CURRENT delay-tap value before
        // either channel's own write step touches it, so the choice of self-feed (Off, default)
        // vs. cross-feed (Stereo Ping-Pong) is a pure selection between two already-computed
        // values -- nothing else about the algorithm changes either way. With stereo_pingpong ==
        // false, feedSampleL/R are exactly dL[posL]/dR[posR] -- byte-for-byte the original's own
        // self-feedback wiring.
        int posL = (int)floorf(st->delayL);
        int posR = (int)floorf(st->delayR);
        if (posL < 0 || posL >= 88200) posL = 0;
        if (posR < 0 || posR >= 88200) posR = 0;
        float feedFromL = st->dL[posL];
        float feedFromR = st->dR[posR];
        float feedSampleL = stereo_pingpong ? feedFromR : feedFromL;
        float feedSampleR = stereo_pingpong ? feedFromL : feedFromR;

        // begin left channel
        float newSample = inputSampleL + feedSampleL * feedback;
        float tempSample = (newSample * st->regenFilterL[2]) + st->regenFilterL[7];
        st->regenFilterL[7] = -(tempSample * st->regenFilterL[5]) + st->regenFilterL[8];
        st->regenFilterL[8] = (newSample * st->regenFilterL[4]) - (tempSample * st->regenFilterL[6]);
        newSample = tempSample;

        st->delayL -= speedL; if (st->delayL < 0.0f) st->delayL += 88200.0f;
        float increment = (newSample - st->prevSampleL) / speedL;
        int pos = posL;
        st->dL[pos] = st->prevSampleL;
        while (pos != (int)floorf(st->delayL)) {
            st->dL[pos] = st->prevSampleL;
            st->prevSampleL += increment;
            pos--; if (pos < 0) pos += 88200;
        }
        st->prevSampleL = newSample;
        pos = (int)floorf(st->delayL); if (pos < 0 || pos >= 88200) pos = 0; inputSampleL = st->dL[pos];
        tempSample = (inputSampleL * st->outFilterL[2]) + st->outFilterL[7];
        st->outFilterL[7] = -(tempSample * st->outFilterL[5]) + st->outFilterL[8];
        st->outFilterL[8] = (inputSampleL * st->outFilterL[4]) - (tempSample * st->outFilterL[6]);
        inputSampleL = tempSample;
        // end left channel

        // begin right channel
        newSample = inputSampleR + feedSampleR * feedback;
        tempSample = (newSample * st->regenFilterR[2]) + st->regenFilterR[7];
        st->regenFilterR[7] = -(tempSample * st->regenFilterR[5]) + st->regenFilterR[8];
        st->regenFilterR[8] = (newSample * st->regenFilterR[4]) - (tempSample * st->regenFilterR[6]);
        newSample = tempSample;

        st->delayR -= speedR; if (st->delayR < 0.0f) st->delayR += 88200.0f;
        increment = (newSample - st->prevSampleR) / speedR;
        pos = posR;
        st->dR[pos] = st->prevSampleR;
        while (pos != (int)floorf(st->delayR)) {
            st->dR[pos] = st->prevSampleR;
            st->prevSampleR += increment;
            pos--; if (pos < 0) pos += 88200;
        }
        st->prevSampleR = newSample;
        pos = (int)floorf(st->delayR); if (pos < 0 || pos >= 88200) pos = 0; inputSampleR = st->dR[pos];
        tempSample = (inputSampleR * st->outFilterR[2]) + st->outFilterR[7];
        st->outFilterR[7] = -(tempSample * st->outFilterR[5]) + st->outFilterR[8];
        st->outFilterR[8] = (inputSampleR * st->outFilterR[4]) - (tempSample * st->outFilterR[6]);
        inputSampleR = tempSample;
        // end right channel

        // Downsampling-ladder reconstruction -- see run_fx_ensemble's identical pattern/comment.
        if (st->cycle_end == 4) {
            st->lastRefL[0] = st->lastRefL[4];
            st->lastRefL[2] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[1] = (st->lastRefL[0] + st->lastRefL[2]) * 0.5f;
            st->lastRefL[3] = (st->lastRefL[2] + inputSampleL) * 0.5f;
            st->lastRefL[4] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[4];
            st->lastRefR[2] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[1] = (st->lastRefR[0] + st->lastRefR[2]) * 0.5f;
            st->lastRefR[3] = (st->lastRefR[2] + inputSampleR) * 0.5f;
            st->lastRefR[4] = inputSampleR;
        } else if (st->cycle_end == 3) {
            st->lastRefL[0] = st->lastRefL[3];
            st->lastRefL[2] = (st->lastRefL[0] + st->lastRefL[0] + inputSampleL) / 3.0f;
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL + inputSampleL) / 3.0f;
            st->lastRefL[3] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[3];
            st->lastRefR[2] = (st->lastRefR[0] + st->lastRefR[0] + inputSampleR) / 3.0f;
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR + inputSampleR) / 3.0f;
            st->lastRefR[3] = inputSampleR;
        } else if (st->cycle_end == 2) {
            st->lastRefL[0] = st->lastRefL[2];
            st->lastRefL[1] = (st->lastRefL[0] + inputSampleL) * 0.5f;
            st->lastRefL[2] = inputSampleL;
            st->lastRefR[0] = st->lastRefR[2];
            st->lastRefR[1] = (st->lastRefR[0] + inputSampleR) * 0.5f;
            st->lastRefR[2] = inputSampleR;
        } else {
            st->lastRefL[0] = inputSampleL;
            st->lastRefR[0] = inputSampleR;
        }
        st->cycle = 0;
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    } else {
        inputSampleL = st->lastRefL[st->cycle];
        inputSampleR = st->lastRefR[st->cycle];
    }

    // Multi-pole average using lastRef[7]/[8] (a second, slower-tracking smoothing stage on top
    // of the ladder above) -- a no-op at cycle_end == 1 (always true at this engine's fixed
    // 48kHz), kept generic for the same "in case SAMPLE_RATE ever changes" reason as the ladder.
    switch (st->cycle_end) {
        case 4:
            st->lastRefL[8] = inputSampleL; inputSampleL = (inputSampleL + st->lastRefL[7]) * 0.5f;
            st->lastRefL[7] = st->lastRefL[8];
            st->lastRefR[8] = inputSampleR; inputSampleR = (inputSampleR + st->lastRefR[7]) * 0.5f;
            st->lastRefR[7] = st->lastRefR[8];
            // fallthrough
        case 3:
            st->lastRefL[8] = inputSampleL; inputSampleL = (inputSampleL + st->lastRefL[6]) * 0.5f;
            st->lastRefL[6] = st->lastRefL[8];
            st->lastRefR[8] = inputSampleR; inputSampleR = (inputSampleR + st->lastRefR[6]) * 0.5f;
            st->lastRefR[6] = st->lastRefR[8];
            // fallthrough
        case 2:
            st->lastRefL[8] = inputSampleL; inputSampleL = (inputSampleL + st->lastRefL[5]) * 0.5f;
            st->lastRefL[5] = st->lastRefL[8];
            st->lastRefR[8] = inputSampleR; inputSampleR = (inputSampleR + st->lastRefR[5]) * 0.5f;
            st->lastRefR[5] = st->lastRefR[8];
            // fallthrough
        case 1:
        default:
            break; // no further averaging
    }

    if (wet < 1.0f) { inputSampleL *= wet; inputSampleR *= wet; }
    if (dry < 1.0f) { drySampleL *= dry; drySampleR *= dry; }
    inputSampleL += drySampleL; inputSampleR += drySampleR;
    // this is our submix echo dry/wet: 0.5 is BOTH at FULL VOLUME (see the comment above)

    int expon;
    frexpf(inputSampleL, &expon);
    st->fpdL ^= st->fpdL << 13; st->fpdL ^= st->fpdL >> 17; st->fpdL ^= st->fpdL << 5;
    inputSampleL += (((float)st->fpdL - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));
    frexpf(inputSampleR, &expon);
    st->fpdR ^= st->fpdR << 13; st->fpdR ^= st->fpdR >> 17; st->fpdR ^= st->fpdR << 5;
    inputSampleR += (((float)st->fpdR - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));

    *l = inputSampleL;
    *r = inputSampleR;
}
