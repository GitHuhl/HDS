// fx_airwindows_tone.c -- Airwindows tone/drive/cabinet ports: CreamCoat, Density3, PointyGuitar, GuitarConditioner, Cabs2.
// Moved verbatim out of fx_chain.c; each effect's run_fx_*() is called by process_fx_chain() via
// fx_effects.h, and its state struct stays private to this file.
#include "fx/fx_effects.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- FX_TYPE_CREAMCOAT: ported from Airwindows CreamCoat (MIT license) ---
// A genuine reverb (unlike Ensemble above): a 4-stage, 4-way Householder feedback delay
// network -- 16 delay lines (A-P), each of the two stereo channels using all 16 physical delay
// lengths but reading/writing them in a different rotation per stage, so L and R decorrelate
// while still sharing one set of tuned lengths. The network runs at a continuously variable,
// reduced internal "tank" rate (DeRez) and reconstructs a full-rate output via a 3-point
// bezier-style blend of the tank's last few ticks (the bez_* fields below -- ported from the
// original's bez[] array, named here for readability, same meaning/order).
// param[0] Select: 17 room-size presets (delay lengths swapped live on change, mid-tank-content
//   reset but feedback/reconstruction state deliberately left alone, matching the original).
// param[1] Regen: feedback amount. param[2] DeRez: tank downsample rate (1.0 = full rate/
//   quality; lower = grittier, more aliased tank, an intentional character control, not just a
//   CPU knob). param[3] Predelay. param[4] Wetness: NOT a crossfade -- 0.5 is both dry and wet
//   at full strength (see the original's own comment, kept below), so raising it adds reverb
//   level without pulling back the dry signal.
#define CREAMCOAT_PREDELAY 15000
// Compile-time max length for each of the 16 delay lines (+5 headroom, matching the original
// header). The runtime length actually in use (shortA..shortP below) depends on the Select
// preset and is always within these maximums.
#define CC_KA 350
#define CC_KB 1710
#define CC_KC 1610
#define CC_KD 835
#define CC_KE 700
#define CC_KF 1260
#define CC_KG 1110
#define CC_KH 1768
#define CC_KI 280
#define CC_KJ 2645
#define CC_KK 1410
#define CC_KL 1175
#define CC_KM 12
#define CC_KN 3110
#define CC_KO 120
#define CC_KP 2370

// 17 room-size presets (Select), each giving all 16 delay-line lengths in A..P order --
// transcribed as-is from the original's switch/case table.
static const int CREAMCOAT_PRESETS[17][16] = {
    {65,124,83,180,200,291,108,189,73,410,479,310,11,928,23,654},
    {114,205,498,195,205,318,143,254,64,721,512,324,11,782,26,394},
    {118,272,292,145,200,241,204,504,50,678,424,412,11,1124,47,766},
    {19,474,301,275,260,321,371,571,50,410,697,414,11,986,47,522},
    {112,387,452,289,173,476,321,593,73,343,829,91,11,1055,43,862},
    {60,368,295,272,210,284,326,830,125,236,737,486,11,1178,75,902},
    {73,311,472,251,134,509,393,591,124,1070,340,525,11,1367,75,816},
    {159,518,514,165,275,494,296,667,75,1101,116,414,11,1261,79,998},
    {41,741,274,59,306,332,291,767,42,881,959,422,11,1237,45,958},
    {251,437,783,189,130,272,244,761,128,1190,320,491,11,1409,58,455},
    {316,510,1087,349,359,74,79,1269,34,693,749,511,11,1751,93,403},
    {254,651,845,316,373,267,182,857,215,1535,1127,315,11,1649,97,829},
    {113,101,673,357,340,229,278,1008,265,1890,155,267,11,2233,116,600},
    {218,1058,862,505,297,580,532,1387,120,576,1409,473,11,1991,76,685},
    {78,760,982,528,445,1128,130,708,22,2144,354,1169,11,2782,58,1515},
    {330,107,1110,371,620,143,1014,1763,184,2068,1406,595,11,2639,33,1594},
    {336,1660,386,623,693,1079,891,1574,24,2641,1239,775,11,3104,55,2366},
};

typedef struct {
    float aAL[CC_KA+5], aBL[CC_KB+5], aCL[CC_KC+5], aDL[CC_KD+5];
    float aEL[CC_KE+5], aFL[CC_KF+5], aGL[CC_KG+5], aHL[CC_KH+5];
    float aIL[CC_KI+5], aJL[CC_KJ+5], aKL[CC_KK+5], aLL[CC_KL+5];
    float aML[CC_KM+5], aNL[CC_KN+5], aOL[CC_KO+5], aPL[CC_KP+5];

    float aAR[CC_KA+5], aBR[CC_KB+5], aCR[CC_KC+5], aDR[CC_KD+5];
    float aER[CC_KE+5], aFR[CC_KF+5], aGR[CC_KG+5], aHR[CC_KH+5];
    float aIR[CC_KI+5], aJR[CC_KJ+5], aKR[CC_KK+5], aLR[CC_KL+5];
    float aMR[CC_KM+5], aNR[CC_KN+5], aOR[CC_KO+5], aPR[CC_KP+5];

    float aZL[CREAMCOAT_PREDELAY+5], aZR[CREAMCOAT_PREDELAY+5];

    float feedbackAL, feedbackBL, feedbackCL, feedbackDL;
    float feedbackDR, feedbackHR, feedbackLR, feedbackPR;

    int countAL, countBL, countCL, countDL, countEL, countFL, countGL, countHL;
    int countIL, countJL, countKL, countLL, countML, countNL, countOL, countPL;
    int countAR, countBR, countCR, countDR, countER, countFR, countGR, countHR;
    int countIR, countJR, countKR, countLR, countMR, countNR, countOR, countPR;
    int countZ;

    int shortA, shortB, shortC, shortD, shortE, shortF, shortG, shortH;
    int shortI, shortJ, shortK, shortL, shortM, shortN, shortO, shortP;
    int prevclearcoat;

    // Bezier-curve tank-rate reconstruction state (ported from the original's bez[] array,
    // indices bez_AL..bez_cycle -- flattened into named fields here for readability, same
    // meaning/order otherwise).
    float bez_AL, bez_AR, bez_BL, bez_BR, bez_CL, bez_CR;
    float bez_InL, bez_InR, bez_UnInL, bez_UnInR, bez_SampL, bez_SampR;
    float bez_cycle;

    uint32_t fpdL, fpdR;
} FxCreamCoatState;

void run_fx_creamcoat(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxCreamCoatState *st = (FxCreamCoatState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxCreamCoatState));
        engine->state = st;
        if (!st) return;
        st->countAL = st->countBL = st->countCL = st->countDL = 1;
        st->countEL = st->countFL = st->countGL = st->countHL = 1;
        st->countIL = st->countJL = st->countKL = st->countLL = 1;
        st->countML = st->countNL = st->countOL = st->countPL = 1;
        st->countAR = st->countBR = st->countCR = st->countDR = 1;
        st->countER = st->countFR = st->countGR = st->countHR = 1;
        st->countIR = st->countJR = st->countKR = st->countLR = 1;
        st->countMR = st->countNR = st->countOR = st->countPR = 1;
        st->countZ = 1;
        st->prevclearcoat = -1;
        // Matches the original constructor's bez[bez_cycle] = 1.0 (all other bez[] slots 0) --
        // combined with "+= derez" happening before the tick check every call, this guarantees
        // the very first sample processed triggers an initial tank tick.
        st->bez_cycle = 1.0f;
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    float A = fmaxf(0.0f, fminf(1.0f, params->param[0])); // Select
    float B = fmaxf(0.0f, fminf(1.0f, params->param[1])); // Regen
    float C = fmaxf(0.0f, fminf(1.0f, params->param[2])); // DeRez
    float D = fmaxf(0.0f, fminf(1.0f, params->param[3])); // Predelay
    float E = fmaxf(0.0f, fminf(1.0f, params->param[4])); // Wetness

    float overallscale = (float)SAMPLE_RATE / 44100.0f;

    int clearcoat = (int)(A * 16.999f);
    if (clearcoat != st->prevclearcoat) {
        // Room preset changed live: clear the 16 delay lines' content and their L/R counters
        // (matching the original exactly) -- but deliberately NOT the feedback/bez/predelay
        // state, same scope the original resets, presumably so the transition isn't a hard mute.
        memset(st->aAL, 0, sizeof(st->aAL)); memset(st->aAR, 0, sizeof(st->aAR));
        memset(st->aBL, 0, sizeof(st->aBL)); memset(st->aBR, 0, sizeof(st->aBR));
        memset(st->aCL, 0, sizeof(st->aCL)); memset(st->aCR, 0, sizeof(st->aCR));
        memset(st->aDL, 0, sizeof(st->aDL)); memset(st->aDR, 0, sizeof(st->aDR));
        memset(st->aEL, 0, sizeof(st->aEL)); memset(st->aER, 0, sizeof(st->aER));
        memset(st->aFL, 0, sizeof(st->aFL)); memset(st->aFR, 0, sizeof(st->aFR));
        memset(st->aGL, 0, sizeof(st->aGL)); memset(st->aGR, 0, sizeof(st->aGR));
        memset(st->aHL, 0, sizeof(st->aHL)); memset(st->aHR, 0, sizeof(st->aHR));
        memset(st->aIL, 0, sizeof(st->aIL)); memset(st->aIR, 0, sizeof(st->aIR));
        memset(st->aJL, 0, sizeof(st->aJL)); memset(st->aJR, 0, sizeof(st->aJR));
        memset(st->aKL, 0, sizeof(st->aKL)); memset(st->aKR, 0, sizeof(st->aKR));
        memset(st->aLL, 0, sizeof(st->aLL)); memset(st->aLR, 0, sizeof(st->aLR));
        memset(st->aML, 0, sizeof(st->aML)); memset(st->aMR, 0, sizeof(st->aMR));
        memset(st->aNL, 0, sizeof(st->aNL)); memset(st->aNR, 0, sizeof(st->aNR));
        memset(st->aOL, 0, sizeof(st->aOL)); memset(st->aOR, 0, sizeof(st->aOR));
        memset(st->aPL, 0, sizeof(st->aPL)); memset(st->aPR, 0, sizeof(st->aPR));
        st->countAL = st->countBL = st->countCL = st->countDL = 1;
        st->countEL = st->countFL = st->countGL = st->countHL = 1;
        st->countIL = st->countJL = st->countKL = st->countLL = 1;
        st->countML = st->countNL = st->countOL = st->countPL = 1;
        st->countAR = st->countBR = st->countCR = st->countDR = 1;
        st->countER = st->countFR = st->countGR = st->countHR = 1;
        st->countIR = st->countJR = st->countKR = st->countLR = 1;
        st->countMR = st->countNR = st->countOR = st->countPR = 1;
        const int *preset = CREAMCOAT_PRESETS[clearcoat];
        st->shortA = preset[0];  st->shortB = preset[1];  st->shortC = preset[2];  st->shortD = preset[3];
        st->shortE = preset[4];  st->shortF = preset[5];  st->shortG = preset[6];  st->shortH = preset[7];
        st->shortI = preset[8];  st->shortJ = preset[9];  st->shortK = preset[10]; st->shortL = preset[11];
        st->shortM = preset[12]; st->shortN = preset[13]; st->shortO = preset[14]; st->shortP = preset[15];
        st->prevclearcoat = clearcoat;
    }

    float regen = (1.0f - powf(1.0f - B, 2.0f)) * 0.0625f;
    float derez = C / overallscale;
    if (derez < 0.0005f) derez = 0.0005f;
    if (derez > 1.0f) derez = 1.0f;
    derez = 1.0f / (float)((int)(1.0f / derez)); // hard-locks derez to exact subdivisions of 1.0
    int adjPredelay = (int)(CREAMCOAT_PREDELAY * D * derez);
    float wet = E * 2.0f;
    float dry = 2.0f - wet;
    if (wet > 1.0f) wet = 1.0f; else wet *= wet;
    if (wet < 0.0f) wet = 0.0f;
    if (dry > 1.0f) dry = 1.0f;
    if (dry < 0.0f) dry = 0.0f;
    // This reverb submixes at 50% full dry AND full wet, not crossfaded -- so it can sit on a
    // send/submix without cutting the dry signal back as Wetness is raised (see param[4] doc
    // above). Ported verbatim from the original's own comment.

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;
    float drySampleL = inputSampleL, drySampleR = inputSampleR;

    st->bez_cycle += derez;
    st->bez_SampL += ((inputSampleL + st->bez_InL) * derez);
    st->bez_SampR += ((inputSampleR + st->bez_InR) * derez);
    st->bez_InL = inputSampleL; st->bez_InR = inputSampleR;

    if (st->bez_cycle > 1.0f) { // hit the end point -- run one reverb-tank tick
        st->bez_cycle = 0.0f;

        // Predelay (operates at tank rate -- see adjPredelay's derivation above).
        st->aZL[st->countZ] = st->bez_SampL;
        st->aZR[st->countZ] = st->bez_SampR;
        st->countZ++; if (st->countZ < 0 || st->countZ > adjPredelay) st->countZ = 0;
        st->bez_SampL = st->aZL[st->countZ - ((st->countZ > adjPredelay) ? adjPredelay + 1 : 0)];
        st->bez_SampR = st->aZR[st->countZ - ((st->countZ > adjPredelay) ? adjPredelay + 1 : 0)];

        // Stage 0: input + feedback taps.
        st->aAL[st->countAL] = (st->bez_SampL + st->bez_UnInL) + (st->feedbackAL * regen);
        st->aBL[st->countBL] = (st->bez_SampL + st->bez_UnInL) + (st->feedbackBL * regen);
        st->aCL[st->countCL] = (st->bez_SampL + st->bez_UnInL) + (st->feedbackCL * regen);
        st->aDL[st->countDL] = (st->bez_SampL + st->bez_UnInL) + (st->feedbackDL * regen);
        st->bez_UnInL = st->bez_SampL;

        st->aDR[st->countDR] = (st->bez_SampR + st->bez_UnInR) + (st->feedbackDR * regen);
        st->aHR[st->countHR] = (st->bez_SampR + st->bez_UnInR) + (st->feedbackHR * regen);
        st->aLR[st->countLR] = (st->bez_SampR + st->bez_UnInR) + (st->feedbackLR * regen);
        st->aPR[st->countPR] = (st->bez_SampR + st->bez_UnInR) + (st->feedbackPR * regen);
        st->bez_UnInR = st->bez_SampR;

        st->countAL++; if (st->countAL < 0 || st->countAL > st->shortA) st->countAL = 0;
        st->countBL++; if (st->countBL < 0 || st->countBL > st->shortB) st->countBL = 0;
        st->countCL++; if (st->countCL < 0 || st->countCL > st->shortC) st->countCL = 0;
        st->countDL++; if (st->countDL < 0 || st->countDL > st->shortD) st->countDL = 0;

        st->countDR++; if (st->countDR < 0 || st->countDR > st->shortD) st->countDR = 0;
        st->countHR++; if (st->countHR < 0 || st->countHR > st->shortH) st->countHR = 0;
        st->countLR++; if (st->countLR < 0 || st->countLR > st->shortL) st->countLR = 0;
        st->countPR++; if (st->countPR < 0 || st->countPR > st->shortP) st->countPR = 0;

        float outAL = st->aAL[st->countAL - ((st->countAL > st->shortA) ? st->shortA + 1 : 0)];
        float outBL = st->aBL[st->countBL - ((st->countBL > st->shortB) ? st->shortB + 1 : 0)];
        float outCL = st->aCL[st->countCL - ((st->countCL > st->shortC) ? st->shortC + 1 : 0)];
        float outDL = st->aDL[st->countDL - ((st->countDL > st->shortD) ? st->shortD + 1 : 0)];

        float outDR = st->aDR[st->countDR - ((st->countDR > st->shortD) ? st->shortD + 1 : 0)];
        float outHR = st->aHR[st->countHR - ((st->countHR > st->shortH) ? st->shortH + 1 : 0)];
        float outLR = st->aLR[st->countLR - ((st->countLR > st->shortL) ? st->shortL + 1 : 0)];
        float outPR = st->aPR[st->countPR - ((st->countPR > st->shortP) ? st->shortP + 1 : 0)];

        // Stage 1: 4-way Householder reflection.
        st->aEL[st->countEL] = outAL - (outBL + outCL + outDL);
        st->aFL[st->countFL] = outBL - (outAL + outCL + outDL);
        st->aGL[st->countGL] = outCL - (outAL + outBL + outDL);
        st->aHL[st->countHL] = outDL - (outAL + outBL + outCL);

        st->aCR[st->countCR] = outDR - (outHR + outLR + outPR);
        st->aGR[st->countGR] = outHR - (outDR + outLR + outPR);
        st->aKR[st->countKR] = outLR - (outDR + outHR + outPR);
        st->aOR[st->countOR] = outPR - (outDR + outHR + outLR);

        st->countEL++; if (st->countEL < 0 || st->countEL > st->shortE) st->countEL = 0;
        st->countFL++; if (st->countFL < 0 || st->countFL > st->shortF) st->countFL = 0;
        st->countGL++; if (st->countGL < 0 || st->countGL > st->shortG) st->countGL = 0;
        st->countHL++; if (st->countHL < 0 || st->countHL > st->shortH) st->countHL = 0;

        st->countCR++; if (st->countCR < 0 || st->countCR > st->shortC) st->countCR = 0;
        st->countGR++; if (st->countGR < 0 || st->countGR > st->shortG) st->countGR = 0;
        st->countKR++; if (st->countKR < 0 || st->countKR > st->shortK) st->countKR = 0;
        st->countOR++; if (st->countOR < 0 || st->countOR > st->shortO) st->countOR = 0;

        float outEL = st->aEL[st->countEL - ((st->countEL > st->shortE) ? st->shortE + 1 : 0)];
        float outFL = st->aFL[st->countFL - ((st->countFL > st->shortF) ? st->shortF + 1 : 0)];
        float outGL = st->aGL[st->countGL - ((st->countGL > st->shortG) ? st->shortG + 1 : 0)];
        float outHL = st->aHL[st->countHL - ((st->countHL > st->shortH) ? st->shortH + 1 : 0)];

        float outCR = st->aCR[st->countCR - ((st->countCR > st->shortC) ? st->shortC + 1 : 0)];
        float outGR = st->aGR[st->countGR - ((st->countGR > st->shortG) ? st->shortG + 1 : 0)];
        float outKR = st->aKR[st->countKR - ((st->countKR > st->shortK) ? st->shortK + 1 : 0)];
        float outOR = st->aOR[st->countOR - ((st->countOR > st->shortO) ? st->shortO + 1 : 0)];

        // Stage 2.
        st->aIL[st->countIL] = outEL - (outFL + outGL + outHL);
        st->aJL[st->countJL] = outFL - (outEL + outGL + outHL);
        st->aKL[st->countKL] = outGL - (outEL + outFL + outHL);
        st->aLL[st->countLL] = outHL - (outEL + outFL + outGL);

        st->aBR[st->countBR] = outCR - (outGR + outKR + outOR);
        st->aFR[st->countFR] = outGR - (outCR + outKR + outOR);
        st->aJR[st->countJR] = outKR - (outCR + outGR + outOR);
        st->aNR[st->countNR] = outOR - (outCR + outGR + outKR);

        st->countIL++; if (st->countIL < 0 || st->countIL > st->shortI) st->countIL = 0;
        st->countJL++; if (st->countJL < 0 || st->countJL > st->shortJ) st->countJL = 0;
        st->countKL++; if (st->countKL < 0 || st->countKL > st->shortK) st->countKL = 0;
        st->countLL++; if (st->countLL < 0 || st->countLL > st->shortL) st->countLL = 0;

        st->countBR++; if (st->countBR < 0 || st->countBR > st->shortB) st->countBR = 0;
        st->countFR++; if (st->countFR < 0 || st->countFR > st->shortF) st->countFR = 0;
        st->countJR++; if (st->countJR < 0 || st->countJR > st->shortJ) st->countJR = 0;
        st->countNR++; if (st->countNR < 0 || st->countNR > st->shortN) st->countNR = 0;

        float outIL = st->aIL[st->countIL - ((st->countIL > st->shortI) ? st->shortI + 1 : 0)];
        float outJL = st->aJL[st->countJL - ((st->countJL > st->shortJ) ? st->shortJ + 1 : 0)];
        float outKL = st->aKL[st->countKL - ((st->countKL > st->shortK) ? st->shortK + 1 : 0)];
        float outLL = st->aLL[st->countLL - ((st->countLL > st->shortL) ? st->shortL + 1 : 0)];

        float outBR = st->aBR[st->countBR - ((st->countBR > st->shortB) ? st->shortB + 1 : 0)];
        float outFR = st->aFR[st->countFR - ((st->countFR > st->shortF) ? st->shortF + 1 : 0)];
        float outJR = st->aJR[st->countJR - ((st->countJR > st->shortJ) ? st->shortJ + 1 : 0)];
        float outNR = st->aNR[st->countNR - ((st->countNR > st->shortN) ? st->shortN + 1 : 0)];

        // Stage 3.
        st->aML[st->countML] = outIL - (outJL + outKL + outLL);
        st->aNL[st->countNL] = outJL - (outIL + outKL + outLL);
        st->aOL[st->countOL] = outKL - (outIL + outJL + outLL);
        st->aPL[st->countPL] = outLL - (outIL + outJL + outKL);

        st->aAR[st->countAR] = outBR - (outFR + outJR + outNR);
        st->aER[st->countER] = outFR - (outBR + outJR + outNR);
        st->aIR[st->countIR] = outJR - (outBR + outFR + outNR);
        st->aMR[st->countMR] = outNR - (outBR + outFR + outJR);

        st->countML++; if (st->countML < 0 || st->countML > st->shortM) st->countML = 0;
        st->countNL++; if (st->countNL < 0 || st->countNL > st->shortN) st->countNL = 0;
        st->countOL++; if (st->countOL < 0 || st->countOL > st->shortO) st->countOL = 0;
        st->countPL++; if (st->countPL < 0 || st->countPL > st->shortP) st->countPL = 0;

        st->countAR++; if (st->countAR < 0 || st->countAR > st->shortA) st->countAR = 0;
        st->countER++; if (st->countER < 0 || st->countER > st->shortE) st->countER = 0;
        st->countIR++; if (st->countIR < 0 || st->countIR > st->shortI) st->countIR = 0;
        st->countMR++; if (st->countMR < 0 || st->countMR > st->shortM) st->countMR = 0;

        float outML = st->aML[st->countML - ((st->countML > st->shortM) ? st->shortM + 1 : 0)];
        float outNL = st->aNL[st->countNL - ((st->countNL > st->shortN) ? st->shortN + 1 : 0)];
        float outOL = st->aOL[st->countOL - ((st->countOL > st->shortO) ? st->shortO + 1 : 0)];
        float outPL = st->aPL[st->countPL - ((st->countPL > st->shortP) ? st->shortP + 1 : 0)];

        float outAR = st->aAR[st->countAR - ((st->countAR > st->shortA) ? st->shortA + 1 : 0)];
        float outER = st->aER[st->countER - ((st->countER > st->shortE) ? st->shortE + 1 : 0)];
        float outIR = st->aIR[st->countIR - ((st->countIR > st->shortI) ? st->shortI + 1 : 0)];
        float outMR = st->aMR[st->countMR - ((st->countMR > st->shortM) ? st->shortM + 1 : 0)];

        // Final Householder stage -> next tick's feedback taps.
        st->feedbackAL = outML - (outNL + outOL + outPL);
        st->feedbackDR = outAR - (outER + outIR + outMR);
        st->feedbackBL = outNL - (outML + outOL + outPL);
        st->feedbackHR = outER - (outAR + outIR + outMR);
        st->feedbackCL = outOL - (outML + outNL + outPL);
        st->feedbackLR = outIR - (outAR + outER + outMR);
        st->feedbackDL = outPL - (outML + outNL + outOL);
        st->feedbackPR = outMR - (outAR + outER + outIR);

        // Final combined sum of outputs, corrected for Householder gain and averaging.
        inputSampleL = (outML + outNL + outOL + outPL) / 32.0f;
        inputSampleR = (outAR + outER + outIR + outMR) / 32.0f;

        st->bez_CL = st->bez_BL; st->bez_BL = st->bez_AL; st->bez_AL = inputSampleL; st->bez_SampL = 0.0f;
        st->bez_CR = st->bez_BR; st->bez_BR = st->bez_AR; st->bez_AR = inputSampleR; st->bez_SampR = 0.0f;
    }

    // 3-point bezier-style reconstruction of the tank's last few ticks into a full-rate signal.
    float CBL = (st->bez_CL * (1.0f - st->bez_cycle)) + (st->bez_BL * st->bez_cycle);
    float CBR = (st->bez_CR * (1.0f - st->bez_cycle)) + (st->bez_BR * st->bez_cycle);
    float BAL = (st->bez_BL * (1.0f - st->bez_cycle)) + (st->bez_AL * st->bez_cycle);
    float BAR = (st->bez_BR * (1.0f - st->bez_cycle)) + (st->bez_AR * st->bez_cycle);
    float CBAL = (st->bez_BL + (CBL * (1.0f - st->bez_cycle)) + (BAL * st->bez_cycle)) * 0.125f;
    float CBAR = (st->bez_BR + (CBR * (1.0f - st->bez_cycle)) + (BAR * st->bez_cycle)) * 0.125f;
    float outSampleL = CBAL, outSampleR = CBAR;

    outSampleL = fmaxf(-1.0f, fminf(1.0f, outSampleL));
    outSampleR = fmaxf(-1.0f, fminf(1.0f, outSampleR));

    if (wet < 1.0f) { outSampleL *= wet; outSampleR *= wet; }
    if (dry < 1.0f) { drySampleL *= dry; drySampleR *= dry; }
    outSampleL += drySampleL;
    outSampleR += drySampleR;

    int expon;
    frexpf(outSampleL, &expon);
    st->fpdL ^= st->fpdL << 13; st->fpdL ^= st->fpdL >> 17; st->fpdL ^= st->fpdL << 5;
    outSampleL += (((float)st->fpdL - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));
    frexpf(outSampleR, &expon);
    st->fpdR ^= st->fpdR << 13; st->fpdR ^= st->fpdR >> 17; st->fpdR ^= st->fpdR << 5;
    outSampleR += (((float)st->fpdR - 2147483647.0f) * 5.5e-36f * powf(2.0f, (float)(expon + 62)));

    *l = outSampleL;
    *r = outSampleR;
}

// --- FX_TYPE_DENSITY3: ported from Airwindows Density3 (MIT license) ---
// Highpass (a leaky-integrator lowpass subtracted from the input) feeding a density/waveshaping
// saturator. `density` (0..5, from param[0]*5) selects between two Taylor-series curves: above
// 1.0, an increasingly hard sine-based clip (the higher density goes, the more the input is
// pre-scaled before the sine approximation, so it clips harder); below 1.0, a softer
// arcsine-family curve; density itself blends smoothly through 1.0 (a pure highpass, no added
// coloration). This is a direct per-sample port of the original's processReplacing (the
// float-precision path, matching this engine's own float sample pipeline) -- only VST host
// plumbing (chunks, program names, canDo) was dropped, along with the original's OWN internal
// Dry/Wet param (see the FX_TYPE_DENSITY3 comment on why).
typedef struct {
    float iirSampleL, iirSampleR;
    uint32_t fpdL, fpdR;
} FxDensity3State;

// Shared waveshaping core for one channel, factored out of the original's copy-pasted L/R
// blocks (both channels ran byte-for-byte identical math on their own sample). `x` is the
// signal AFTER the highpass subtraction; returns the same density-blended result the original
// computed inline.
static inline float density3_waveshape_sample(float x, float density) {
    float altered = x;
    if (density > 1.0f) {
        float half_pi = (float)(M_PI * 0.5);
        altered = fmaxf(fminf(x * density * half_pi, half_pi), -half_pi);
        float xx = altered * altered;
        float temp = altered * xx;
        altered -= temp / 6.0f;      temp *= xx;
        altered += temp / 120.0f;    temp *= xx;
        altered -= temp / 5040.0f;   temp *= xx;
        altered += temp / 362880.0f; temp *= xx;
        altered -= temp / 39916800.0f;
    }
    if (density < 1.0f) {
        float clamped = fmaxf(fminf(x, 1.0f), -1.0f);
        float polarity = clamped;
        float xx = x * clamped;
        float temp = xx;
        altered = temp / 2.0f;       temp *= xx;
        altered -= temp / 24.0f;     temp *= xx;
        altered += temp / 720.0f;    temp *= xx;
        altered -= temp / 40320.0f;  temp *= xx;
        altered += temp / 3628800.0f;
        altered *= (polarity < 0.0f) ? -1.0f : 1.0f;
    }
    if (density > 2.0f) return altered;
    float blend = fabsf(density - 1.0f);
    return x * (1.0f - blend) + altered * blend;
}

void run_fx_density3(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxDensity3State *st = (FxDensity3State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxDensity3State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    // param[0] Density, param[1] Highpass, param[2] Output -- all 0..1 (see the FX_TYPE_DENSITY3
    // comment above for what each becomes internally).
    float density = fmaxf(0.0f, fminf(1.0f, params->param[0])) * 5.0f;
    float highpass = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    float output = fmaxf(0.0f, fminf(1.0f, params->param[2]));

    // overallscale/iirAmount match the original exactly: the highpass's leaky-integrator
    // coefficient is scaled so its character stays consistent across sample rates, referenced
    // against 44100Hz. highpass^3 (not highpass itself) gives most of the knob's travel to the
    // low end of its range, where the audible difference per turn is largest.
    float overallscale = (float)SAMPLE_RATE / 44100.0f;
    float iir_amount = (highpass * highpass * highpass) / overallscale;
    // The original resets its leaky-integrator state to silence once per processReplacing call
    // whenever Highpass is fully off, so a stale nonzero iir sample can never leak back in as a
    // DC-ish offset if Highpass is turned back up later. Called once per sample here rather than
    // once per host buffer, but functionally identical: once iir_amount is 0, this keeps forcing
    // the state to 0 every sample, same end state as the original's once-per-buffer reset.
    if (iir_amount <= 0.0f) { st->iirSampleL = 0.0f; st->iirSampleR = 0.0f; }

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    st->iirSampleL = st->iirSampleL * (1.0f - iir_amount) + inputSampleL * iir_amount;
    inputSampleL -= st->iirSampleL;
    st->iirSampleR = st->iirSampleR * (1.0f - iir_amount) + inputSampleR * iir_amount;
    inputSampleR -= st->iirSampleR;

    inputSampleL = density3_waveshape_sample(inputSampleL, density);
    inputSampleR = density3_waveshape_sample(inputSampleR, density);

    inputSampleL *= output;
    inputSampleR *= output;

    // 32-bit-float dither, scaled relative to each sample's own magnitude (frexpf's exponent) --
    // same convention (and constant, 5.5e-36f) already used by run_fx_ensemble/run_fx_creamcoat
    // above, rather than the original plugin's own literal (Airwindows' per-plugin dither
    // constants vary release to release; this codebase standardizes on one).
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

// --- FX_TYPE_POINTYGUITAR: ported from Airwindows PointyGuitar (MIT license) --- see this
// effect's own FX_TYPE_POINTYGUITAR comment above for the full param map and algorithm summary.
//
// angS/angA are the per-pole, per-band one-pole filter state (Position/Angle in this codebase's
// usual naming for this "IIR" smoother shape, named angS/angA here to match the original's own
// "angled" band-splitting terminology): angS[x][y] is band y's smoothed level within pole x,
// angA[x][y] its own rate-of-change tracker. angG[0,2,4,6,8] are the 5 real band gains (Presence/
// High/Mid/Low/Sub); angG[1,3,5,7] are the plain average of their neighbors (interpolated
// in-between bands, not independently controlled); angG[9] repeats angG[8] (Sub) as the "band 9"
// weight applied to whatever's left in the lowpass chain after 9 successive splits. angG[] is
// shared by both channels (it depends only on params, not per-channel signal), matching the
// original's own single shared array.
typedef struct {
    float angSL[18][12], angAL[18][12];
    float angSR[18][12], angAR[18][12];
    float iirHPositionL[37], iirHAngleL[37], iirBPositionL[37], iirBAngleL[37];
    float iirHPositionR[37], iirHAngleR[37], iirBPositionR[37], iirBAngleR[37];
    bool was_negative_l, was_negative_r;
    int zero_cross_l, zero_cross_r;
    float gateroller_l, gateroller_r;
    float gate_l, gate_r;
    uint32_t fpdL, fpdR;
} FxPointyGuitarState;

void run_fx_pointyguitar(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxPointyGuitarState *st = (FxPointyGuitarState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxPointyGuitarState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    // param[0..9] A-J, per this effect's own FX_TYPE_POINTYGUITAR comment above.
    float a = fmaxf(0.0f, fminf(1.0f, params->param[0])); // Drive
    float b = fmaxf(0.0f, fminf(1.0f, params->param[1])); // Presence
    float c = fmaxf(0.0f, fminf(1.0f, params->param[2])); // High
    float d = fmaxf(0.0f, fminf(1.0f, params->param[3])); // Mid
    float e = fmaxf(0.0f, fminf(1.0f, params->param[4])); // Low
    float f = fmaxf(0.0f, fminf(1.0f, params->param[5])); // Sub
    float g = fmaxf(0.0f, fminf(1.0f, params->param[6])); // HSpeakr
    float h = fmaxf(0.0f, fminf(1.0f, params->param[7])); // LSpeakr
    float i_ = fmaxf(0.0f, fminf(1.0f, params->param[8])); // Gate
    float j = fmaxf(0.0f, fminf(1.0f, params->param[9])); // Output

    float overallscale = (float)SAMPLE_RATE / 44100.0f;

    float drive = a + 0.618033988749894f;
    float angG[10];
    angG[0] = sqrtf(b * 2.0f);
    angG[2] = sqrtf(c * 2.0f);
    angG[4] = sqrtf(d * 2.0f);
    angG[6] = sqrtf(e * 2.0f);
    angG[8] = sqrtf(f * 2.0f);
    angG[1] = (angG[0] + angG[2]) * 0.5f;
    angG[3] = (angG[2] + angG[4]) * 0.5f;
    angG[5] = (angG[4] + angG[6]) * 0.5f;
    angG[7] = (angG[6] + angG[8]) * 0.5f;
    angG[9] = angG[8];
    int poles = (int)(drive * 10.0f); // 6-16 across Drive's full 0..1 range
    if (poles > 17) poles = 17; // defensive: stay inside angSL/angAL's 18-row headroom regardless
    float hFreq = powf(g, overallscale);
    float lFreq = powf(h, overallscale + 3.0f);
    // begin Gate
    float onthreshold = (powf(i_, 3.0f) / 3.0f) + 0.00018f;
    float offthreshold = onthreshold * 1.1f;
    float release = 0.028331119964586f;
    float absmax = 220.9f;
    // end Gate
    float output = j;

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    // begin Gate (left) -- zero-crossing-aware envelope follower on the CLEAN input, applied to
    // the DISTORTED output further down (see this effect's own comment above for why).
    if (inputSampleL > 0.0f) {
        if (st->was_negative_l) st->zero_cross_l = (int)(absmax * 0.3f);
        st->was_negative_l = false;
    } else {
        st->zero_cross_l += 1; st->was_negative_l = true;
    }
    if (st->zero_cross_l > absmax) st->zero_cross_l = (int)absmax;
    if (st->gateroller_l == 0.0f) {
        if (fabsf(inputSampleL) > onthreshold) {
            if (st->gateroller_l == 0.0f) st->gateroller_l = (float)st->zero_cross_l;
            else st->gateroller_l -= release;
        } else st->gateroller_l -= release;
    } else {
        if (fabsf(inputSampleL) > offthreshold) {
            if (st->gateroller_l < (float)st->zero_cross_l) st->gateroller_l = (float)st->zero_cross_l;
            else st->gateroller_l -= release;
        } else st->gateroller_l -= release;
    }
    if (st->gateroller_l < 0.0f) st->gateroller_l = 0.0f;
    // end Gate (left)

    for (int x = 0; x < poles; x++) {
        float fr = 0.9f / overallscale;
        float band = inputSampleL; inputSampleL = 0.0f;
        for (int y = 0; y < 9; y++) {
            st->angAL[x][y] = (st->angAL[x][y] * (1.0f - fr)) + ((band - st->angSL[x][y]) * fr);
            float temp = band; band = ((st->angSL[x][y] + (st->angAL[x][y] * fr)) * (1.0f - fr)) + (band * fr);
            st->angSL[x][y] = ((st->angSL[x][y] + (st->angAL[x][y] * fr)) * (1.0f - fr)) + (band * fr);
            inputSampleL += ((temp - band) * angG[y]);
            fr *= 0.618033988749894f;
        }
        inputSampleL += (band * angG[9]);
        inputSampleL *= drive;
        inputSampleL -= fminf(fmaxf((inputSampleL * (fabsf(inputSampleL) * 0.654f) * (fabsf(inputSampleL) * 0.654f)), -1.0f), 1.0f);
    }

    if (st->gateroller_l < 1.0f) {
        st->gate_l = st->gateroller_l;
        float bridgerectifier = 1.0f - cosf(fabsf(inputSampleL));
        if (inputSampleL > 0.0f) inputSampleL = (inputSampleL * st->gate_l) + (bridgerectifier * (1.0f - st->gate_l));
        else inputSampleL = (inputSampleL * st->gate_l) - (bridgerectifier * (1.0f - st->gate_l));
        if (st->gate_l == 0.0f) inputSampleL = 0.0f;
    } else st->gate_l = 1.0f;
    // end Gate (left, applied)

    float lowSample = inputSampleL;
    int lPoles = (int)(3.0f + (lFreq * 32.0f));
    for (int count = 0; count < lPoles; count++) {
        st->iirBAngleL[count] = (st->iirBAngleL[count] * (1.0f - lFreq)) + ((lowSample - st->iirBPositionL[count]) * lFreq);
        lowSample = ((st->iirBPositionL[count] + (st->iirBAngleL[count] * lFreq)) * (1.0f - lFreq)) + (lowSample * lFreq);
        st->iirBPositionL[count] = ((st->iirBPositionL[count] + (st->iirBAngleL[count] * lFreq)) * (1.0f - lFreq)) + (lowSample * lFreq);
        inputSampleL -= (lowSample * (1.0f / (3.0f + (lFreq * 32.0f))));
    }

    int hPoles = (int)(3.0f + (hFreq * 32.0f));
    for (int count = 0; count < hPoles; count++) {
        st->iirHAngleL[count] = (st->iirHAngleL[count] * (1.0f - hFreq)) + ((inputSampleL - st->iirHPositionL[count]) * hFreq);
        inputSampleL = ((st->iirHPositionL[count] + (st->iirHAngleL[count] * hFreq)) * (1.0f - hFreq)) + (inputSampleL * hFreq);
        st->iirHPositionL[count] = ((st->iirHPositionL[count] + (st->iirHAngleL[count] * hFreq)) * (1.0f - hFreq)) + (inputSampleL * hFreq);
    } // the lowpass
    inputSampleL *= output;

    // begin Gate (right)
    if (inputSampleR > 0.0f) {
        if (st->was_negative_r) st->zero_cross_r = (int)(absmax * 0.3f);
        st->was_negative_r = false;
    } else {
        st->zero_cross_r += 1; st->was_negative_r = true;
    }
    if (st->zero_cross_r > absmax) st->zero_cross_r = (int)absmax;
    if (st->gateroller_r == 0.0f) {
        if (fabsf(inputSampleR) > onthreshold) {
            if (st->gateroller_r == 0.0f) st->gateroller_r = (float)st->zero_cross_r;
            else st->gateroller_r -= release;
        } else st->gateroller_r -= release;
    } else {
        if (fabsf(inputSampleR) > offthreshold) {
            if (st->gateroller_r < (float)st->zero_cross_r) st->gateroller_r = (float)st->zero_cross_r;
            else st->gateroller_r -= release;
        } else st->gateroller_r -= release;
    }
    if (st->gateroller_r < 0.0f) st->gateroller_r = 0.0f;
    // end Gate (right)

    for (int x = 0; x < poles; x++) {
        float fr = 0.9f / overallscale;
        float band = inputSampleR; inputSampleR = 0.0f;
        for (int y = 0; y < 9; y++) {
            st->angAR[x][y] = (st->angAR[x][y] * (1.0f - fr)) + ((band - st->angSR[x][y]) * fr);
            float temp = band; band = ((st->angSR[x][y] + (st->angAR[x][y] * fr)) * (1.0f - fr)) + (band * fr);
            st->angSR[x][y] = ((st->angSR[x][y] + (st->angAR[x][y] * fr)) * (1.0f - fr)) + (band * fr);
            inputSampleR += ((temp - band) * angG[y]);
            fr *= 0.618033988749894f;
        }
        inputSampleR += (band * angG[9]);
        inputSampleR *= drive;
        inputSampleR -= fminf(fmaxf((inputSampleR * (fabsf(inputSampleR) * 0.654f) * (fabsf(inputSampleR) * 0.654f)), -1.0f), 1.0f);
    }

    if (st->gateroller_r < 1.0f) {
        st->gate_r = st->gateroller_r;
        float bridgerectifier = 1.0f - cosf(fabsf(inputSampleR));
        if (inputSampleR > 0.0f) inputSampleR = (inputSampleR * st->gate_r) + (bridgerectifier * (1.0f - st->gate_r));
        else inputSampleR = (inputSampleR * st->gate_r) - (bridgerectifier * (1.0f - st->gate_r));
        if (st->gate_r == 0.0f) inputSampleR = 0.0f;
    } else st->gate_r = 1.0f;
    // end Gate (right, applied)

    lowSample = inputSampleR;
    for (int count = 0; count < lPoles; count++) {
        st->iirBAngleR[count] = (st->iirBAngleR[count] * (1.0f - lFreq)) + ((lowSample - st->iirBPositionR[count]) * lFreq);
        lowSample = ((st->iirBPositionR[count] + (st->iirBAngleR[count] * lFreq)) * (1.0f - lFreq)) + (lowSample * lFreq);
        st->iirBPositionR[count] = ((st->iirBPositionR[count] + (st->iirBAngleR[count] * lFreq)) * (1.0f - lFreq)) + (lowSample * lFreq);
        inputSampleR -= (lowSample * (1.0f / (3.0f + (lFreq * 32.0f))));
    }

    for (int count = 0; count < hPoles; count++) {
        st->iirHAngleR[count] = (st->iirHAngleR[count] * (1.0f - hFreq)) + ((inputSampleR - st->iirHPositionR[count]) * hFreq);
        inputSampleR = ((st->iirHPositionR[count] + (st->iirHAngleR[count] * hFreq)) * (1.0f - hFreq)) + (inputSampleR * hFreq);
        st->iirHPositionR[count] = ((st->iirHPositionR[count] + (st->iirHAngleR[count] * hFreq)) * (1.0f - hFreq)) + (inputSampleR * hFreq);
    } // the lowpass
    inputSampleR *= output;

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

// --- FX_TYPE_GUITARCONDITIONER: ported from Airwindows GuitarConditioner (MIT license) --- see
// this effect's own FX_TYPE_GUITARCONDITIONER comment above for the full algorithm summary.
// Genuinely parameterless -- `params` is unused, matching the original's own kNumParameters == 0.
typedef struct {
    bool fp_flip;
    float last_sample_tl, last_sample_bl;
    float iir_sample_tal, iir_sample_tbl, iir_sample_bal, iir_sample_bbl;
    float last_sample_tr, last_sample_br;
    float iir_sample_tar, iir_sample_tbr, iir_sample_bar, iir_sample_bbr;
    uint32_t fpdL, fpdR;
} FxGuitarConditionerState;

void run_fx_guitarconditioner(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    (void)params; // no params -- see this effect's own FX_TYPE_GUITARCONDITIONER comment above
    FxGuitarConditionerState *st = (FxGuitarConditionerState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxGuitarConditionerState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->fp_flip = true; // matches the original's own constructor default
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
    }

    float overallscale = (float)SAMPLE_RATE / 44100.0f;
    float iirTreble = 0.287496f / overallscale; // tight is -1
    float iirBass = 0.085184f / overallscale;   // tight is 1
    iirTreble += iirTreble;
    iirBass += iirBass; // simple double when tight is -1 or 1
    const float tightBass = 0.6666666666f;
    const float tightTreble = -0.3333333333f;
    float threshTreble = 0.0081f / overallscale;
    float threshBass = 0.0256f / overallscale;

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    float trebleL = inputSampleL, bassL = inputSampleL;
    float trebleR = inputSampleR, bassR = inputSampleR;
    trebleL += trebleL; // +3dB on treble as the highpass is higher
    trebleR += trebleR;

    float offset = (1.0f + tightTreble) + ((1.0f - fabsf(trebleL)) * tightTreble); // treble HP
    offset = fmaxf(0.0f, fminf(1.0f, offset));
    if (st->fp_flip) {
        st->iir_sample_tal = (st->iir_sample_tal * (1.0f - (offset * iirTreble))) + (trebleL * (offset * iirTreble));
        trebleL -= st->iir_sample_tal;
    } else {
        st->iir_sample_tbl = (st->iir_sample_tbl * (1.0f - (offset * iirTreble))) + (trebleL * (offset * iirTreble));
        trebleL -= st->iir_sample_tbl;
    } // done trebleL HP

    offset = (1.0f + tightTreble) + ((1.0f - fabsf(trebleR)) * tightTreble); // treble HP
    offset = fmaxf(0.0f, fminf(1.0f, offset));
    if (st->fp_flip) {
        st->iir_sample_tar = (st->iir_sample_tar * (1.0f - (offset * iirTreble))) + (trebleR * (offset * iirTreble));
        trebleR -= st->iir_sample_tar;
    } else {
        st->iir_sample_tbr = (st->iir_sample_tbr * (1.0f - (offset * iirTreble))) + (trebleR * (offset * iirTreble));
        trebleR -= st->iir_sample_tbr;
    } // done trebleR HP

    offset = (1.0f - tightBass) + (fabsf(bassL) * tightBass); // bass HP
    offset = fmaxf(0.0f, fminf(1.0f, offset));
    if (st->fp_flip) {
        st->iir_sample_bal = (st->iir_sample_bal * (1.0f - (offset * iirBass))) + (bassL * (offset * iirBass));
        bassL -= st->iir_sample_bal;
    } else {
        st->iir_sample_bbl = (st->iir_sample_bbl * (1.0f - (offset * iirBass))) + (bassL * (offset * iirBass));
        bassL -= st->iir_sample_bbl;
    } // done bassL HP

    offset = (1.0f - tightBass) + (fabsf(bassR) * tightBass); // bass HP
    offset = fmaxf(0.0f, fminf(1.0f, offset));
    if (st->fp_flip) {
        st->iir_sample_bar = (st->iir_sample_bar * (1.0f - (offset * iirBass))) + (bassR * (offset * iirBass));
        bassR -= st->iir_sample_bar;
    } else {
        st->iir_sample_bbr = (st->iir_sample_bbr * (1.0f - (offset * iirBass))) + (bassR * (offset * iirBass));
        bassR -= st->iir_sample_bbr;
    } // done bassR HP

    // Slew-limit each band's sample-to-sample delta (a hard cap on how fast it can move) --
    // ported directly, using each band's own value straight (the original round-trips it through
    // a same-value local first; behaviorally identical, just fewer temporaries).
    float clamp = trebleL - st->last_sample_tl;
    if (clamp > threshTreble) trebleL = st->last_sample_tl + threshTreble;
    if (-clamp > threshTreble) trebleL = st->last_sample_tl - threshTreble;
    st->last_sample_tl = trebleL; // trebleL slew

    clamp = trebleR - st->last_sample_tr;
    if (clamp > threshTreble) trebleR = st->last_sample_tr + threshTreble;
    if (-clamp > threshTreble) trebleR = st->last_sample_tr - threshTreble;
    st->last_sample_tr = trebleR; // trebleR slew

    clamp = bassL - st->last_sample_bl;
    if (clamp > threshBass) bassL = st->last_sample_bl + threshBass;
    if (-clamp > threshBass) bassL = st->last_sample_bl - threshBass;
    st->last_sample_bl = bassL; // bassL slew

    clamp = bassR - st->last_sample_br;
    if (clamp > threshBass) bassR = st->last_sample_br + threshBass;
    if (-clamp > threshBass) bassR = st->last_sample_br - threshBass;
    st->last_sample_br = bassR; // bassR slew

    inputSampleL = trebleL + bassL; // final merge
    inputSampleR = trebleR + bassR; // final merge
    st->fp_flip = !st->fp_flip;

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

// --- FX_TYPE_CABS2: ported from Airwindows Cabs2 (MIT license) --- see this effect's own
// FX_TYPE_CABS2 comment above for the full algorithm summary and param map.
typedef struct {
    float iirHPositionL[23], iirHAngleL[23], iirHPositionR[23], iirHAngleR[23];
    bool hBypass;
    float iirLPositionL[15], iirLAngleL[15], iirLPositionR[15], iirLAngleR[15];
    bool lBypass;
    uint32_t fpdL, fpdR;
} FxCabs2State;

void run_fx_cabs2(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxCabs2State *st = (FxCabs2State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxCabs2State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        st->fpdL = (uint32_t)rand(); while (st->fpdL < 16386) st->fpdL = (uint32_t)rand();
        st->fpdR = (uint32_t)rand(); while (st->fpdR < 16386) st->fpdR = (uint32_t)rand();
        // hBypass/lBypass start false (calloc) -- matches the original's own constructor default.
    }

    float a = fmaxf(0.0f, fminf(1.0f, params->param[0])); // Lowpass
    float b = fmaxf(0.0f, fminf(1.0f, params->param[1])); // Hipass
    float overallscale = (float)SAMPLE_RATE / 44100.0f;
    float lFreq = powf(fmaxf(a, 0.002f), overallscale); // the lowpass
    float hFreq = powf(b, overallscale + 2.0f);          // the highpass

    float inputSampleL = *l, inputSampleR = *r;
    if (fabsf(inputSampleL) < 1.18e-23f) inputSampleL = (float)st->fpdL * 1.18e-17f;
    if (fabsf(inputSampleR) < 1.18e-23f) inputSampleR = (float)st->fpdR * 1.18e-17f;

    if (hFreq > 0.0f) {
        float lowSampleL = inputSampleL, lowSampleR = inputSampleR;
        for (int count = 0; count < 21; count++) {
            st->iirHAngleL[count] = (st->iirHAngleL[count] * (1.0f - hFreq)) + ((lowSampleL - st->iirHPositionL[count]) * hFreq);
            lowSampleL = ((st->iirHPositionL[count] + (st->iirHAngleL[count] * hFreq)) * (1.0f - hFreq)) + (lowSampleL * hFreq);
            st->iirHPositionL[count] = ((st->iirHPositionL[count] + (st->iirHAngleL[count] * hFreq)) * (1.0f - hFreq)) + (lowSampleL * hFreq);
            inputSampleL -= (lowSampleL * (1.0f / 21.0f)); // left
            st->iirHAngleR[count] = (st->iirHAngleR[count] * (1.0f - hFreq)) + ((lowSampleR - st->iirHPositionR[count]) * hFreq);
            lowSampleR = ((st->iirHPositionR[count] + (st->iirHAngleR[count] * hFreq)) * (1.0f - hFreq)) + (lowSampleR * hFreq);
            st->iirHPositionR[count] = ((st->iirHPositionR[count] + (st->iirHAngleR[count] * hFreq)) * (1.0f - hFreq)) + (lowSampleR * hFreq);
            inputSampleR -= (lowSampleR * (1.0f / 21.0f)); // right
        } // the highpass
        st->hBypass = false;
    } else {
        if (!st->hBypass) {
            st->hBypass = true;
            memset(st->iirHPositionL, 0, sizeof(st->iirHPositionL));
            memset(st->iirHAngleL, 0, sizeof(st->iirHAngleL));
            memset(st->iirHPositionR, 0, sizeof(st->iirHPositionR));
            memset(st->iirHAngleR, 0, sizeof(st->iirHAngleR));
        } // blank out highpass if just switched off
    }

    if (lFreq < 1.0f) {
        for (int count = 0; count < 13; count++) {
            st->iirLAngleL[count] = (st->iirLAngleL[count] * (1.0f - lFreq)) + ((inputSampleL - st->iirLPositionL[count]) * lFreq);
            inputSampleL = ((st->iirLPositionL[count] + (st->iirLAngleL[count] * lFreq)) * (1.0f - lFreq)) + (inputSampleL * lFreq);
            st->iirLPositionL[count] = ((st->iirLPositionL[count] + (st->iirLAngleL[count] * lFreq)) * (1.0f - lFreq)) + (inputSampleL * lFreq); // left
            st->iirLAngleR[count] = (st->iirLAngleR[count] * (1.0f - lFreq)) + ((inputSampleR - st->iirLPositionR[count]) * lFreq);
            inputSampleR = ((st->iirLPositionR[count] + (st->iirLAngleR[count] * lFreq)) * (1.0f - lFreq)) + (inputSampleR * lFreq);
            st->iirLPositionR[count] = ((st->iirLPositionR[count] + (st->iirLAngleR[count] * lFreq)) * (1.0f - lFreq)) + (inputSampleR * lFreq); // right
        } // the lowpass
        st->lBypass = false;
    } else {
        if (!st->lBypass) {
            st->lBypass = true;
            memset(st->iirLPositionL, 0, sizeof(st->iirLPositionL));
            memset(st->iirLAngleL, 0, sizeof(st->iirLAngleL));
            memset(st->iirLPositionR, 0, sizeof(st->iirLPositionR));
            memset(st->iirLAngleR, 0, sizeof(st->iirLAngleR));
        } // blank out lowpass if just switched off
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
