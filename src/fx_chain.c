// fx_chain: the FX slot bus -- every FX_TYPE_* effect's own DSP (run_fx_ensemble through
// run_fx_fdnverb), the per-slot state lifecycle (reset_fx_engine/sync_fx_engine_types), and the
// Series/Parallel routing dispatcher (process_fx_chain) that runs them once per sample. Split out
// of backend.c for the same "human-manageability" reason as hds_standalone.c's own split (this is
// roughly a third of backend.c's own line count) -- NOT for any architectural reason: every
// function here already worked purely off FXSlotParams/FXSlotEngine/a sidechain snapshot (verified
// by grep before this move -- zero references to SynthVoice, oscillator state, or g_engine
// anywhere in this file), so there was nothing to untangle, just something to relocate.
//
// #include'd by backend.c at the exact position this code used to occupy inline (right after
// FXSlotEngine's own definition, before FX_TYPE_ENSEMBLE's old position) -- both real binaries
// still see this as part of the SAME translation unit as backend.c itself, exactly like backend.c
// is already #include'd wholesale by both hds_standalone.c and clap_plugin.c. Pure code motion:
// no new cross-file call, nothing lost to inlining, no behavior change -- the two globals declared
// in here (g_hardtune_held_note/g_hardtune_bend_semitones/g_hardtune_modwheel/
// g_hardtune_channel_pressure) are read by handle_note_on/handle_note_off/dispatch_midi_event much
// later in backend.c's own body, exactly as before this split, since this #include still lands at
// the same point in the file relative to everything else.
//
// FXSlotParams/FXSlotEngine themselves, and every FX_TYPE_* enum value, stay in backend.c proper
// (not here) -- they're part of SynthSharedControls's own layout and the OSC control table, used
// by code far outside the FX system itself, not something this file owns.
static void reset_fx_engine(FXSlotEngine engine[FX_SLOT_COUNT]) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) { engine[s].active_type = FX_TYPE_OFF; engine[s].state = NULL; }
}

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

static void run_fx_ensemble(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

static void run_fx_creamcoat(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

static void run_fx_density3(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

static void run_fx_stereochorus(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

static void run_fx_stereodoubler(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

static void run_fx_tapedelay2(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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
    // stronger one exists. Only decoded when the slot's Type is actually this effect, per the
    // "choice" param convention (see FX_TYPE_METADATA's own comment in frontend.py).
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

static void run_fx_pointyguitar(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

static void run_fx_guitarconditioner(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

static void run_fx_cabs2(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

// Reads an entire WAV file into a newly malloc'd interleaved float buffer. Moved here (from
// sequencer.c, its original home) because it now has TWO independent callers that don't share a
// translation unit: sequencer.c's own audio lanes (standalone-only, via hds_standalone.c) and
// FX_TYPE_NAM's cab IR loader just below (needed in every translation unit that runs the FX bus at
// all, including both CLAP plugins) -- fx_chain.c is the one file already #include'd by all three
// (backend.c, hence hds_standalone.c and clap_plugin.c, plus hds_fx_plugin.c directly), so this is
// where a function two of them need independently of each other has to live. Deliberately OUTSIDE
// the `#if HDS_NAM_ENABLED` guard just below -- sequencer.c's own use of it has nothing to do with
// NAM at all, so it must stay available even in a hypothetical build with NAM compiled out.
// Deliberately separate from load_sample_slot() (which only caches a short pre-roll and hands the
// rest to on-demand disk streaming) -- an audio lane's whole clip lives in RAM at once, so this is
// a plain one-shot full-file decode. Keeps every channel present in the file (unlike the sampler's
// mono/stereo-only pre-roll) since the caller, not this loader, should decide how to handle >2
// channels. Returns NULL on any failure (out_frames/out_channels/out_sample_rate left at 0).
static float *seq_audio_load_wav_from_disk(const char *path, int *out_frames, int *out_channels, int *out_sample_rate) {
    *out_frames = 0; *out_channels = 0; *out_sample_rate = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[seq-audio] failed to open '%s': %s\n", path, strerror(errno)); return NULL; }

    char chunk_id[4];
    fseek(f, 12, SEEK_SET);
    if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    while (strncmp(chunk_id, "fmt ", 4) != 0) {
        unsigned int chunk_size;
        if (fread(&chunk_size, 4, 1, f) != 1) { fclose(f); return NULL; }
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    }

    unsigned int subchunk1_size; unsigned short audio_format, num_channels;
    unsigned int sample_rate, byte_rate; unsigned short block_align, bits_per_sample;
    if (fread(&subchunk1_size, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&audio_format, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&num_channels, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&sample_rate, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&byte_rate, 4, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&block_align, 2, 1, f) != 1) { fclose(f); return NULL; }
    if (fread(&bits_per_sample, 2, 1, f) != 1) { fclose(f); return NULL; }
    (void)byte_rate; (void)block_align;

    if (audio_format != 1 && audio_format != 3) {
        fprintf(stderr, "[seq-audio] '%s': unsupported WAV format tag %u (need 1=PCM or 3=float)\n", path, audio_format);
        fclose(f); return NULL;
    }
    if (audio_format == 1 && bits_per_sample != 16 && bits_per_sample != 24 && bits_per_sample != 32) {
        fprintf(stderr, "[seq-audio] '%s': unsupported %u-bit PCM\n", path, bits_per_sample);
        fclose(f); return NULL;
    }
    if (audio_format == 3 && bits_per_sample != 32) {
        fprintf(stderr, "[seq-audio] '%s': float WAV must be 32-bit\n", path);
        fclose(f); return NULL;
    }
    if (num_channels == 0) { fclose(f); return NULL; }
    if (subchunk1_size > 16) fseek(f, subchunk1_size - 16, SEEK_CUR);

    if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    while (strncmp(chunk_id, "data", 4) != 0) {
        unsigned int chunk_size;
        if (fread(&chunk_size, 4, 1, f) != 1) { fclose(f); return NULL; }
        fseek(f, chunk_size, SEEK_CUR);
        if (fread(chunk_id, 1, 4, f) != 4) { fclose(f); return NULL; }
    }
    unsigned int data_size;
    if (fread(&data_size, 4, 1, f) != 1) { fclose(f); return NULL; }

    int bytes_per_sample = bits_per_sample / 8;
    unsigned int total_frames = data_size / (unsigned int)(num_channels * bytes_per_sample);
    if (total_frames == 0) { fclose(f); return NULL; }

    float *pcm = malloc((size_t)total_frames * num_channels * sizeof(float));
    if (!pcm) { fprintf(stderr, "[seq-audio] '%s': out of memory (%u frames x %u ch)\n", path, total_frames, num_channels); fclose(f); return NULL; }

    for (unsigned int i = 0; i < total_frames * num_channels; i++) {
        float sample = 0.0f;
        bool ok = true;
        if (audio_format == 1) {
            if (bits_per_sample == 16) {
                short raw16;
                if (fread(&raw16, 2, 1, f) != 1) ok = false; else sample = (float)raw16 / 32768.0f;
            } else if (bits_per_sample == 24) {
                unsigned char b24[3];
                if (fread(b24, 1, 3, f) != 3) ok = false;
                else { int v = (b24[0] << 8) | (b24[1] << 16) | (b24[2] << 24); sample = (float)v / 2147483648.0f; }
            } else {
                int raw32;
                if (fread(&raw32, 4, 1, f) != 1) ok = false; else sample = (float)raw32 / 2147483648.0f;
            }
        } else {
            if (fread(&sample, 4, 1, f) != 1) ok = false;
        }
        if (!ok) { fprintf(stderr, "[seq-audio] '%s': truncated read at sample %u\n", path, i); free(pcm); fclose(f); return NULL; }
        pcm[i] = sample;
    }
    fclose(f);
    *out_frames = (int)total_frames;
    *out_channels = (int)num_channels;
    *out_sample_rate = (int)sample_rate;
    return pcm;
}

#if HDS_NAM_ENABLED
// --- FX_TYPE_NAM: Neural Amp Modeler (NeuralAmpModelerCore, MIT license) --- see FX_TYPE_NAM's
// own comment above for the param map and why this needed a small C++ bridge (nam_bridge.h/.cpp)
// at all. model_l/model_r are opaque nam::DSP* handles from that bridge -- genuinely two separate
// model instances loaded from the SAME file, never one instance shared between channels, because
// each carries real internal state (WaveNet dilated-conv history, LSTM hidden state) that must not
// leak between L and R the same way this file's other stereo effects (Cabs2's dual iirPosition/
// iirAngle arrays above, PointyGuitar's own per-channel bands) never share state either.
//
// Cab IR convolution is plain C, not more C++: NeuralAmpModelerCore's own wav.h only exposes a raw
// decode helper (load_wav_ir), not a ready convolution engine, and this file's own
// seq_audio_load_wav_from_disk() just above already decodes WAVs -- reusing it here avoids a
// second file-format dependency for the one new thing actually needed: the convolution loop
// itself, a plain per-sample ring-buffer dot product like every other hand-rolled effect in this
// file.
typedef struct {
    void *model_l, *model_r;                 // bridge-owned nam::DSP*, NULL until a model is loaded
    char loaded_model_path[SAMPLE_PATH_LEN];   // "" if none -- compared against fx.nam_model_path
    bool model_load_failed;                    // sticky until the path changes again -- avoids
                                                // retrying a known-bad file every single block

    float *ir_l, *ir_r;                        // coefficients, read-only once loaded -- NULL if no
                                                // cab loaded; ir_r == ir_l for a mono IR (safe to
                                                // alias since neither is ever written after load)
    int ir_taps;
    // Per-channel convolution HISTORY -- unlike ir_l/ir_r above, these are never aliased even for
    // a mono IR: each ring holds that channel's own past INPUT samples, which differ between L and
    // R regardless of whether they share one set of coefficients. Sharing these the way ir_l/ir_r
    // can be shared would have L's and R's samples overwriting each other in the same ring.
    // Each is allocated at 2*ir_taps, not ir_taps -- a doubled/mirrored ring buffer (every write
    // stores the same sample at both `pos` and `pos+ir_taps`) so fx_nam_convolve_tap()'s own hot
    // loop can walk a single contiguous range instead of wrapping the index every tap -- see that
    // function's own comment for why the wrapping version was a real, measured performance problem.
    float *ir_hist_l, *ir_hist_r;
    int ir_hist_pos_l, ir_hist_pos_r;
    char loaded_ir_path[SAMPLE_PATH_LEN];
    bool ir_load_failed;
} FxNamState;

// A direct time-domain FIR: real guitar cab IRs decay within tens of milliseconds, so this cap
// (~85ms at 48kHz) comfortably covers them while bounding the O(taps) per-sample cost this
// approach has -- see poll_fx_nam_requests()'s own comment on why this is loaded on the main
// thread rather than the realtime audio thread despite that cost.
#define FX_NAM_IR_MAX_TAPS 4096

static void fx_nam_free_ir(FxNamState *st) {
    if (st->ir_r && st->ir_r != st->ir_l) free(st->ir_r);
    free(st->ir_l);
    st->ir_l = NULL; st->ir_r = NULL;
    // ir_hist_l/ir_hist_r are always two independent allocations (see the struct's own comment on
    // why they're never aliased even when ir_l/ir_r are) -- both are simply freed, no alias check.
    free(st->ir_hist_l); free(st->ir_hist_r);
    st->ir_hist_l = NULL; st->ir_hist_r = NULL;
    st->ir_taps = 0;
    st->ir_hist_pos_l = 0; st->ir_hist_pos_r = 0;
}

// Releases everything an FxNamState owns OTHER than the struct itself. Called from
// sync_fx_engine_types() right before ITS OWN generic free(fx_engine[s].state) whenever a slot
// changes away from FX_TYPE_NAM -- that generic free() is a plain libc free(), correct for every
// other effect's plain-old-data state struct, but model_l/model_r point at real C++ objects
// (owning Eigen matrices and weight vectors of their own) that a bare free() would leak rather than
// destroy. This function's job is exactly that: run the proper destructor (via the bridge) on
// anything that needs one, so the generic free() right after it only has to reclaim the outer
// struct's own flat memory, same as any other effect's state.
static void fx_nam_free_state(FxNamState *st) {
    hds_nam_destroy(st->model_l);
    hds_nam_destroy(st->model_r);
    fx_nam_free_ir(st);
}

// fx_nam_reload_model/fx_nam_reload_ir/poll_fx_nam_requests -- genuinely shared now, not just
// backend.c's own concern: originally lived in hds_standalone.c (moved there under the mistaken
// assumption that ONLY the standalone binary would ever poll for a path change), which meant a
// CLAP plugin's own FX_TYPE_NAM slots could load a model file at all -- neither hds.clap
// (clap_plugin.c never called poll_fx_nam_requests()) nor a plugin like HDS FX could ever have
// working NAM support, a real, confirmed gap, not a hypothetical one. Moved back here, right next
// to run_fx_nam()/FxNamState/fx_nam_free_state above (their only real home), so every translation
// unit that runs the FX bus -- backend.c (hds_standalone.c/clap_plugin.c) AND hds_fx_plugin.c
// directly -- gets working NAM for free by calling poll_fx_nam_requests() once per block, the same
// way they already all share process_fx_chain()/sync_fx_engine_types(). Takes a plain
// `FXSlotParams fx[FX_SLOT_COUNT]` array directly (not a SynthSharedControls*, which only backend.c
// has) -- the same convention sync_fx_engine_types()/process_fx_chain() already use, precisely so
// hds_fx_plugin.c's own flat pool (no wrapping struct at all) can call it identically.
static void fx_nam_reload_model(FxNamState *st, const char *path) {
    hds_nam_destroy(st->model_l); st->model_l = NULL;
    hds_nam_destroy(st->model_r); st->model_r = NULL;
    snprintf(st->loaded_model_path, SAMPLE_PATH_LEN, "%s", path);
    st->model_load_failed = false;
    if (path[0] == '\0') return; // cleared -- leave both NULL, run_fx_nam() then passes dry
    // num_frames=1 is this call's own real usage pattern (see run_fx_nam()), but Reset() still
    // wants a genuine max-block-size hint for any internal buffer sizing the model does up front.
    st->model_l = hds_nam_create(path, (double)SAMPLE_RATE, 1);
    st->model_r = hds_nam_create(path, (double)SAMPLE_RATE, 1);
    if (!st->model_l || !st->model_r) {
        fprintf(stderr, "[nam] failed to load model '%s'\n", path);
        hds_nam_destroy(st->model_l); st->model_l = NULL;
        hds_nam_destroy(st->model_r); st->model_r = NULL;
        st->model_load_failed = true;
    }
}

static void fx_nam_reload_ir(FxNamState *st, const char *path) {
    fx_nam_free_ir(st);
    snprintf(st->loaded_ir_path, SAMPLE_PATH_LEN, "%s", path);
    st->ir_load_failed = false;
    if (path[0] == '\0') return; // cleared -- leave NULL, run_fx_nam() then skips the cab stage

    int frames = 0, channels = 0, sample_rate = 0;
    float *pcm = seq_audio_load_wav_from_disk(path, &frames, &channels, &sample_rate);
    if (!pcm || frames <= 0) { st->ir_load_failed = true; free(pcm); return; }

    int taps = frames;
    bool truncated = false;
    if (taps > FX_NAM_IR_MAX_TAPS) { taps = FX_NAM_IR_MAX_TAPS; truncated = true; }

    st->ir_l = malloc(sizeof(float) * (size_t)taps);
    bool stereo = channels >= 2;
    st->ir_r = stereo ? malloc(sizeof(float) * (size_t)taps) : st->ir_l;
    if (!st->ir_l || (stereo && !st->ir_r)) {
        fx_nam_free_ir(st); st->ir_load_failed = true; free(pcm); return;
    }
    for (int i = 0; i < taps; i++) {
        st->ir_l[i] = pcm[(size_t)i * channels];
        if (stereo) st->ir_r[i] = pcm[(size_t)i * channels + 1];
    }
    free(pcm);

    st->ir_taps = taps;
    // Always two independent rings, even for a mono IR -- see the struct's own comment on why
    // ir_hist_l/ir_hist_r must never alias the way ir_l/ir_r (read-only coefficients) safely can.
    // 2*taps, not taps -- see the struct's own comment on the doubled-buffer trick.
    st->ir_hist_l = calloc((size_t)taps * 2, sizeof(float));
    st->ir_hist_r = calloc((size_t)taps * 2, sizeof(float));
    st->ir_hist_pos_l = 0; st->ir_hist_pos_r = 0;
    if (!st->ir_hist_l || !st->ir_hist_r) { fx_nam_free_ir(st); st->ir_load_failed = true; return; }

    if (truncated) fprintf(stderr, "[nam] cab IR '%s' truncated to %d taps (was %d)\n", path, taps, frames);
    if (sample_rate != 0 && sample_rate != SAMPLE_RATE) {
        fprintf(stderr, "[nam] cab IR '%s' is %d Hz, engine runs at %d Hz -- loaded as-is, uncorrected\n", path, sample_rate, SAMPLE_RATE);
    }
}

// Same "once-per-block change detection" shape as poll_seq_audio_load_requests() (hds_standalone.c,
// standalone-only -- Sequencer audio lanes are a real, separate, already-documented CLAP gap, this
// one just got fixed). Notices when an FX slot's nam_model_path/nam_ir_path differs from what that
// slot's own state already has loaded, and (re)loads it. Runs the load SYNCHRONOUSLY on whatever
// thread calls it (this plugin's own audio/main thread, same one process() itself runs on for both
// CLAP plugins) rather than spawning a loader thread: a .nam model file is a small, bounded
// JSON+weights blob (nothing like a whole song-length audio recording), and a cab IR is capped at
// FX_NAM_IR_MAX_TAPS frames -- both comfortably finish within one block's worth of stall.
//
// Only meaningful once a slot's own engine state actually exists as an FxNamState -- gated on
// active_type == FX_TYPE_NAM, which sync_fx_engine_types() (the caller's own responsibility to run
// first, same block -- see e.g. hds_fx_plugin_process()'s own call order) guarantees is current for
// any type change already applied by the time this runs; a slot switched to FX_TYPE_NAM within the
// SAME block this runs simply picks up its load on the next block instead, same one-block grace
// every other live-knob check in a per-block poll like this already has.
static void poll_fx_nam_requests(const FXSlotParams fx[FX_SLOT_COUNT], FXSlotEngine fx_engine[FX_SLOT_COUNT]) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        if (fx_engine[s].active_type != FX_TYPE_NAM || !fx_engine[s].state) continue;
        FxNamState *st = (FxNamState *)fx_engine[s].state;
        const char *want_model = fx[s].nam_model_path;
        if (strncmp(want_model, st->loaded_model_path, SAMPLE_PATH_LEN) != 0) {
            fx_nam_reload_model(st, want_model);
        }
        const char *want_ir = fx[s].nam_ir_path;
        if (strncmp(want_ir, st->loaded_ir_path, SAMPLE_PATH_LEN) != 0) {
            fx_nam_reload_ir(st, want_ir);
        }
    }
}

// One tap of the direct-form FIR: writes this sample into the ring, then walks it newest-to-oldest
// against the IR (coeffs[0] is the IR's first/earliest tap, multiplying the sample just written --
// standard y[n] = sum_k h[k]*x[n-k] convolution). `hist` is a DOUBLED ring (2*taps floats, see the
// struct's own comment): this sample is written at both `pos` and `pos+taps`, which lets the walk
// below read the contiguous range hist[pos+1 .. pos+taps] (via the `base[-i]` indexing) instead of
// wrapping the array index every single tap. That wrap-check used to sit inside this loop's own
// body (`idx--; if (idx < 0) idx = taps - 1;`) -- a data-dependent branch on EVERY one of up to
// 4096 taps, run once per output sample, which the "optimize" attribute below needs GONE before
// it can vectorize this loop at all.
// __attribute__((optimize("O3","fast-math"))): this whole file otherwise builds at the project's
// normal -O2 with strict IEEE FP (see Makefile's own CFLAGS) -- this ONE hot loop gets a
// function-scoped exception instead of a global compiler-flag change, so nothing else in this
// codebase's numerical behavior is affected. "fast-math" lets the compiler reassociate the
// summation (required for it to auto-vectorize a reduction at all -- floating-point addition
// isn't associative under strict IEEE rules, so a compiler can't reorder it otherwise); the
// result can differ from a strict left-to-right sum by less than 1ULP per sample, which is
// standard, expected, and inaudible for an FIR convolution -- every real-world audio convolution
// engine relies on exactly this reordering. `restrict` on hist/coeffs (they never alias each
// other) is what actually lets the vectorizer act on that permission. Measured, not assumed:
// baseline (plain -O2, no restrict/fast-math) cost ~0.28x of one core's realtime budget per
// channel at the full 4096-tap cap; this combination measures ~0.08x -- both figures for ONE
// channel of ONE FX_TYPE_NAM slot's cab stage, so a stereo instance (let alone several tracks
// each running one) is the actual multiplier that made this show up as system-wide CPU distress.
__attribute__((optimize("O3","fast-math")))
static inline float fx_nam_convolve_tap(float *restrict hist, int taps, int *pos, const float *restrict coeffs, float in) {
    int p = *pos;
    hist[p] = in;
    hist[p + taps] = in;
    const float *restrict base = hist + p + taps;
    float acc = 0.0f;
    for (int i = 0; i < taps; i++) {
        acc += coeffs[i] * base[-i];
    }
    p += 1; if (p >= taps) p = 0;
    *pos = p;
    return acc;
}

static void run_fx_nam(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxNamState *st = (FxNamState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxNamState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }

    // param[0]/param[1] arrive as the same 0.0-1.0 fraction every other effect's param[] uses
    // (see FX_SLOT_PARAM_COUNT's own comment and e.g. Cabs2/Density3 above) -- this effect's own
    // meaning for that fraction is a +/-20dB trim, linearly mapped (0.5 = 0dB, matching the
    // frontend dial's own default-at-midpoint convention for a centered knob).
    float input_gain_db = (params->param[0] - 0.5f) * 40.0f;
    float output_gain_db = (params->param[1] - 0.5f) * 40.0f;
    float input_gain = powf(10.0f, input_gain_db / 20.0f);
    float output_gain = powf(10.0f, output_gain_db / 20.0f);
    bool cab_enabled = params->param[2] != 0.0f;

    float sl = *l * input_gain, sr = *r * input_gain;
    if (st->model_l && st->model_r) {
        hds_nam_process(st->model_l, &sl, 1);
        hds_nam_process(st->model_r, &sr, 1);
    } // no model loaded (or load failed) -- passes the (gain-adjusted) dry signal straight through

    if (cab_enabled && st->ir_l && st->ir_r) {
        sl = fx_nam_convolve_tap(st->ir_hist_l, st->ir_taps, &st->ir_hist_pos_l, st->ir_l, sl);
        sr = fx_nam_convolve_tap(st->ir_hist_r, st->ir_taps, &st->ir_hist_pos_r, st->ir_r, sr);
    }

    *l = sl * output_gain;
    *r = sr * output_gain;
}
#endif // HDS_NAM_ENABLED

// --- FX_TYPE_GATE: a simple, zero-latency noise gate --- see FX_TYPE_GATE's own comment above for
// the param map. No lookahead buffer anywhere in this function -- the ONLY thing smoothed is the
// gain itself (an envelope follower), never the audio, so nothing here ever delays the signal
// path. Both channels' peak feeds ONE shared envelope/gain value (never independent per-channel
// gains), the same reasoning FX_TYPE_LIMITER/the master bus limiter below both already use: a
// signal panned other than dead-center would otherwise open/close each channel at a slightly
// different moment, audibly skewing the stereo image every time the gate moved at all.
typedef struct {
    float gain; // 0.0 (fully closed, at Range) .. 1.0 (fully open) -- starts at 1.0 (calloc gives
                 // 0.0, which would wrongly gate the very first block's audio before any real
                 // envelope has been established), fixed up on first use below.
    bool initialized;
    float last_gr_db; // -20*log10(gain), clamped >=0 -- this slot's current gain reduction in dB,
                        // read back by process_fx_chain() for the /hds/status/fx_gr/<slot> meter.
} FxGateState;

static void run_fx_gate(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxGateState *st = (FxGateState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxGateState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) { st->gain = 1.0f; st->initialized = true; } // open by default, not closed

    float threshold_db = -60.0f + fmaxf(0.0f, fminf(1.0f, params->param[0])) * 60.0f;
    float attack_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 19.9f;
    float release_ms = 5.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 495.0f;
    float range_db = -80.0f * fmaxf(0.0f, fminf(1.0f, params->param[3]));

    float threshold_lin = powf(10.0f, threshold_db / 20.0f);
    float range_lin = powf(10.0f, range_db / 20.0f);
    float peak = fmaxf(fabsf(*l), fabsf(*r));
    float target_gain = (peak >= threshold_lin) ? 1.0f : range_lin;

    // Same "instant attack, exponential release toward target" shape as the master bus limiter
    // below, just with both directions smoothed here (a gate's own attack must still be a short
    // ramp, not a literal single-sample jump, or opening/closing would click audibly -- "zero-
    // latency" means no signal delay, not an instant, click-prone gain step).
    float rate_ms = (target_gain > st->gain) ? attack_ms : release_ms;
    float rate = 1.0f - expf(-1.0f / (0.001f * rate_ms * (float)SAMPLE_RATE));
    st->gain += (target_gain - st->gain) * rate;

    *l *= st->gain;
    *r *= st->gain;
    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, st->gain)));
}

// --- FX_TYPE_EQ3: the Program's own post-filter 3-band EQ, ported to an FX slot --- see
// FX_TYPE_EQ3's own comment above for the param map. Reuses compute_eq_band_coeffs()/
// run_eq_biquad() (see their own comments, far above) completely unchanged -- this is genuinely
// the SAME EQ, not a re-derivation of it, so it can never behave differently from the Program EQ
// version for the same Freq/Gain/Q settings.
typedef struct {
    float z1[3][2], z2[3][2]; // [band][channel] -- band 0=Low Shelf, 1=Mid Peak, 2=High Shelf
} FxEq3State;

// 0.0-1.0 fraction -> 20Hz-20kHz, log-spaced (equal knob distance = equal perceived pitch change,
// same musical mapping a hardware EQ's own frequency knob uses) -- needed because, unlike the
// Program EQ's own dedicated frontend dial (which is hardcoded directly to a 20-20000 Hz range,
// see frontend.py's own eq_freq_dials), this FX slot's Freq params are two more generic P1-P10
// dials like every other effect's params, which always transmit as a plain 0.0-1.0 fraction (see
// FX_TYPE_METADATA's own comment in frontend.py) -- decoding that fraction into Hz is this
// function's job, not the frontend's, matching how every other unit conversion in this file (dB,
// ms, Q, ...) already happens backend-side.
static inline float fx_eq3_fraction_to_hz(float fraction) {
    fraction = fmaxf(0.0f, fminf(1.0f, fraction));
    return 20.0f * powf(1000.0f, fraction); // 20 * 1000^0 = 20Hz .. 20 * 1000^1 = 20000Hz
}

static void run_fx_eq3(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxEq3State *st = (FxEq3State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxEq3State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }

    EqBandParams bands[3];
    bands[0].freq = fx_eq3_fraction_to_hz(params->param[0]);
    bands[0].gain_db = -18.0f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 36.0f;
    bands[0].q = 0.707f; // ignored for shelf bands -- see compute_eq_band_coeffs()'s own comment
    bands[1].freq = fx_eq3_fraction_to_hz(params->param[2]);
    bands[1].gain_db = -18.0f + fmaxf(0.0f, fminf(1.0f, params->param[3])) * 36.0f;
    bands[1].q = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 9.9f;
    bands[2].freq = fx_eq3_fraction_to_hz(params->param[5]);
    bands[2].gain_db = -18.0f + fmaxf(0.0f, fminf(1.0f, params->param[6])) * 36.0f;
    bands[2].q = 0.707f; // ignored for shelf bands

    float sl = *l, sr = *r;
    for (int band = 0; band < 3; band++) {
        EqBiquadCoeffs c = compute_eq_band_coeffs(&bands[band], band);
        sl = run_eq_biquad(sl, &c, &st->z1[band][0], &st->z2[band][0]);
        sr = run_eq_biquad(sr, &c, &st->z1[band][1], &st->z2[band][1]);
    }
    *l = sl;
    *r = sr;
}

// --- FX_TYPE_SSL4K -- see its own comment in fx_types.h for the param map and design summary. ---

// 0.0-1.0 fraction -> [lo_hz, hi_hz], log-spaced -- same musical-taper reasoning as
// fx_eq3_fraction_to_hz() above, just with each band's own frequency range instead of one fixed
// 20-20000Hz span (the console's own bands each only sweep a fraction of the full audio range).
static inline float ssl4k_fraction_to_hz(float fraction, float lo_hz, float hi_hz) {
    fraction = fmaxf(0.0f, fminf(1.0f, fraction));
    return lo_hz * powf(hi_hz / lo_hz, fraction);
}

typedef struct {
    float z1[4][2], z2[4][2]; // [band][channel] -- 0=LF Shelf, 1=LMF Bell, 2=HMF Bell, 3=HF Shelf
    EqBiquadCoeffs hpf_coeffs;
    float hpf_z1[2], hpf_z2[2];
} FxSsl4kState;

// 2-pole (12dB/oct) Butterworth high-pass, Q=0.7071 (maximally flat passband, the standard HPF
// alignment) -- same RBJ-cookbook HPF form already used for FairKid67's own output-transformer
// coloration (fk67_xfmr2_init's own hpf_coeffs, just above), reused here as a plain audible HPF
// rather than a fixed sub-audio corner.
static void ssl4k_compute_hpf_coeffs(EqBiquadCoeffs *c, float freq_hz) {
    const float fs = (float)SAMPLE_RATE;
    const float w0 = 2.0f * (float)M_PI * fmaxf(20.0f, fminf(2000.0f, freq_hz)) / fs;
    const float alpha = sinf(w0) / (2.0f * 0.70710678f);
    const float cosw0 = cosf(w0);
    const float a0 = 1.0f + alpha;
    c->b0 = ((1.0f + cosw0) * 0.5f) / a0;
    c->b1 = -(1.0f + cosw0) / a0;
    c->b2 = c->b0;
    c->a1 = (-2.0f * cosw0) / a0;
    c->a2 = (1.0f - alpha) / a0;
}

static void run_fx_ssl4k(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxSsl4kState *st = (FxSsl4kState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxSsl4kState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }

    // Console (E/G) -- see fx_types.h's own comment: this selects the LMF/HMF top-of-range
    // (G sweeps noticeably higher on both, per its own reputation as the "extended range,
    // brighter/smoother" revision) and how aggressively gain narrows a bell band's own bandwidth
    // and drives the saturation stage (E: punchier/more aggressive on both counts).
    const bool console_g = (int)(fmaxf(0.0f, fminf(1.0f, params->param[10])) * 1.0f + 0.5f) != 0;
    const float lmf_hi_hz = console_g ? 7000.0f : 2500.0f;
    const float hmf_hi_hz = console_g ? 12000.0f : 7000.0f;
    const float q_narrow_rate = console_g ? 0.02f : 0.05f;
    const float sat_drive_rate = console_g ? 0.015f : 0.03f;

    EqBandParams bands[4];
    bands[0].freq = ssl4k_fraction_to_hz(params->param[1], 30.0f, 450.0f);
    bands[0].gain_db = -16.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 32.0f;
    bands[0].q = 0.707f; // ignored for shelf bands -- see compute_eq_band_coeffs()'s own comment

    bands[1].freq = ssl4k_fraction_to_hz(params->param[3], 200.0f, lmf_hi_hz);
    bands[1].gain_db = -16.0f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 32.0f;
    const float lmf_q_base = 0.3f + fmaxf(0.0f, fminf(1.0f, params->param[5])) * 2.7f;

    bands[2].freq = ssl4k_fraction_to_hz(params->param[6], 600.0f, hmf_hi_hz);
    bands[2].gain_db = -16.0f + fmaxf(0.0f, fminf(1.0f, params->param[7])) * 32.0f;
    const float hmf_q_base = 0.7f; // no dedicated Q knob for this band (matches the real console's own HMF, which is fixed-bandwidth on several revisions) -- still narrows/widens dynamically with gain below

    bands[3].freq = ssl4k_fraction_to_hz(params->param[8], 1500.0f, 16000.0f);
    bands[3].gain_db = -16.0f + fmaxf(0.0f, fminf(1.0f, params->param[9])) * 32.0f;
    bands[3].q = 0.707f; // ignored for shelf bands

    // Proportional/"dynamic" bandwidth: as a bell band's own |gain| rises, its Q narrows --
    // a broad boost/cut at low gain, a surgical one at high gain, the "musical", self-scaling
    // bandwidth behavior this whole console style is known for (as opposed to a plain fixed-Q
    // parametric EQ, where the same Q sounds proportionally wider at low gain and narrower at high
    // gain purely as a side effect of the gain itself, with no compensation). This own curve/rate
    // is an original choice here, not measured or reverse-engineered from any specific unit.
    bands[1].q = fmaxf(0.15f, lmf_q_base / (1.0f + q_narrow_rate * fabsf(bands[1].gain_db)));
    bands[2].q = fmaxf(0.15f, hmf_q_base / (1.0f + q_narrow_rate * fabsf(bands[2].gain_db)));

    ssl4k_compute_hpf_coeffs(&st->hpf_coeffs, ssl4k_fraction_to_hz(params->param[0], 20.0f, 500.0f));

    // bands[] index -> compute_eq_band_coeffs()'s own band_index (0=Low Shelf, 1=Mid Peak,
    // 2=High Shelf): LF is a Low Shelf, LMF/HMF are both Mid Peak (bell) bands, HF is a High Shelf.
    static const int SSL4K_EQ_BAND_INDEX[4] = {0, 1, 1, 2};
    float sl = run_eq_biquad(*l, &st->hpf_coeffs, &st->hpf_z1[0], &st->hpf_z2[0]);
    float sr = run_eq_biquad(*r, &st->hpf_coeffs, &st->hpf_z1[1], &st->hpf_z2[1]);
    for (int band = 0; band < 4; band++) {
        EqBiquadCoeffs c = compute_eq_band_coeffs(&bands[band], SSL4K_EQ_BAND_INDEX[band]);
        sl = run_eq_biquad(sl, &c, &st->z1[band][0], &st->z2[band][0]);
        sr = run_eq_biquad(sr, &c, &st->z1[band][1], &st->z2[band][1]);
    }

    // Always-on, subtle saturation standing in for the real console's own transformer/op-amp
    // stage -- a normalized tanh soft-clip (drive in, 1/tanh(drive) back out keeps it unity-gain
    // at DC/low level, so this never quietly pads the signal), whose drive scales with how hard
    // this slot's own bands are being pushed (the sum of every band's own |gain_db|, on top of a
    // small fixed baseline so even a flat/0dB setting still imparts a touch of console character).
    // Purely an original stand-in for a general, widely-described trait of this console style, not
    // a measured/reverse-engineered circuit model of any specific unit.
    const float total_boost_db = fabsf(bands[0].gain_db) + fabsf(bands[1].gain_db) + fabsf(bands[2].gain_db) + fabsf(bands[3].gain_db);
    const float drive = 1.015f + sat_drive_rate * total_boost_db;
    const float inv_tanh_drive = 1.0f / tanhf(drive);
    *l = tanhf(drive * sl) * inv_tanh_drive;
    *r = tanhf(drive * sr) * inv_tanh_drive;
}

// --- FX_TYPE_LIMITER: a digital brick-wall peak limiter --- see FX_TYPE_LIMITER's own comment
// above for the param map and why this is an original implementation of the same technique the
// master bus safety limiter below already uses, not a literal port of anything. Both channels
// always share one gain value, same reasoning as FX_TYPE_GATE above -- the whole point of a
// limiter is protecting levels without being audible as anything OTHER than "it got a little
// quieter for a moment," and an independent per-channel gain would smear the stereo image on
// every single gain-reduction event, not just an occasional one.
typedef struct {
    float gain; // starts at 1.0 -- see FxGateState's own comment on why calloc's 0.0 default would
                 // be wrong here too (a fully-closed limiter on the very first block).
    bool initialized;
    float last_gr_db; // -20*log10(gain), clamped >=0 -- see FxGateState.last_gr_db's own comment.
} FxLimiterState;

static void run_fx_limiter(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxLimiterState *st = (FxLimiterState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxLimiterState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) { st->gain = 1.0f; st->initialized = true; }

    float ceiling_db = -20.0f + fmaxf(0.0f, fminf(1.0f, params->param[0])) * 20.0f;
    float input_gain_db = fmaxf(0.0f, fminf(1.0f, params->param[1])) * 24.0f;
    float release_ms = 10.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 490.0f;

    float ceiling = powf(10.0f, ceiling_db / 20.0f);
    float input_gain = powf(10.0f, input_gain_db / 20.0f);
    float sl = *l * input_gain, sr = *r * input_gain;

    // Identical shape to the master bus limiter further down (see its own comment for the full
    // rationale) -- instant attack ("this sample can never exceed ceiling" is only true if the
    // gain reduction is computed and applied to the SAME sample it's measuring, not a future one),
    // exponential release so the reduction isn't audible as pumping on an isolated transient.
    float peak = fmaxf(fabsf(sl), fabsf(sr));
    if (peak > ceiling || st->gain != 1.0f) {
        float needed_gain = (peak > ceiling) ? (ceiling / peak) : 1.0f;
        if (needed_gain < st->gain) {
            st->gain = needed_gain; // attack: instant
        } else {
            float release_rate = 1.0f - expf(-1.0f / (0.001f * release_ms * (float)SAMPLE_RATE));
            st->gain += (needed_gain - st->gain) * release_rate;
        }
        sl *= st->gain;
        sr *= st->gain;
    }
    *l = sl;
    *r = sr;
    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, st->gain)));
}

// --- FX_TYPE_FETCOMP: ported from fetcomp-dsp (github.com/Paulllux/fetcomp-dsp, MIT license) --- see
// FX_TYPE_FETCOMP's own comment above for the param map. This is a genuinely large port (a real
// circuit-derived model, not a simple filter), so this comment records exactly what's kept
// faithful and what's deliberately changed from the original FetLimiterDsp.h:
//
// KEPT FAITHFUL: the gain-cell quadratic solve, the diode detector law, the two-node attack/
// release timing network (including its log/pow-domain calibration curves -- these fit the real
// measured behavior of the timing network itself, not a physical-potentiometer's rotation feel,
// so they apply just as much to a plain 0-100 dial as to the original's own knob), the ratio bank's
// per-position drive/bias constants, the Class A tanh output stage, and the full Jiles-Atherton
// hysteresis transformer model (including the langevinPair() precision workaround -- that's a
// property of the ANALYTIC FORMULA's own catastrophic cancellation near zero, true regardless of
// which tanh implementation computes it).
//
// DELIBERATELY CHANGED:
//   1. juce::jmin/jmax/jlimit/jmap -> fminf/fmaxf; juce::Decibels -> powf(10,db/20); JUCE is gone
//      entirely -- this whole port exists because the original needs it only for these utilities,
//      a buffer wrapper, and the oversampler below, none of which need a whole framework.
//   2. The original's own "fast" tanh/exp2 polynomial approximations (dsp_math::fastTanh/fastExp2)
//      are replaced with real libm tanhf/expf. Strictly more accurate, and it removes the
//      overflow-to-NaN failure mode the original's own RailVolts clamp comment describes guarding
//      against (a real tanhf saturates cleanly at +/-1 for a large argument; it cannot overflow).
//   3. Oversampling: the original runs a runtime-switchable JUCE polyphase halfband IIR oversampler
//      (1x/2x/4x). This port fixes a single 2x factor using one RBJ-cookbook Butterworth lowpass
//      biquad on the way up (zero-stuff + filter + 2x gain) and a matching one on the way down
//      (filter + decimate) -- see run_fx_fetcomp() itself. Less stopband rejection than a real
//      polyphase halfband, but self-contained in ~10 lines against this file's own existing
//      biquad runner, and more than enough to tame the aliasing a 20us attack's control-signal
//      energy would otherwise fold back into the audible band.
//   4. txModel/relModel are fixed at the original author's own "verdict Aug 2026" shipping
//      defaults (physics-corrected iron, the fitted two-node release network) rather than exposed
//      as runtime-switchable bench A/B toggles -- those exist upstream for the *author's own*
//      shipped-vs-experimental comparison, not as a user-facing control. Likewise DiodeKneeV stays
//      at its own shipping default of 0 (the "exact comparator law" -- the original's own comments
//      mark the nonzero logistic-gate alternative "bench experiments ONLY: three formulations all
//      measured worse than the UAD"), which drops the softplus/logistic discharge-gate branch
//      entirely -- dead code at that default in the original too.
//   5. idss/pinchOffV ("selected-part latitude", i.e. simulating a hand-picked FET) stay fixed at
//      the original's own "typical small-signal n-JFET" defaults rather than exposed as user
//      params -- a real per-unit trimming knob, not something a software knob should expose.
//   6. No internal Mix/parallel-blend param -- the slot's own generic Mix knob already does that
//      job, same convention as every other ported effect in this file.
//
// The gain cell's own detector/control-voltage loop is SHARED between L/R (one envelope, driven by
// max(|L|,|R|)), exactly like the original -- an independent envelope per channel would let the
// louder channel duck the quieter one out from under it and the stereo image would wander with the
// programme material, which is precisely why the real hardware's own stereo-link mode exists.
typedef struct { float h, m, lp; } FetCompTxState;
typedef struct { float z; } FetCompHpState;

typedef struct {
    // Fixed coefficients, computed once when this slot's engine first activates. Fixed because
    // idss/pinchOffV/txModel/relModel/DiodeKneeV are all fixed choices (see this effect's own
    // top-of-section comment) -- none of these ever change again for the life of this state.
    float hp_g;                                    // DC-blocking highpass (TPT one-pole)
    float two_beta;                                 // JFET transconductance-derived constant
    float class_a_bias_out;                         // tanh(ClassABias), Class A's DC-cancel term
    float tx_decay, tx_leak_coef, tx_src_b, tx_b_trim;
    float tx_a, tx_c, tx_alpha, tx_core_asym, tx_k;
    float inv_tx_a, one_minus_c, one_minus_ck, c_alpha;
    float rel_couple_a, rel_charge_b;                // two-node release network coupling (fixed)
    EqBiquadCoeffs os_coeffs;                        // shared 2x oversampling lowpass (see above)
    float smooth_coef;                               // input/output gain smoothing, at 2x rate

    // Live state, updated every (oversampled) sample.
    float env, env_res, env_discharge_gate, ctrl_volts; // SHARED across L/R -- see this section's
                                                         // own top comment
    FetCompTxState tx[2];                               // per-channel output transformer state
    FetCompHpState hp[2];                                // per-channel DC-blocking highpass state
    float os_up_z1[2], os_up_z2[2];                   // per-channel 2x-upsample filter state
    float os_down_z1[2], os_down_z2[2];               // per-channel 2x-downsample filter state
    float smoothed_in, smoothed_out;                  // ramped input/output gain (linear)
    float last_gr_db; // the gain cell's own current reduction: at ctrl_volts=0 (quiescent) the
                        // closed-form solve's small-signal gain is exactly 1 (see fetcomp_solve_cell's
                        // own comment), so unlike FairKid67 below this needs no separate quiescent
                        // probe -- 20*log10(1+R5*G0) directly gives the reduction relative to that
                        // unity baseline. Read back by process_fx_chain() for the GR meter.
} FxFetCompState;

#define FETCOMP_R5 27000.0f               // gain-cell series resistor, from the Rev D drawing
#define FETCOMP_CELL_VOLTS_PER_UNIT 0.03f // 0dBFS -> volts at the FET (keeps the cell in its linear region)
#define FETCOMP_INV_CELL_VOLTS (1.0f / FETCOMP_CELL_VOLTS_PER_UNIT)
#define FETCOMP_RAIL_VOLTS 40.0f          // the Class A stage cannot swing past its own supply rails
#define FETCOMP_CLASS_A_BIAS (-0.18f)
#define FETCOMP_MAX_ENVELOPE 4.0f
#define FETCOMP_MAX_CTRL_VOLTS 2.0f
#define FETCOMP_DETECTOR_INV_VT 180.0f
#define FETCOMP_DETECTOR_K 0.10f

static void compute_fetcomp_os_lowpass_coeffs(EqBiquadCoeffs *c, float os_rate) {
    const float freq = 21000.0f, q = 0.70710678f; // Butterworth Q, cutoff just under the base
                                                   // Nyquist -- see this effect's own comment on
                                                   // why this replaces the original's polyphase
                                                   // halfband oversampler
    const float w0 = 2.0f * (float)M_PI * freq / os_rate;
    const float alpha = sinf(w0) / (2.0f * q);
    const float cosw0 = cosf(w0);
    const float a0 = 1.0f + alpha;
    c->b0 = ((1.0f - cosw0) * 0.5f) / a0;
    c->b1 = (1.0f - cosw0) / a0;
    c->b2 = c->b0;
    c->a1 = (-2.0f * cosw0) / a0;
    c->a2 = (1.0f - alpha) / a0;
}

static void fetcomp_init(FxFetCompState *st) {
    const double os_rate = (double)SAMPLE_RATE * 2.0;

    const double g = tan(M_PI * 8.0 / fmax(1.0, os_rate)); // CouplingHz = 8Hz
    st->hp_g = (float)(g / (1.0 + g));

    const float idss = 0.010f, pinch_off_v = -3.0f; // "typical small-signal n-JFET" (see comment)
    const float beta = idss / (pinch_off_v * pinch_off_v);
    st->two_beta = 2.0f * beta;

    st->class_a_bias_out = tanhf(FETCOMP_CLASS_A_BIAS);

    const double w_lf = 2.0 * M_PI * 3.0 / fmax(1.0, os_rate); // TxLowCornerHz = 3Hz
    st->tx_decay = (float)(1.0 - w_lf);
    st->tx_leak_coef = (float)(1.0 - exp(-2.0 * M_PI * 32000.0 / fmax(1.0, os_rate))); // TxLeakageHz
    st->tx_src_b = 0.5f;  // TxSrcB
    st->tx_b_trim = 1.95f; // TxBTrim

    st->tx_a = 0.063f;       // TxCoreA
    st->tx_c = 0.17f;        // TxCoreC
    st->tx_alpha = 0.0016f;  // TxCoreAlpha
    st->tx_core_asym = 0.3f; // TxAsymB (physics-corrected iron model, always used -- see comment)
    st->tx_k = 0.040f;       // TxCoreK * TxKScaleB

    st->inv_tx_a = 1.0f / st->tx_a;
    st->one_minus_c = 1.0f - st->tx_c;
    st->one_minus_ck = st->one_minus_c * st->tx_k;
    st->c_alpha = st->tx_c * st->tx_alpha;

    st->rel_couple_a = (float)(1.0 - exp(-1.0 / (0.160 * os_rate))); // RelCoupleTauMs = 160ms
    st->rel_charge_b = st->rel_couple_a; // RelCapRatio = 1.0 -- both nodes share one time constant

    compute_fetcomp_os_lowpass_coeffs(&st->os_coeffs, (float)os_rate);
    st->smooth_coef = (float)(1.0 - exp(-1.0 / (0.02 * os_rate))); // GainSmoothingSeconds = 20ms

    st->smoothed_in = -1.0f; // sentinel: primed to the first target value on the very first call
}

static inline float fetcomp_highpass(float x, FetCompHpState *s, float g) {
    const float v = (x - s->z) * g;
    const float lp = v + s->z;
    s->z = lp + v;
    if (!isfinite(s->z)) s->z = 0.0f;
    return x - lp;
}

// The closed-form divider solution (see FX_TYPE_FETCOMP's own comment for the circuit derivation):
//     R5*k*v^2 + (1 + R5*G0)*v - x = 0
static inline float fetcomp_solve_cell(float x, float G0, float k) {
    const float b = 1.0f + FETCOMP_R5 * G0;
    if (fabsf(k) < 1.0e-12f) return x / fmaxf(1.0e-6f, b);
    const float a = FETCOMP_R5 * k;
    const float disc = b * b + 4.0f * a * x;
    if (disc <= 0.0f) return -b / (2.0f * a);
    return (-b + sqrtf(disc)) / (2.0f * a);
}

static inline float fetcomp_class_a(float x, float drive, float bias_out, float norm) {
    const float xc = fmaxf(-FETCOMP_RAIL_VOLTS, fminf(FETCOMP_RAIL_VOLTS, x));
    return (tanhf(drive * xc + FETCOMP_CLASS_A_BIAS) - bias_out) * norm;
}

// Langevin function and its derivative. The |x|>0.5 threshold (rather than the ~1e-4 the
// literature quotes) is load-bearing precision: at a much smaller x, 1/x^2 and coth^2(x) are both
// huge and nearly equal, and subtracting them in float loses almost everything to cancellation --
// a property of this ANALYTIC FORMULA, true no matter which tanh implementation computes coth.
static inline void fetcomp_langevin_pair(float x, float *L, float *Lp) {
    if (fabsf(x) > 0.5f) {
        const float ct = 1.0f / tanhf(x);
        *L = ct - 1.0f / x;
        *Lp = 1.0f / (x * x) - ct * ct + 1.0f;
        return;
    }
    const float x2 = x * x;
    *L = x * (1.0f / 3.0f - x2 * (1.0f / 45.0f - x2 * (2.0f / 945.0f)));
    *Lp = 1.0f / 3.0f - x2 * (1.0f / 15.0f - x2 * (2.0f / 189.0f));
}

// The Jiles-Atherton ODE (returns dM/dH_dot -- multiply by Hdot for dM/dt). Hysteresis is what
// gives the transformer stage real loop area/remanence; the core asymmetry term (txCoreAsym) is
// the only source of even harmonics here, modeling the single-ended Class A stage ahead of it
// biasing the core off-center.
static inline float fetcomp_jiles_atherton_slope(const FxFetCompState *st, float M, float H, float Hdot) {
    const float qRaw = (H + st->tx_alpha * M) * st->inv_tx_a;
    const float aEff = 1.0f + st->tx_core_asym * tanhf(fmaxf(-FETCOMP_RAIL_VOLTS, fminf(FETCOMP_RAIL_VOLTS, qRaw)));
    const float invAEff = 1.0f / aEff;
    const float Q = qRaw * invAEff;
    float Man, dManRaw;
    fetcomp_langevin_pair(Q, &Man, &dManRaw);
    const float dMan = dManRaw * st->inv_tx_a * invAEff;
    const float diff = Man - M;
    const float delta = (Hdot >= 0.0f) ? 1.0f : -1.0f;
    const float deltaM = ((delta > 0.0f) == (diff > 0.0f)) ? 1.0f : 0.0f;
    float den = st->one_minus_ck * delta - st->tx_alpha * diff;
    if (fabsf(den) < 1.0e-6f) den = (den < 0.0f) ? -1.0e-6f : 1.0e-6f;
    const float irr = st->one_minus_c * deltaM * diff / den;
    const float rev = st->tx_c * dMan;
    const float scale = 1.0f - st->c_alpha * dMan;
    return (irr + rev) / ((fabsf(scale) < 1.0e-6f) ? 1.0e-6f : scale);
}

// Output transformer: flux is the integral of the winding voltage (that's why a 50Hz signal drives
// far more core excursion than a 5kHz one at the same level -- the whole reason a static
// waveshaper can't stand in for iron). "y" (not the J-A output) is the actual signal fed to the
// leak-inductance one-pole -- H/M's own integration only feeds back into the NEXT sample's
// impedance drop, matching the original's own physics-corrected topology exactly.
static inline float fetcomp_transformer(const FxFetCompState *st, float x, FetCompTxState *s, float tx_flux_gain) {
    const float y = (x - st->tx_src_b * s->h) * st->tx_b_trim;
    const float dB = y * tx_flux_gain;
    const float s2b = fetcomp_jiles_atherton_slope(st, s->m, s->h, dB);
    const float dH = dB / fmaxf(0.05f, 1.0f + s2b);
    s->h += dH;
    s->m += s2b * dH;
    if (!isfinite(s->h)) s->h = 0.0f;
    if (!isfinite(s->m)) s->m = 0.0f;
    s->h *= st->tx_decay;
    s->m = fmaxf(-4.0f, fminf(4.0f, s->m));
    s->lp += (y - s->lp) * st->tx_leak_coef;
    if (!isfinite(s->lp)) s->lp = 0.0f;
    return s->lp;
}

// One (oversampled) sample's worth of gain cell + detector/timing network + output stage, both
// channels at once (the detector/control loop is shared -- see this section's own top comment).
static void fetcomp_process_subsample(FxFetCompState *st, float xL, float xR,
                                    float cell_k, float class_a_drive, float class_a_norm,
                                    float tx_flux_gain, float attack_step, float attack_rc_coef,
                                    float release_coef, float ratio_drive, float ratio_bias_v,
                                    float env_bias, bool limiting, float *outL, float *outR) {
    const float x  = fetcomp_highpass(xL * st->smoothed_in, &st->hp[0], st->hp_g);
    const float xr = fetcomp_highpass(xR * st->smoothed_in, &st->hp[1], st->hp_g);

    // Feedback: this sample's G0 comes from ctrl_volts computed at the END of the PREVIOUS
    // sample -- a one-sample loop delay, which is what keeps a digital feedback compressor stable.
    const float G0 = st->two_beta * st->ctrl_volts;
    const float v  = fetcomp_solve_cell(x  * FETCOMP_CELL_VOLTS_PER_UNIT, G0, cell_k) * FETCOMP_INV_CELL_VOLTS;
    const float vr = fetcomp_solve_cell(xr * FETCOMP_CELL_VOLTS_PER_UNIT, G0, cell_k) * FETCOMP_INV_CELL_VOLTS;

    const float rect = fmaxf(fabsf(v), fabsf(vr)); // sidechain, tapped AFTER the cell

    if (!limiting) {
        // The real unit's Attack-knob OFF detent: cell stays open (no gain reduction), signal
        // still runs the whole path (divider, Class A, transformer), so the coloration stays.
        st->env = 0.0f;
        st->env_res = 0.0f;
    } else {
        // Attack is slew-limited, not exponential -- see FX_TYPE_FETCOMP's own comment on why this
        // is what makes the Attack dial mean anything in a feedback topology.
        const float over = rect - st->env;
        if (over > 0.0f) st->env += fminf(attack_step, over * attack_rc_coef);
        st->env_discharge_gate = (over > 0.0f) ? 0.0f : 1.0f; // shipping comparator law (DiodeKneeV=0)

        // Release: exponential discharge toward the Q-bias point.
        const float dis = (env_bias - st->env) * release_coef * st->env_discharge_gate;
        st->env += dis;
    }

    // Two-node timing network: the release pot drains a fast node; the reservoir couples through
    // a fixed resistance and has no path of its own -- this is what sheds a transient bump's gain
    // reduction far faster than the same network's own sustained-material release time.
    st->env     += (st->env_res - st->env) * st->rel_couple_a;
    st->env_res += (st->env - st->env_res) * st->rel_charge_b;

    if (!isfinite(st->env)) st->env = 0.0f;
    if (!isfinite(st->env_res)) st->env_res = 0.0f;
    st->env     = fmaxf(0.0f, fminf(FETCOMP_MAX_ENVELOPE, st->env));
    st->env_res = fmaxf(0.0f, fminf(FETCOMP_MAX_ENVELOPE, st->env_res));

    // Detector law: a diode (exp(x)-1 gives zero output below the bias for free).
    const float arg = fmaxf(-30.0f, fminf(20.0f, (ratio_drive * st->env - ratio_bias_v) * FETCOMP_DETECTOR_INV_VT));
    st->ctrl_volts = fmaxf(0.0f, fminf(FETCOMP_MAX_CTRL_VOLTS, FETCOMP_DETECTOR_K * (expf(arg) - 1.0f)));

    // Output section, following the hardware's own signal order: gain cell, Class A line amp,
    // Output control, transformer.
    const float stage_l = fetcomp_class_a(v,  class_a_drive, st->class_a_bias_out, class_a_norm) * st->smoothed_out;
    const float stage_r = fetcomp_class_a(vr, class_a_drive, st->class_a_bias_out, class_a_norm) * st->smoothed_out;
    *outL = fetcomp_transformer(st, stage_l, &st->tx[0], tx_flux_gain);
    *outR = fetcomp_transformer(st, stage_r, &st->tx[1], tx_flux_gain);
}

static void run_fx_fetcomp(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxFetCompState *st = (FxFetCompState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxFetCompState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        fetcomp_init(st);
    }

    const float p0 = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    const float p1 = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    const float p2 = fmaxf(0.0f, fminf(1.0f, params->param[2]));
    const float p3 = fmaxf(0.0f, fminf(1.0f, params->param[3]));
    const float p4 = fmaxf(0.0f, fminf(1.0f, params->param[4]));
    const bool limiting = params->param[5] < 0.5f; // choice: 0=Limit, 1=Character Only
    const float p6 = fmaxf(0.0f, fminf(1.0f, params->param[6]));
    const float p7 = fmaxf(0.0f, fminf(1.0f, params->param[7]));

    const float input_db  = -20.0f + p0 * 60.0f; // -20..+40
    const float output_db = -20.0f + p1 * 40.0f; // -20..+20
    int ratio_idx = (int)(p2 * 4.0f + 0.5f);     // 0..4 -> 4:1/8:1/12:1/20:1/All
    if (ratio_idx < 0) ratio_idx = 0; else if (ratio_idx > 4) ratio_idx = 4;
    const float attack_us  = 20.0f + p3 * (800.0f - 20.0f);   // fastest at 0, slowest at 1
    const float release_ms = 50.0f + p4 * (1100.0f - 50.0f);  // fastest at 0, slowest at 1
    const float linearisation = p6 * 0.5f; // 0..0.5, LN feedback fraction
    const float iron_amount = p7;          // 0..1, Class A/transformer drive

    const double os_rate = (double)SAMPLE_RATE * 2.0;

    const float target_in  = powf(10.0f, input_db  / 20.0f);
    const float target_out = powf(10.0f, output_db / 20.0f);
    if (st->smoothed_in < 0.0f) { st->smoothed_in = target_in; st->smoothed_out = target_out; }

    const float cell_k = st->two_beta * 0.5f * (2.0f * linearisation - 1.0f);

    // Ratio bank: each position's own drive/bias resistor-tap constants (from the Rev D drawing)
    // times a per-position "pole B" trim (solved against measured hardware behavior -- see
    // FX_TYPE_FETCOMP's own comment; index 4 = All, which shorts every tap in parallel).
    static const float fit_drive[5]    = { 0.166f, 0.207f, 0.275f, 0.456f, 0.456f };
    static const float fit_bias_r[5]   = { 150.0f, 620.0f, 1180.0f, 2680.0f, 150.0f };
    static const float pole_b_drive[5] = { 1.7507f, 2.5986f, 2.9198f, 3.2227f, 3.3415f };
    static const float pole_b_bias[5]  = { 2.5505f, 1.3858f, 1.2092f, 1.1407f, 20.7546f };
    const float ratio_drive  = fit_drive[ratio_idx] * pole_b_drive[ratio_idx];
    const float ratio_bias_v = fit_bias_r[ratio_idx] * 3.3e-5f * pole_b_bias[ratio_idx];
    const float env_bias     = ratio_bias_v / fmaxf(1.0e-6f, ratio_drive);

    const float class_a_drive = 0.14f * fmaxf(1.0e-4f, iron_amount);
    const float class_a_norm  = 1.0f / (class_a_drive * (1.0f - st->class_a_bias_out * st->class_a_bias_out));
    const float tx_flux_gain  = 0.2f * iron_amount * (float)(2.0 * M_PI * 50.0 / os_rate);

    // Attack/release timing: log-domain two-segment curves through a measured mid-anchor point --
    // fitted to the real network's own step-response trajectories, not a UI pot taper (see this
    // section's own top comment) -- so this applies to attack_us/release_ms as decoded above
    // exactly as it would to a value that came off a physical dial.
    const double atk_nom = (double)attack_us * 1.0e-6;
    const double atk_frac = fmax(0.0, fmin(1.0, log(atk_nom / 20.0e-6) / log(800.0 / 20.0)));
    const double atk_mid = sqrt(20.0e-6 * 800.0e-6) * 2.0; // AtkMidScale
    const double attack_sec = (atk_frac <= 0.5)
        ? 20.0e-6 * pow(atk_mid / 20.0e-6, atk_frac / 0.5)
        : atk_mid * pow(800.0e-6 / atk_mid, (atk_frac - 0.5) / 0.5);
    const float attack_step    = (float)(0.035 / fmax(1.0, attack_sec * os_rate));          // EnvSlewSpan
    const float attack_rc_coef = (float)(1.0 - exp(-1.0 / fmax(1.0, 16.0 * attack_sec * os_rate))); // AtkLandFactor

    const double release_sec = (double)release_ms * 1.0e-3;
    const double rel_frac = fmax(0.0, fmin(1.0, log(release_sec / 0.050) / log(1.100 / 0.050)));
    const double rel_r_mid = sqrt(270.0e3 * 5.27e6) * 1.85; // RelMidScale
    const double rel_r = (rel_frac <= 0.5)
        ? 270.0e3 * pow(rel_r_mid / 270.0e3, rel_frac / 0.5)
        : rel_r_mid * pow(5.27e6 / rel_r_mid, (rel_frac - 0.5) / 0.5);
    const double rel_seconds = rel_r * 0.22e-6 / 2.0; // RelCapRatio = 1.0 -> divide by (1+ratio)
    const float release_coef = (float)((rel_seconds <= 0.0) ? 1.0 : (1.0 - exp(-1.0 / (rel_seconds * os_rate))));

    // 2x oversampling: upsample (zero-stuff + lowpass, x2 gain to compensate), run the core engine
    // at the oversampled rate for both resulting sub-samples, then lowpass + decimate back down --
    // see FX_TYPE_FETCOMP's own comment for why this replaces the original's polyphase oversampler.
    float outL = *l, outR = *r;
    for (int pass = 0; pass < 2; pass++) {
        st->smoothed_in  += (target_in  - st->smoothed_in)  * st->smooth_coef;
        st->smoothed_out += (target_out - st->smoothed_out) * st->smooth_coef;

        const float in_l = (pass == 0) ? (*l * 2.0f) : 0.0f;
        const float in_r = (pass == 0) ? (*r * 2.0f) : 0.0f;

        const float up_l = run_eq_biquad(in_l, &st->os_coeffs, &st->os_up_z1[0], &st->os_up_z2[0]);
        const float up_r = run_eq_biquad(in_r, &st->os_coeffs, &st->os_up_z1[1], &st->os_up_z2[1]);

        float core_l, core_r;
        fetcomp_process_subsample(st, up_l, up_r, cell_k, class_a_drive, class_a_norm, tx_flux_gain,
                                attack_step, attack_rc_coef, release_coef, ratio_drive, ratio_bias_v,
                                env_bias, limiting, &core_l, &core_r);

        const float down_l = run_eq_biquad(core_l, &st->os_coeffs, &st->os_down_z1[0], &st->os_down_z2[0]);
        const float down_r = run_eq_biquad(core_r, &st->os_coeffs, &st->os_down_z1[1], &st->os_down_z2[1]);

        if (pass == 1) { outL = down_l; outR = down_r; } // decimate: keep the second sub-sample
    }
    *l = outL;
    *r = outR;
    const float g0_now = st->two_beta * st->ctrl_volts;
    st->last_gr_db = fmaxf(0.0f, 20.0f * log10f(1.0f + FETCOMP_R5 * g0_now));
}

// --- FX_TYPE_FAIRKID67: ported from PHU-FAIR-KID-67 (github.com/huberp/phu-fair-kid-67, MIT
// license; DSP core from its sibling github.com/huberp/phu-audio-lib, also MIT) --- see
// FX_TYPE_FAIRKID67's own comment above for the param map. Like FX_TYPE_FETCOMP this is a genuine
// circuit-derived model, but a structurally different one: instead of one closed-form quadratic
// gain cell, every gain stage here (the preamp, the 3-stage sidechain tube cascade, and both arms
// of the push-pull output stage -- SIX stages total, times 2 channels) is its own small nonlinear
// circuit solved via warm-started Newton-Raphson on a Koren-model triode.
//
// KEPT FAITHFUL: the Koren triode current model (and its analytic partial derivatives) verbatim;
// each stage's exact 2-node (plate/cathode) KCL equations and the closed-form 2x2 Cramer's-rule
// Newton step derived from them; warm-starting the NR solve from the previous SAMPLE's converged
// operating point (this is what makes 8 iterations, let alone the handful actually needed once
// warmed up, enough -- a cold solve every sample would need far more); the grid-conduction clamp
// (Vgk can never exceed 0, modeling a real triode's grid drawing current above that point); the
// push-pull topology's own (outA-outB)/2 differential combination (arm B fed -sample, both arms
// sharing one CV) for even-harmonic cancellation; the sidechain's diode-detector forward-voltage
// dead zone and its 6-position timing-preset table (values traced to the real schematic's own
// component values, not invented); the cubic-Hermite soft-knee CV limiter; and every LinkMode's
// combination formula (Independent/Linked-Max/Linked-Avg/Mid-Side).
//
// DELIBERATELY CHANGED from the original JUCE-based C++:
//   1. No JUCE anywhere -- juce::AudioBuffer/SmoothedValue/Decibels/jlimit are gone, replaced with
//      plain arrays and fminf/fmaxf/powf, the same posture as every other ported effect in this
//      file.
//   2. Every stage's cathode bypass capacitor is omitted. The original library supports one as an
//      optional per-stage feature (a trapezoidal companion model), but every stage this signal
//      chain actually uses leaves it at that feature's own default of "none fitted" as far as this
//      port's own research into the source could confirm -- so there is nothing here to omit
//      faithfully, and adding unused capacitor-companion machinery would just be dead code.
//   3. The original computes each stage's small-signal quiescent gain (used to normalize the
//      stage to unity gain at rest, phase-corrected for the common-cathode stage's own inversion)
//      analytically, from the linearized system's own Jacobian. This port gets the same NUMBER a
//      different way: a one-time (not per-sample) finite-difference probe at the already-solved
//      quiescent operating point -- nudge Vin by +-1mV, resolve, take the central difference. Both
//      are literally computing the same derivative (a small-signal gain IS a derivative); this
//      port just evaluates it numerically instead of deriving the closed form, at zero per-sample
//      cost either way since it only runs once when a slot first activates this effect.
//   4. The original exposes a runtime-switchable NR iteration budget (a "Draft" vs "High quality"
//      toggle, 8 vs 20 iterations). This port fixes the per-sample cap at 8 (the original's own
//      "Draft" ceiling) -- warm-starting means steady-state audio converges in far fewer than that
//      regardless, so this only ever bites during a hard transient, exactly like the original's
//      own Draft mode.
//   5. The output transformer's own 2nd-order (vs. the other two transformers' 1st-order) filter
//      pair is reimplemented with this file's own existing RBJ-cookbook biquad machinery (the same
//      one EQ3/FETCOMP's own oversampling filter already use) rather than the original's own
//      distinct class, since that specific class's exact source wasn't available to this port's
//      research -- both are standard textbook 2-pole Butterworth (Q=0.7071) filter pairs, so this
//      is a same-technique reimplementation, not a guess at different math.
//   6. Sidechain routing: the original hardware (and this plugin) has no external sidechain input
//      at all -- the detector always taps the same signal being compressed. This port adds one:
//      the detector's own input transformer is a SEPARATE instance from the main path's, always
//      fed from whatever FXSlotParams.sidechain_source resolves to (see that field's own comment
//      and process_fx_chain()'s sc_l/sc_r) -- "Same as Source" reproduces the original's
//      self-sidechain behavior exactly (sc_l/sc_r simply equal this slot's own dry input), while
//      any other source lets this compressor duck/gate off a different signal entirely (e.g. a
///     kick drum on an ext-in pair ducking a synth pad's Program bus).
typedef struct { float mu, kp, kvb, kg1, x; } FK67TubeParams;

static const FK67TubeParams FK67_TUBE_12AX7 = { 100.0f, 600.0f, 300.0f, 1060.0f, 1.4f };
static const FK67TubeParams FK67_TUBE_12AU7 = { 21.5f, 84.5f, 0.0f, 1180.0f, 1.3f };
static const FK67TubeParams FK67_TUBE_12BH7 = { 20.0f, 3000.0f, 150.0f, 1200.0f, 1.3f };
static const FK67TubeParams FK67_TUBE_6386  = { 70.0f, 300.0f, 300.0f, 700.0f, 1.0f };

#define FK67_VCC 250.0f
#define FK67_RP 100000.0f
#define FK67_RK 1500.0f
#define FK67_INV_RP (1.0f / FK67_RP)
#define FK67_INV_RK (1.0f / FK67_RK)
#define FK67_INPUT_CLAMP_V 50.0f
#define FK67_CV_MAX_V 6.0f
#define FK67_SOFT_KNEE_V 0.75f
#define FK67_FORWARD_VOLTAGE_V 0.8f
#define FK67_VOLTS_PER_SAMPLE 10.0f
#define FK67_MAX_NR_ITERATIONS 8

// One 2-node (plate/cathode) triode stage's warm-started Newton-Raphson state -- shared shape for
// every one of the six stage roles (preamp, 3-stage sidechain cascade, both push-pull arms), since
// the original library itself uses the identical class for all of them (the ones that never
// receive a nonzero control voltage simply never have setCv() called with anything else). double,
// not float, MATCHING THE ORIGINAL EXACTLY (its own state is std::array<double,2>) -- this is not
// a style choice: the Jacobian's determinant is the difference of two products of conductances
// (~1e-4 S) and reciprocal resistances (~1e-5 to ~7e-4 S), and at the 12AU7's own bias point (the
// one tube here with kvb=0) that difference loses enough precision in float that the solve stalls
// at its own cold-start guess on iteration 0 forever -- confirmed directly (12AU7 alone came out
// stuck exactly at the {Vcc*0.5, 1.5} initial guess, every other tube converged fine in float).
// Double avoids the cancellation with room to spare, same as the original.
typedef struct { double vp, vk; } FK67NodeState;

// The closed-form 2x2 Cramer's-rule Newton step, warm-started from *st's own entry values and
// falling back to them on a singular Jacobian, a non-finite result, or exceeding max_iters without
// converging -- see this section's own top comment for why each of those is faithful to the
// original's own NRPolicy, not a shortcut.
static void fk67_stage_nr_solve(FK67NodeState *st, double vin, double cv_bias, const FK67TubeParams *tube, int max_iters) {
    const double mu = tube->mu, kp = tube->kp, kvb = tube->kvb, kg1 = tube->kg1, x = tube->x;
    vin = fmin(vin, st->vk); // grid-conduction clamp: Vgk can never exceed 0 (before the CV bias)
    const double fallback_vp = st->vp, fallback_vk = st->vk;
    double vp = st->vp, vk = st->vk;
    for (int iter = 0; iter < max_iters; iter++) {
        const double Vpk = vp - vk;
        const double Vgk = vin - vk - cv_bias;

        // Koren triode model, fused current + both analytic partials (Norman L. Koren, "Improved
        // Vacuum-Tube Models for SPICE", 1996) -- numerically-stable softplus/sigmoid forms avoid
        // overflow for large arguments.
        const double V2 = fmax(kvb + Vpk * Vpk, 1e-12);
        const double sqV2 = sqrt(V2);
        const double u = kp * (1.0 / mu + Vgk / sqV2);
        double sig;
        if (u >= 0.0) sig = 1.0 / (1.0 + exp(-u));
        else { const double eu = exp(u); sig = eu / (1.0 + eu); }
        double sp;
        if (u > 30.0) sp = u;
        else if (u < -30.0) sp = exp(u);
        else sp = log1p(exp(u));
        const double E1 = (Vpk / kp) * sp;

        double Ip = 0.0, gds = 0.0, gm = 0.0;
        if (E1 > 0.0) {
            const double Ex = pow(E1, x);
            const double Exx1 = Ex / E1;
            Ip = Ex / kg1;
            const double sp_over_kp = (Vpk != 0.0) ? (E1 / Vpk) : (sp / kp);
            const double dE1_dVpk = sp_over_kp - Vpk * Vpk * Vgk * sig / (V2 * sqV2);
            const double dE1_dVgk = Vpk * sig / sqV2;
            const double x_Exx1_kg1 = x * Exx1 / kg1;
            gds = x_Exx1_kg1 * dE1_dVpk;
            gm  = x_Exx1_kg1 * dE1_dVgk;
        }

        // KCL residuals (plate, cathode -- no cathode bypass companion terms, see this section's
        // own top comment) and the resulting Jacobian, solved via Cramer's rule.
        const double f1 = (FK67_VCC - vp) * FK67_INV_RP - Ip;
        const double f2 = vk * FK67_INV_RK - Ip;
        const double J00 = -FK67_INV_RP - gds;
        const double J01 = gds + gm;
        const double J10 = -gds;
        const double J11 = FK67_INV_RK + gds + gm;
        const double det = J00 * J11 - J01 * J10;
        if (fabs(det) < 1e-30) { vp = fallback_vp; vk = fallback_vk; break; }

        const double inv = 1.0 / det;
        const double new_vp = vp + (-f1 * J11 + f2 * J01) * inv;
        const double new_vk = vk + ( f1 * J10 - f2 * J00) * inv;
        if (!isfinite(new_vp) || !isfinite(new_vk)) { vp = fallback_vp; vk = fallback_vk; break; }

        const double dvp = new_vp - vp, dvk = new_vk - vk;
        vp = new_vp; vk = new_vk;
        const double delta_l2 = sqrt(dvp * dvp + dvk * dvk);
        const double x_l2 = sqrt(vp * vp + vk * vk);
        if (delta_l2 < 1e-9 * (x_l2 + 1.0)) break; // converged
        if (iter == max_iters - 1) { vp = fallback_vp; vk = fallback_vk; } // exceeded budget -- fall back
    }
    st->vp = vp;
    st->vk = vk;
}

// Small-signal gain (|dVp/dVin|) of a triode stage AT ITS CURRENT (already NR-converged) operating
// point -- used only for the push-pull arms' GR metering (see FX_TYPE_FAIRKID67's own GR comment
// below). At a fixed point f(vp,vk,vin)=0 of the same two KCL residuals fk67_stage_nr_solve zeroes,
// implicit differentiation gives J*d(vp,vk)/dVin = -df/dVin, and since Vin only enters both residuals
// through Vgk (df1/dVin = df2/dVin = -gm), Cramer's rule on that 2x2 system gives
// dVp/dVin = gm*(J11-J01)/det directly from the SAME Jacobian terms the NR solve itself computes on
// every iteration. Rather than threading those terms out of fk67_stage_nr_solve's loop (which would
// touch every one of its 6 call sites for a value only 2 of them need), this re-evaluates the Koren
// model once more at the already-converged (vp,vk) -- one extra model evaluation per stage per
// sample, no extra Newton iterations, and mathematically identical since the Jacobian at a converged
// point doesn't depend on how it was reached.
static double fk67_stage_signal_gain(const FK67NodeState *st, double vin, double cv_bias, const FK67TubeParams *tube) {
    const double mu = tube->mu, kp = tube->kp, kvb = tube->kvb, kg1 = tube->kg1, x = tube->x;
    const double vp = st->vp, vk = st->vk;
    vin = fmin(vin, vk);
    const double Vpk = vp - vk;
    const double Vgk = vin - vk - cv_bias;
    const double V2 = fmax(kvb + Vpk * Vpk, 1e-12);
    const double sqV2 = sqrt(V2);
    const double u = kp * (1.0 / mu + Vgk / sqV2);
    double sig;
    if (u >= 0.0) sig = 1.0 / (1.0 + exp(-u));
    else { const double eu = exp(u); sig = eu / (1.0 + eu); }
    double sp;
    if (u > 30.0) sp = u;
    else if (u < -30.0) sp = exp(u);
    else sp = log1p(exp(u));
    const double E1 = (Vpk / kp) * sp;
    double gds = 0.0, gm = 0.0;
    if (E1 > 0.0) {
        const double Ex = pow(E1, x);
        const double Exx1 = Ex / E1;
        const double sp_over_kp = (Vpk != 0.0) ? (E1 / Vpk) : (sp / kp);
        const double dE1_dVpk = sp_over_kp - Vpk * Vpk * Vgk * sig / (V2 * sqV2);
        const double dE1_dVgk = Vpk * sig / sqV2;
        const double x_Exx1_kg1 = x * Exx1 / kg1;
        gds = x_Exx1_kg1 * dE1_dVpk;
        gm  = x_Exx1_kg1 * dE1_dVgk;
    }
    const double J00 = -FK67_INV_RP - gds;
    const double J01 = gds + gm;
    const double J10 = -gds;
    const double J11 = FK67_INV_RK + gds + gm;
    const double det = J00 * J11 - J01 * J10;
    if (fabs(det) < 1e-30) return 0.0;
    return fabs(gm * (J11 - J01) / det);
}

// Per-channel, per-stage-role dynamic state: the NR node above plus the stage's own DC-blocking
// output filter (models the real output coupling capacitor every one of these tube stages has --
// without it the triode's own large quiescent plate voltage, ~100-200V, would swamp everything
// downstream).
typedef struct {
    FK67NodeState node;
    float dc_x1, dc_y1; // one-pole DC-blocking HPF state: y[n] = x[n]-x[n-1] + R*y[n-1], R ~10Hz
    float last_signal_gain; // |dVp/dVin| at this call's operating point (fk67_stage_signal_gain) --
                              // written unconditionally by fk67_stage_process below, cheap for every
                              // stage role even though only the push-pull arms' GR meter reads it.
} FK67StageState;

// Precomputed once per distinct TUBE TYPE (not per channel/instance -- every stage using the same
// tube+Vcc+Rp+Rk config shares one of these): the small-signal quiescent gain used to normalize
// the stage to unity gain at rest (with the sign flip a common-cathode stage's own inversion
// needs), and the quiescent operating point itself (state init / DC-blocker pre-charge).
typedef struct { float inv_gain_mag; float vq; float vkq; } FK67StageCoeffs;

static void fk67_compute_stage_coeffs(FK67StageCoeffs *out, const FK67TubeParams *tube) {
    // One-time quiescent solve, generous iteration budget (this runs once when a slot first
    // activates FX_TYPE_FAIRKID67, never per-sample -- see this section's own top comment).
    FK67NodeState probe = { FK67_VCC * 0.5f, 1.5f };
    fk67_stage_nr_solve(&probe, 0.0f, 0.0f, tube, 200);
    out->vq = probe.vp;
    out->vkq = probe.vk;

    // Small-signal gain via a central-difference probe at the now-known quiescent point (see this
    // section's own top comment for why this numeric approach is equivalent to the original's own
    // analytic Jacobian-based one).
    const float eps = 0.001f;
    FK67NodeState probe_hi = { out->vq, out->vkq };
    FK67NodeState probe_lo = { out->vq, out->vkq };
    fk67_stage_nr_solve(&probe_hi, eps, 0.0f, tube, 20);
    fk67_stage_nr_solve(&probe_lo, -eps, 0.0f, tube, 20);
    const float dvp = (probe_hi.vp - probe_lo.vp) / (2.0f * eps);
    out->inv_gain_mag = 1.0f / fmaxf(1e-6f, fabsf(dvp));
}

// One stage's full per-sample processing: volts scaling, the NR solve, DC blocking, and the
// unity-gain-at-rest normalization (with the common-cathode stage's own phase-inversion corrected
// so every stage's wet output stays in-phase with its dry input, matching the original).
static inline float fk67_stage_process(FK67StageState *st, const FK67StageCoeffs *coeffs,
                                        float dc_block_coeff, float sample, float cv_bias_raw,
                                        const FK67TubeParams *tube) {
    const float vin = fmaxf(-FK67_INPUT_CLAMP_V, fminf(FK67_INPUT_CLAMP_V, sample * FK67_VOLTS_PER_SAMPLE));
    const float cv_bias = fmaxf(0.0f, fminf(FK67_CV_MAX_V, cv_bias_raw));
    fk67_stage_nr_solve(&st->node, vin, cv_bias, tube, FK67_MAX_NR_ITERATIONS);

    const float vp_norm = st->node.vp / FK67_VOLTS_PER_SAMPLE;
    const float y = vp_norm - st->dc_x1 + dc_block_coeff * st->dc_y1;
    st->dc_x1 = vp_norm;
    st->dc_y1 = y;

    st->last_signal_gain = (float)fk67_stage_signal_gain(&st->node, vin, cv_bias, tube);

    return -coeffs->inv_gain_mag * y;
}

// First-order (one HPF pole, one LPF pole, both bilinear-transform-derived) transformer
// coloration stage, used for the input/sidechain-input/interstage transformers. A memoryless tanh
// saturator with unity small-signal gain (tanh(drive*x)/drive) follows.
typedef struct {
    float hpf_b0, hpf_b1, hpf_a1, hpf_x1, hpf_y1;
    float lpf_b0, lpf_b1, lpf_a1, lpf_x1, lpf_y1;
} FK67Xfmr1State;

static void fk67_xfmr1_init(FK67Xfmr1State *s, float hpf_hz, float lpf_hz) {
    const float fs = (float)SAMPLE_RATE;
    const float hpf_c = fmaxf(1.0f, fminf(0.49f * fs, hpf_hz));
    const float lpf_c = fmaxf(1.0f, fminf(0.49f * fs, lpf_hz));

    const float Kh = tanf((float)M_PI * hpf_c / fs);
    s->hpf_b0 = 1.0f / (1.0f + Kh);
    s->hpf_b1 = -s->hpf_b0;
    s->hpf_a1 = (1.0f - Kh) / (1.0f + Kh);
    s->hpf_x1 = s->hpf_y1 = 0.0f;

    const float Kl = tanf((float)M_PI * lpf_c / fs);
    s->lpf_b0 = Kl / (1.0f + Kl);
    s->lpf_b1 = s->lpf_b0;
    s->lpf_a1 = (1.0f - Kl) / (1.0f + Kl);
    s->lpf_x1 = s->lpf_y1 = 0.0f;
}

static inline float fk67_xfmr1_process(FK67Xfmr1State *s, float x, float drive) {
    const float h = s->hpf_b0 * x + s->hpf_b1 * s->hpf_x1 + s->hpf_a1 * s->hpf_y1;
    s->hpf_x1 = x; s->hpf_y1 = h;
    const float lp = s->lpf_b0 * h + s->lpf_b1 * s->lpf_x1 + s->lpf_a1 * s->lpf_y1;
    s->lpf_x1 = h; s->lpf_y1 = lp;
    const float d = fmaxf(1.0f, drive);
    return tanhf(d * lp) / d;
}

// Second-order (2-pole HPF + 2-pole LPF, Butterworth Q=0.7071) transformer coloration, used for
// the output transformer -- see this section's own top comment (point 5) for why this reuses
// EqBiquadCoeffs/run_eq_biquad rather than a bespoke class.
typedef struct {
    EqBiquadCoeffs hpf_coeffs, lpf_coeffs;
    float hpf_z1, hpf_z2, lpf_z1, lpf_z2;
} FK67Xfmr2State;

static void fk67_xfmr2_init(FK67Xfmr2State *s, float hpf_hz, float lpf_hz, float q) {
    const float fs = (float)SAMPLE_RATE;
    {
        const float w0 = 2.0f * (float)M_PI * hpf_hz / fs;
        const float alpha = sinf(w0) / (2.0f * q);
        const float cosw0 = cosf(w0);
        const float a0 = 1.0f + alpha;
        s->hpf_coeffs.b0 = ((1.0f + cosw0) * 0.5f) / a0;
        s->hpf_coeffs.b1 = -(1.0f + cosw0) / a0;
        s->hpf_coeffs.b2 = s->hpf_coeffs.b0;
        s->hpf_coeffs.a1 = (-2.0f * cosw0) / a0;
        s->hpf_coeffs.a2 = (1.0f - alpha) / a0;
    }
    {
        const float w0 = 2.0f * (float)M_PI * lpf_hz / fs;
        const float alpha = sinf(w0) / (2.0f * q);
        const float cosw0 = cosf(w0);
        const float a0 = 1.0f + alpha;
        s->lpf_coeffs.b0 = ((1.0f - cosw0) * 0.5f) / a0;
        s->lpf_coeffs.b1 = (1.0f - cosw0) / a0;
        s->lpf_coeffs.b2 = s->lpf_coeffs.b0;
        s->lpf_coeffs.a1 = (-2.0f * cosw0) / a0;
        s->lpf_coeffs.a2 = (1.0f - alpha) / a0;
    }
    s->hpf_z1 = s->hpf_z2 = s->lpf_z1 = s->lpf_z2 = 0.0f;
}

static inline float fk67_xfmr2_process(FK67Xfmr2State *s, float x, float drive) {
    const float h = run_eq_biquad(x, &s->hpf_coeffs, &s->hpf_z1, &s->hpf_z2);
    const float lp = run_eq_biquad(h, &s->lpf_coeffs, &s->lpf_z1, &s->lpf_z2);
    const float d = fmaxf(1.0f, drive);
    return tanhf(d * lp) / d;
}

// Sidechain timing presets (6 positions) -- attack/release time constants traced to the real
// schematic's own component values, not invented (positions 5/6 use a two-branch program-dependent
// release: the output follows whichever of a fast or slow decaying branch is currently higher,
// giving quick recovery after a brief transient and a long tail after sustained heavy drive).
typedef struct { bool auto_release; float attack_sec, release_sec, fast_sec, slow_sec; } FK67TimingPreset;
static const FK67TimingPreset FK67_TIMING_PRESETS[6] = {
    { false, 0.0002f, 0.30f, 0.0f,  0.0f  },
    { false, 0.0002f, 0.80f, 0.0f,  0.0f  },
    { false, 0.0002f, 2.00f, 0.0f,  0.0f  },
    { false, 0.0002f, 5.00f, 0.0f,  0.0f  },
    { true,  0.0002f, 10.0f, 0.50f, 10.0f },
    { true,  0.0002f, 25.0f, 1.00f, 25.0f },
};

typedef struct { float cv, cv_fast, cv_slow; } FK67DetectorState;

static inline float fk67_alpha(float tau_sec) {
    return expf(-1.0f / (tau_sec * (float)SAMPLE_RATE));
}

// Full-wave rectification with a forward-voltage dead zone (models the real twin-diode tube
// rectifier's own forward voltage -- signals below it produce no detector response at all, which
// is why this whole topology sounds gentle on low-level material even at maximum drive) followed
// by one-pole attack/release smoothing, or the dual-branch program-dependent release above.
static float fk67_detector_process(FK67DetectorState *st, float sample, const FK67TimingPreset *preset) {
    const float rect_v = fmaxf(0.0f, fabsf(sample * FK67_VOLTS_PER_SAMPLE) - FK67_FORWARD_VOLTAGE_V);
    const float alpha_attack = fk67_alpha(preset->attack_sec);
    if (!preset->auto_release) {
        const float alpha_release = fk67_alpha(preset->release_sec);
        const float alpha = (rect_v > st->cv) ? alpha_attack : alpha_release;
        st->cv = alpha * st->cv + (1.0f - alpha) * rect_v;
    } else {
        const float alpha_fast = fk67_alpha(preset->fast_sec);
        const float alpha_slow = fk67_alpha(preset->slow_sec);
        if (rect_v > st->cv_fast) st->cv_fast = alpha_attack * st->cv_fast + (1.0f - alpha_attack) * rect_v;
        else st->cv_fast = alpha_fast * st->cv_fast;
        st->cv_slow = alpha_slow * st->cv_slow + (1.0f - alpha_slow) * rect_v;
        st->cv = fmaxf(st->cv_fast, st->cv_slow);
    }
    return st->cv;
}

// Cubic-Hermite soft-knee CV limiter: unit slope at the knee's start, zero slope at cv_max, so the
// final approach to the stage's own hard CV ceiling is gradual rather than a sharp digital corner.
static float fk67_soft_limit_cv(float cv, float cv_max, float knee) {
    if (cv_max <= 0.0f) return 0.0f;
    knee = fmaxf(0.0f, fminf(cv_max, knee));
    if (knee <= 0.0f) return fmaxf(0.0f, fminf(cv_max, cv));
    const float clamped = fmaxf(0.0f, cv);
    const float knee_start = cv_max - knee;
    if (clamped <= knee_start) return clamped;
    if (clamped >= cv_max) return cv_max;
    const float t = (clamped - knee_start) / knee;
    const float p = t + t * t - t * t * t;
    return knee_start + knee * p;
}

typedef struct {
    // 6 stage roles x 2 channels: preamp, 3-stage sidechain cascade, push-pull arms A/B.
    FK67StageState preamp[2], sc1[2], sc2[2], sc3[2], arm_a[2], arm_b[2];
    // Coefficients, one per distinct tube type (shared by every stage/channel using that tube).
    FK67StageCoeffs coeffs_12au7, coeffs_12ax7, coeffs_12bh7, coeffs_6386;
    float dc_block_coeff; // ~10Hz one-pole DC blocker, same for every stage

    // Main-path input transformer, a SEPARATE sidechain-key input transformer (see this section's
    // own top comment, point 6), interstage transformer (all 1st-order), and output transformer
    // (2nd-order) -- one instance per channel each.
    FK67Xfmr1State xfmr_in[2], xfmr_sc_in[2], xfmr_interstage[2];
    FK67Xfmr2State xfmr_out[2];

    FK67DetectorState detector[2];

    bool initialized;
    float last_gr_db; // worst-case (max across both push-pull arms/channels) current reduction of
                        // the 6386 gain stage vs. its own cv=0 quiescent gain (coeffs_6386.
                        // inv_gain_mag) -- see run_fx_fairkid67's own comment. Read back by
                        // process_fx_chain() for the GR meter.
} FxFairKid67State;

static void fk67_init(FxFairKid67State *st) {
    fk67_compute_stage_coeffs(&st->coeffs_12au7, &FK67_TUBE_12AU7);
    fk67_compute_stage_coeffs(&st->coeffs_12ax7, &FK67_TUBE_12AX7);
    fk67_compute_stage_coeffs(&st->coeffs_12bh7, &FK67_TUBE_12BH7);
    fk67_compute_stage_coeffs(&st->coeffs_6386, &FK67_TUBE_6386);
    st->dc_block_coeff = expf(-2.0f * (float)M_PI * 10.0f / (float)SAMPLE_RATE);

    // Warm-start every stage instance at its own tube type's quiescent point, and pre-charge its
    // DC blocker there too (dc_x1 = vq, dc_y1 = 0 -> the very first real sample sees zero DC-block
    // transient rather than a ~1s settle from a cold zero, matching the original's own documented
    // behavior).
    const FK67StageCoeffs *preamp_c = &st->coeffs_12au7;
    const FK67StageCoeffs *sc12_c = &st->coeffs_12ax7;
    const FK67StageCoeffs *sc3_c = &st->coeffs_12bh7;
    const FK67StageCoeffs *arm_c = &st->coeffs_6386;
    FK67StageState *all_stages[6][2] = {
        { &st->preamp[0], &st->preamp[1] }, { &st->sc1[0], &st->sc1[1] },
        { &st->sc2[0], &st->sc2[1] },       { &st->sc3[0], &st->sc3[1] },
        { &st->arm_a[0], &st->arm_a[1] },   { &st->arm_b[0], &st->arm_b[1] },
    };
    const FK67StageCoeffs *role_coeffs[6] = { preamp_c, sc12_c, sc12_c, sc3_c, arm_c, arm_c };
    for (int role = 0; role < 6; role++) {
        for (int ch = 0; ch < 2; ch++) {
            FK67StageState *s = all_stages[role][ch];
            s->node.vp = role_coeffs[role]->vq;
            s->node.vk = role_coeffs[role]->vkq;
            s->dc_x1 = role_coeffs[role]->vq / FK67_VOLTS_PER_SAMPLE;
            s->dc_y1 = 0.0f;
        }
    }

    for (int ch = 0; ch < 2; ch++) {
        fk67_xfmr1_init(&st->xfmr_in[ch], 30.0f, 30000.0f);
        fk67_xfmr1_init(&st->xfmr_sc_in[ch], 30.0f, 30000.0f);
        fk67_xfmr1_init(&st->xfmr_interstage[ch], 25.0f, 22000.0f);
        fk67_xfmr2_init(&st->xfmr_out[ch], 30.0f, 18000.0f, 0.70710678f);
        st->detector[ch].cv = st->detector[ch].cv_fast = st->detector[ch].cv_slow = 0.0f;
    }
}

static void run_fx_fairkid67(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r, float sc_l, float sc_r) {
    FxFairKid67State *st = (FxFairKid67State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxFairKid67State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        fk67_init(st);
    }

    const float p0 = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    const float p1 = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    const float p2 = fmaxf(0.0f, fminf(1.0f, params->param[2]));
    const float p3 = fmaxf(0.0f, fminf(1.0f, params->param[3]));
    const float p4 = fmaxf(0.0f, fminf(1.0f, params->param[4]));
    const float p5 = fmaxf(0.0f, fminf(1.0f, params->param[5]));
    const float p6 = fmaxf(0.0f, fminf(1.0f, params->param[6]));
    const float p7 = fmaxf(0.0f, fminf(1.0f, params->param[7]));

    const float input_gain = powf(10.0f, (-20.0f + p0 * 60.0f) / 20.0f);  // -20..+40dB
    const float output_gain = powf(10.0f, (-20.0f + p1 * 40.0f) / 20.0f); // -20..+20dB
    const float threshold_v = p2 * 5.0f;                                    // 0..5V -- calibrated
    // against what this port's own detector chain can actually produce (a raw CV around 2-3V even
    // at a hot full-scale drive with Sidechain Gain around unity -- see this effect's own test
    // harness), not the original library's own acThresholdVoltage default (10V), which lives in a
    // different upstream gain-staging context this port doesn't reproduce.
    int timing_idx = (int)(p3 * 5.0f + 0.5f);
    if (timing_idx < 0) timing_idx = 0; else if (timing_idx > 5) timing_idx = 5;
    int link_idx = (int)(p4 * 3.0f + 0.5f); // 0=Independent,1=Linked-Max,2=Linked-Avg,3=Mid-Side
    if (link_idx < 0) link_idx = 0; else if (link_idx > 3) link_idx = 3;
    const float bias_v = p5 * 2.0f;               // 0..2V
    const float iron = 0.5f + p6 * 2.5f;          // 0.5..3.0x transformer drive multiplier
    const float sc_gain = p7 * 2.0f;              // 0..2.0x sidechain cascade drive

    const FK67TimingPreset *preset = &FK67_TIMING_PRESETS[timing_idx];

    float main_in[2] = { *l * input_gain, *r * input_gain };
    float sc_in[2] = { sc_l * input_gain, sc_r * input_gain };

    float preamp_out[2], raw_cv[2];
    for (int ch = 0; ch < 2; ch++) {
        const float xfmr_in = fk67_xfmr1_process(&st->xfmr_in[ch], main_in[ch], 1.2f * iron);
        preamp_out[ch] = fk67_stage_process(&st->preamp[ch], &st->coeffs_12au7, st->dc_block_coeff,
                                             xfmr_in, 0.0f, &FK67_TUBE_12AU7);

        const float sc_xfmr_in = fk67_xfmr1_process(&st->xfmr_sc_in[ch], sc_in[ch], 1.2f * iron);
        const float sc1 = fk67_stage_process(&st->sc1[ch], &st->coeffs_12ax7, st->dc_block_coeff,
                                              sc_xfmr_in, 0.0f, &FK67_TUBE_12AX7);
        const float sc2 = fk67_stage_process(&st->sc2[ch], &st->coeffs_12ax7, st->dc_block_coeff,
                                              sc1, 0.0f, &FK67_TUBE_12AX7);
        const float sc3 = fk67_stage_process(&st->sc3[ch], &st->coeffs_12bh7, st->dc_block_coeff,
                                              sc2, 0.0f, &FK67_TUBE_12BH7);
        raw_cv[ch] = fk67_detector_process(&st->detector[ch], sc3 * sc_gain, preset);
    }

    // Stereo linking (see FX_TYPE_FAIRKID67's own comment for each mode).
    float final_cv[2];
    if (link_idx == 3) { // Mid-Side
        const float mid = (raw_cv[0] + raw_cv[1]) * 0.5f;
        final_cv[0] = final_cv[1] = fmaxf(0.0f, mid - threshold_v) + bias_v;
    } else {
        const float eff0 = fmaxf(0.0f, raw_cv[0] - threshold_v) + bias_v;
        const float eff1 = fmaxf(0.0f, raw_cv[1] - threshold_v) + bias_v;
        if (link_idx == 0) { // Independent
            final_cv[0] = eff0; final_cv[1] = eff1;
        } else { // Linked-Max (1) or Linked-Avg (2)
            const float linked = (link_idx == 1) ? fmaxf(eff0, eff1) : (eff0 + eff1) * 0.5f;
            final_cv[0] = final_cv[1] = linked;
        }
    }

    for (int ch = 0; ch < 2; ch++) {
        const float shaped_cv = fk67_soft_limit_cv(final_cv[ch], FK67_CV_MAX_V, FK67_SOFT_KNEE_V);
        const float out_a = fk67_stage_process(&st->arm_a[ch], &st->coeffs_6386, st->dc_block_coeff,
                                                 preamp_out[ch], shaped_cv, &FK67_TUBE_6386);
        const float out_b = fk67_stage_process(&st->arm_b[ch], &st->coeffs_6386, st->dc_block_coeff,
                                                 -preamp_out[ch], shaped_cv, &FK67_TUBE_6386);
        const float gain_out = (out_a - out_b) * 0.5f;
        const float interstage = fk67_xfmr1_process(&st->xfmr_interstage[ch], gain_out, 1.1f * iron);
        const float output = fk67_xfmr2_process(&st->xfmr_out[ch], interstage, 1.0f * iron);
        *(ch == 0 ? l : r) = output * output_gain;
    }

    // GR meter: the 6386 push-pull stage is the only one of the 6 stage roles cv_bias ever drives
    // away from quiescent (see the tube-role table in fk67_init) -- worst case (max, not average)
    // across both arms/channels, so a single hot channel's compression isn't hidden by an idle one.
    const float gain_mag = fmaxf(fmaxf(st->arm_a[0].last_signal_gain, st->arm_a[1].last_signal_gain),
                                  fmaxf(st->arm_b[0].last_signal_gain, st->arm_b[1].last_signal_gain));
    const float ratio = gain_mag * st->coeffs_6386.inv_gain_mag; // 1.0 at quiescent (cv=0), <1.0 when compressing
    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, ratio)));
}

// --- FX_TYPE_PEAKMAX: ported from JClones_L2 (github.com/JClones/JSFXClones, MIT license) --- see
// FX_TYPE_PEAKMAX's own comment above for the param map. Unlike every earlier effect in this file,
// this one is LOOKAHEAD: it delays its own audio by a fixed number of samples (~1.3ms, see
// PEAKMAX_MAX_LOOKAHEAD's own comment) so gain reduction can already account for peaks that haven't
// reached the output yet -- true brickwall behavior, not just fast-attack limiting. That delay is
// real, unavoidable added latency on whichever slot uses this effect; nothing else in this file adds
// latency, and this engine has no plugin-delay-compensation mechanism of its own for mixing a
// delayed source's bus against an undelayed one live (a DAW host CAN compensate a CLAP instance for
// its own reported latency, but this codebase doesn't report any yet -- see the comment on
// clap_plugin.c's own process() call site, a real follow-up if this matters in practice).
//
// KEPT FAITHFUL: the "boost input by -Threshold dB, brickwall at a fixed 0dBFS-equivalent point,
// trim by Ceiling on the way out" gain-staging shape; the lookahead peak detector (an EXACT minimum
// of the raw per-sample gain-needed value over the upcoming lookahead window -- see
// PeakMaxMinWindow's own comment for why a different exact algorithm than the original's own is used
// here); the two-stage triangular-window smoothing after the release envelope (two cascaded boxcar
// moving averages, whose convolution IS a triangular window); the manual release's simple one-pole
// toward the lookahead minimum; the ENTIRE auto-release cascade (slow/fast/dynamic 3-stage detector,
// the piecewise-linear release-time-vs-"dynamic factor" curve, the 3 fixed calibration time
// constants 0.281s/0.057s/0.00054s) -- this multi-stage adaptive release is the original's own
// signature "smart" behavior and is ported verbatim, no simplification; and the full IDR-style
// dither/noise-shaping stage (the type-1 and type-2 PRNGs, the 9-tap per-sample-rate-bucket FIR
// noise-shaping filter and its complete coefficient table, the truncate-to-N-bits-with-error-
// feedback structure) -- included in full even though it's arguably a mastering/bit-depth-delivery
// feature orthogonal to this engine's always-float internal signal path, because it's still a
// well-defined, self-contained coloration effect on a float signal regardless of what's downstream
// (the same reasoning any bit-crusher effect already relies on), and the reference material specifies
// it completely -- there was no principled partial version to build instead.
//
// DELIBERATELY CHANGED:
//   1. No JSFX/EEL2 runtime -- allocate()'s dynamic memory pool becomes fixed-size C arrays sized to
//      PEAKMAX_MAX_LOOKAHEAD (see its own comment), same "opt out of dynamic allocation in the
//      real-time path" posture this whole codebase already uses everywhere else.
//   2. DelayLineMin's own bespoke branching-factor-8 min-tree is replaced with a standard monotonic
//      deque sliding-window minimum (PeakMaxMinWindow) -- a DIFFERENT exact algorithm for the
//      IDENTICAL well-defined quantity ("the minimum of the last N pushed values"), not an
//      approximation of it, the same class of substitution as this file's own reuse of one RBJ
//      biquad implementation for multiple filter roles elsewhere. Simpler to verify correct than
//      porting the original's own multi-level padded-array tree structure line for line.
//   3. JSFX's implicit `srate`-keyed recomputation (@init's own `sample_rate != srate` guard) becomes
//      a one-time computation on first slot activation, matching fk67_init/fetcomp_init's own
//      "compute derived constants once, not per sample" convention -- this engine's SAMPLE_RATE is a
//      fixed compile-time constant, so there is no runtime sample-rate change to react to at all.
//   4. Dither type-1's 32-bit LCG is implemented with plain uint32_t wraparound arithmetic rather
//      than EEL2's own double-precision-value-reinterpreted-as-a-bitwise-integer semantics -- the
//      same modular arithmetic either way (multiplication and addition mod 2^32 don't care whether
//      the ring's elements are labeled as signed or unsigned representatives), so this produces the
//      identical sequence of values, just expressed in C's own native integer types.
//   5. No internal Mix param and no sidechain input -- like the original, this works on its own
//      stereo signal only; the slot's own generic Mix knob already covers dry/wet if ever wanted.

// ~1.3ms at any sample rate this engine plausibly runs at (64 samples at 48kHz, the original's own
// `floor((0.0014*srate)/64+0.6)*64` formula, computed once at slot activation from the fixed
// SAMPLE_RATE -- see fx_peakmax_activate()). 256 gives a large safety margin (up to ~384kHz) for a
// value that's actually always 64 today, matching this codebase's own general "bound generously,
// clamp defensively" posture elsewhere (e.g. OSC_DISPATCH_TABLE_SIZE).
#define PEAKMAX_MAX_LOOKAHEAD 256
#define PEAKMAX_MAX_MA_SIZE (PEAKMAX_MAX_LOOKAHEAD / 2 + 1)
#define PEAKMAX_DITHER_HISTORY 9

// One-pole coefficient/apply helpers, calibrated the same way the original does: time_ref_20 is
// "seconds to settle to -20dB (10%) of a step's full excursion" -- ref20_mult bakes srate and that
// -20dB reference point together once so every later call is just one expf(). Not reused from
// anything else in this file since every other effect's own one-pole helper uses a DIFFERENT
// reference convention (e.g. run_fx_gate's attack/release use -60dB/e-folding style time constants)
// -- porting THIS specific convention exactly is what keeps the auto-release cascade's own 3
// calibration constants (0.281s/0.057s/0.00054s) meaning what the original intends.
static inline float peakmax_filter1_g(float time_ref_20, float ref20_mult) {
    return expf(-1.0f / (time_ref_20 * ref20_mult));
}
static inline float peakmax_filter1(float s, float g, float x) {
    return x + g * (s - x);
}

// Exact sliding-window minimum of the last `window_size` pushed values, via a standard monotonic
// deque (values kept increasing front-to-back; the front is always the current window's minimum) --
// see this effect's own top comment (point 2) for why this replaces the original's own min-tree
// structure. Capacity-bounded ring buffer of (value, push-index) pairs; at most `window_size`
// entries can ever be live in a monotonic deque over a window of that size, so PEAKMAX_MAX_LOOKAHEAD
// capacity is always enough.
typedef struct {
    float val[PEAKMAX_MAX_LOOKAHEAD];
    int64_t idx[PEAKMAX_MAX_LOOKAHEAD];
    int head, count;
    int64_t sample_pos;
    int window_size;
} PeakMaxMinWindow;

static float peakmax_minwindow_push(PeakMaxMinWindow *w, float x) {
    int64_t pos = w->sample_pos++;
    while (w->count > 0 && w->val[(w->head + w->count - 1) % PEAKMAX_MAX_LOOKAHEAD] >= x) {
        w->count--; // strictly-increasing invariant: anything >= the new value can never win again
    }
    int back = (w->head + w->count) % PEAKMAX_MAX_LOOKAHEAD;
    w->val[back] = x;
    w->idx[back] = pos;
    w->count++;
    while (w->count > 0 && w->idx[w->head] <= pos - w->window_size) {
        w->head = (w->head + 1) % PEAKMAX_MAX_LOOKAHEAD; // expire anything that's aged out of the window
        w->count--;
    }
    return w->val[w->head];
}

// Pre-seeds the window with `window_size` copies of default_value, matching DelayLineMin_reset's
// own "already full" starting state (so the very first real sample's getMin() reads default_value,
// not an empty/undefined result) -- identical repeated values simply collapse to one deque entry.
static void peakmax_minwindow_reset(PeakMaxMinWindow *w, int window_size, float default_value) {
    w->window_size = window_size;
    w->head = 0; w->count = 0; w->sample_pos = 0;
    for (int i = 0; i < window_size; i++) peakmax_minwindow_push(w, default_value);
}

// Plain boxcar moving average via a ring buffer + running sum -- two cascaded instances (see
// run_fx_peakmax) give a triangular smoothing window, exactly as the original's own two
// MovingAverageFilter instances do.
typedef struct {
    float buf[PEAKMAX_MAX_MA_SIZE];
    int head;
    float sum;
    int size;
} PeakMaxMovingAvg;

static void peakmax_ma_reset(PeakMaxMovingAvg *ma, int size, float default_value) {
    ma->size = size;
    ma->head = 0;
    for (int i = 0; i < size; i++) ma->buf[i] = default_value;
    ma->sum = default_value * (float)size;
}

static float peakmax_ma_push(PeakMaxMovingAvg *ma, float x) {
    ma->sum -= ma->buf[ma->head];
    ma->buf[ma->head] = x;
    ma->head++;
    if (ma->head >= ma->size) ma->head = 0;
    ma->sum += x;
    return ma->sum / (float)ma->size;
}

// Plain circular buffer delaying the actual audio signal by `size` samples, so gain reduction
// (computed from the lookahead window above) reaches each sample before -- not after -- it's
// applied. One instance per channel.
typedef struct {
    float buf[PEAKMAX_MAX_LOOKAHEAD];
    int head;
    int size;
} PeakMaxDelayLine;

static void peakmax_delay_reset(PeakMaxDelayLine *d, int size) {
    d->size = size;
    d->head = 0;
    for (int i = 0; i < size; i++) d->buf[i] = 0.0f;
}

static float peakmax_delay_push(PeakMaxDelayLine *d, float x) {
    float y = d->buf[d->head];
    d->buf[d->head] = x;
    d->head++;
    if (d->head >= d->size) d->head = 0;
    return y;
}

// 9-tap noise-shaping FIR filter coefficients, 4 shaping modes (None/Moderate/Normal/Ultra) x 4
// sample-rate buckets (<=44100/<=48000/<=88200/>88200 Hz) -- ported verbatim from the original's own
// table (this engine's fixed SAMPLE_RATE=48000 always selects the second bucket, but every bucket is
// kept so this table matches its source exactly rather than being pruned to "what's reachable
// today"). IR = PEAKMAX_DITHER_IR + 9*(shaping_type + 4*sample_rate_index).
static const float PEAKMAX_DITHER_IR[144] = {
    // bucket 0 (<=44100 Hz): None, Moderate, Normal, Ultra (9 taps each)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    -0.054383024f, -0.20654947f, -0.3822527f, -0.39219186f, -0.36627461f, -0.49556099f, -0.29294844f, 0.61599216f, 1.3177642f,
    -0.070082023f, -0.29561589f, -0.5451639f, -0.5487125f, -0.55320585f, -0.75886502f, -0.25751337f, 1.2202875f, 1.9089777f,
    -0.10739672f, -0.43569984f, -0.72708491f, -0.66456244f, -0.73261389f, -1.0359325f, -0.16065249f, 1.8239374f, 2.3525222f,
    // bucket 1 (<=48000 Hz -- the only bucket this engine's fixed SAMPLE_RATE ever selects)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    -0.1002716f, -0.29584958f, -0.40982519f, -0.38606261f, -0.44715518f, -0.52985251f, -0.10806594f, 0.82622085f, 1.3482654f,
    -0.11617057f, -0.41429373f, -0.69819106f, -0.84679904f, -0.99510928f, -0.8137319f, 0.33485791f, 1.8264681f, 2.072573f,
    -0.15380712f, -0.5469209f, -0.91673806f, -1.1670967f, -1.4463654f, -1.0427872f, 0.84138339f, 2.7803198f, 2.624423f,
    // bucket 2 (<=88200 Hz)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.088072546f, 0.36459012f, 0.88040251f, 1.5499839f, 2.1484938f, 2.4151576f, 2.2070139f, 1.5982079f,
    -0.00390274f, 0.1073065f, 0.63257259f, 1.8682127f, 3.5575584f, 5.014459f, 5.3379144f, 4.3814994f, 2.5787734f,
    -0.021631265f, 0.1594025f, 1.0213817f, 2.9718205f, 5.5339018f, 7.4154889f, 7.5202313f, 5.7418745f, 3.1130463f,
    // bucket 3 (>88200 Hz)
    0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    0.15717715f, 0.54336716f, 1.1581471f, 1.8591082f, 2.4045727f, 2.5688185f, 2.2673639f, 1.6110487f,
    -0.011899905f, 0.12947208f, 0.63891136f, 1.7815208f, 3.4063984f, 4.8728995f, 5.2856435f, 4.3677134f, 2.6052934f,
    -0.021628401f, 0.15915684f, 1.0202237f, 2.9688732f, 5.5289454f, 7.4093702f, 7.5145789f, 5.7382228f, 3.1116338f,
};

typedef struct {
    uint32_t type1_state;
    float type2_a, type2_b;
    float history_left[PEAKMAX_DITHER_HISTORY];
    float history_right[PEAKMAX_DITHER_HISTORY];
    int history_index;
    int dither_type;       // 0=None, 1=Type1, 2=Type2
    int shaping_type;      // 0=None, 1=Moderate, 2=Normal, 3=Ultra
    int sample_rate_index; // 0..3, from SAMPLE_RATE
    float scaling;         // 1 << (24 - bits)
} PeakMaxDither;

static void peakmax_dither_reset(PeakMaxDither *d) {
    d->type1_state = 0;
    d->type2_a = 0.0f;
    d->type2_b = 0.25f;
    for (int i = 0; i < PEAKMAX_DITHER_HISTORY; i++) { d->history_left[i] = 0.0f; d->history_right[i] = 0.0f; }
    d->history_index = 0;
}

static void peakmax_dither_set_mode(PeakMaxDither *d, int bits, int dither_type, int shaping_type) {
    d->dither_type = dither_type;
    d->shaping_type = shaping_type;
    d->scaling = (float)(1 << (24 - bits));
}

// Standard 32-bit LCG, kept in plain unsigned arithmetic (wraps mod 2^32 exactly like the original's
// own explicit `&4294967295`) -- see this effect's own top comment (point 4) for why the signed
// reinterpretation only needs to happen where the result is actually used below.
static int32_t peakmax_dither_type1_rand(PeakMaxDither *d) {
    d->type1_state = d->type1_state * 10368889u + 1u;
    return (int32_t)d->type1_state; // two's-complement reinterpretation -- this codebase already assumes it elsewhere
}

static float peakmax_dither_process_channel(PeakMaxDither *d, float input, float rand_type1, float rand_type2, float *history) {
    const float *ir = PEAKMAX_DITHER_IR + 9 * (d->shaping_type + 4 * d->sample_rate_index);
    float filtered_error = 0.0f;
    int k = 0, i = d->history_index;
    for (int n = 0; n < PEAKMAX_DITHER_HISTORY; n++) {
        filtered_error += history[i] * ir[k];
        k++;
        i++;
        if (i >= PEAKMAX_DITHER_HISTORY) i -= PEAKMAX_DITHER_HISTORY;
    }

    input = fmaxf(-1.0f, fminf(1.0f, input));

    float x1 = input * 8388608.0f; // 0x800000 -- to 24-bit integer scale
    float x2 = x1 + rand_type2 * d->scaling;
    float x3 = x2 + (rand_type1 + 0.5f) * d->scaling + filtered_error;

    float out_fp = floorf(x3);
    out_fp = fmaxf(-8388608.0f, fminf(8388607.0f, out_fp));
    if (d->scaling != 1.0f) out_fp = floorf(out_fp / d->scaling) * d->scaling;

    float error = x2 - out_fp;
    history[d->history_index] = error;

    return out_fp / 8388608.0f;
}

static void peakmax_dither_process(PeakMaxDither *d, float *l, float *r) {
    float rand_type1_left = 0.0f, rand_type1_right = 0.0f;
    float rand_type2_left = 0.0f, rand_type2_right = 0.0f;

    if (d->dither_type == 1) {
        int32_t a = peakmax_dither_type1_rand(d);
        int32_t b = peakmax_dither_type1_rand(d);
        rand_type1_left = (float)((double)((int64_t)a + (int64_t)b) / 4294967296.0);
        rand_type1_right = (float)((double)((int64_t)a - (int64_t)b) / 4294967296.0);
    } else if (d->dither_type == 2) {
        d->type2_a -= 0.16666667f;
        if (d->type2_a < -0.16666667f) d->type2_a = 0.16666667f;
        d->type2_b = -d->type2_b;
        rand_type2_left = d->type2_a + d->type2_b;
        rand_type2_right = d->type2_a - d->type2_b;
    }

    float out_l = peakmax_dither_process_channel(d, *l, rand_type1_left, rand_type2_left, d->history_left);
    float out_r = peakmax_dither_process_channel(d, *r, rand_type1_right, rand_type2_right, d->history_right);

    d->history_index++;
    if (d->history_index >= PEAKMAX_DITHER_HISTORY) d->history_index -= PEAKMAX_DITHER_HISTORY;

    *l = out_l;
    *r = out_r;
}

typedef struct {
    int lookahead_size;   // computed once, from SAMPLE_RATE -- see fx_peakmax_activate()
    float ref20_mult;     // srate / -log(0.1), for peakmax_filter1_g()

    PeakMaxMinWindow lookahead_min;
    PeakMaxMovingAvg smooth1, smooth2;
    PeakMaxDelayLine delay_left, delay_right;
    PeakMaxDither dither;

    float slow_gr_s, fast_gr_s, dynamic_gr_s;
    float dynamic_release_g;

    // Fixed auto-release calibration constants (0.281s/0.057s/0.00054s), computed once.
    float dynamic_g1, dynamic_g2, dynamic_g3;

    float last_gr_db;
    bool initialized;
} FxPeakMaxState;

static void fx_peakmax_activate(FxPeakMaxState *st) {
    // Same formula the original computes on every samplerate change (@init's own `sample_rate !=
    // srate` guard) -- this engine's SAMPLE_RATE is a fixed compile-time constant, so there's only
    // ever one value to compute, once, here.
    int lookahead = (int)(floorf((0.0014f * (float)SAMPLE_RATE) / 64.0f + 0.6f) * 64.0f);
    if (lookahead < 64) lookahead = 64; // never less than one 64-sample chunk
    if (lookahead > PEAKMAX_MAX_LOOKAHEAD) lookahead = PEAKMAX_MAX_LOOKAHEAD; // defensive clamp -- see PEAKMAX_MAX_LOOKAHEAD's own comment
    st->lookahead_size = lookahead;
    st->ref20_mult = (float)SAMPLE_RATE / -logf(0.1f);

    peakmax_minwindow_reset(&st->lookahead_min, st->lookahead_size, 1.0f);
    int ma_size = st->lookahead_size / 2 + 1;
    if (ma_size > PEAKMAX_MAX_MA_SIZE) ma_size = PEAKMAX_MAX_MA_SIZE;
    peakmax_ma_reset(&st->smooth1, ma_size, 1.0f);
    peakmax_ma_reset(&st->smooth2, ma_size, 1.0f);
    peakmax_delay_reset(&st->delay_left, st->lookahead_size);
    peakmax_delay_reset(&st->delay_right, st->lookahead_size);

    st->slow_gr_s = st->fast_gr_s = st->dynamic_gr_s = 1.0f;
    st->dynamic_release_g = peakmax_filter1_g(0.001f, st->ref20_mult);

    st->dynamic_g1 = peakmax_filter1_g(0.281f, st->ref20_mult);
    st->dynamic_g2 = peakmax_filter1_g(0.057f, st->ref20_mult);
    st->dynamic_g3 = peakmax_filter1_g(0.00054f, st->ref20_mult);

    // sample-rate bucket for the dither's own noise-shaping table (see PEAKMAX_DITHER_IR) --
    // SAMPLE_RATE is fixed today, but this mirrors the original's own bucketing exactly rather than
    // hardcoding "always bucket 1".
    int sr_idx = (SAMPLE_RATE <= 44100) ? 0 : (SAMPLE_RATE <= 48000) ? 1 : (SAMPLE_RATE <= 88200) ? 2 : 3;
    peakmax_dither_reset(&st->dither);
    st->dither.sample_rate_index = sr_idx;

    st->last_gr_db = 0.0f;
    st->initialized = true;
}

static void run_fx_peakmax(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxPeakMaxState *st = (FxPeakMaxState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxPeakMaxState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
        fx_peakmax_activate(st);
    }

    const float threshold_db = -30.0f + fmaxf(0.0f, fminf(1.0f, params->param[0])) * 30.0f; // -30..0dB
    const float ceiling_db = -30.0f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 30.0f;   // -30..0dB
    const bool use_auto_release = params->param[2] >= 0.5f;
    int release_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[3])) * 5.0f + 0.5f);
    if (release_idx < 0) release_idx = 0; else if (release_idx > 5) release_idx = 5;
    int quantize_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[4])) * 4.0f + 0.5f);
    if (quantize_idx < 0) quantize_idx = 0; else if (quantize_idx > 4) quantize_idx = 4;
    int dither_type = (int)(fmaxf(0.0f, fminf(1.0f, params->param[5])) * 2.0f + 0.5f);
    if (dither_type < 0) dither_type = 0; else if (dither_type > 2) dither_type = 2;
    int shaping_type = (int)(fmaxf(0.0f, fminf(1.0f, params->param[6])) * 3.0f + 0.5f);
    if (shaping_type < 0) shaping_type = 0; else if (shaping_type > 3) shaping_type = 3;

    const float input_gain_k = powf(10.0f, -threshold_db / 20.0f);
    const float output_gain_k = powf(10.0f, ceiling_db / 20.0f);
    // {0.01, 0.1, 1.0, 10.0, 100, 1000} ms, from pow(10, index-5) seconds -- the original's own
    // exact release_idx-to-seconds mapping (its slider4's own labeled option values).
    const float release_time_sec = powf(10.0f, (float)release_idx - 5.0f);
    const float fast_gr_g = peakmax_filter1_g(release_time_sec, st->ref20_mult);

    peakmax_dither_set_mode(&st->dither, 16 + quantize_idx * 2, dither_type, shaping_type);

    float in_l = *l * input_gain_k;
    float in_r = *r * input_gain_k;

    const float level = fmaxf(fabsf(in_l), fabsf(in_r));
    const float raw_gr = (level < 1.0f) ? 1.0f : (1.0f / level);

    const float min_gr = peakmax_minwindow_push(&st->lookahead_min, raw_gr);

    float output_gr;
    if (!use_auto_release) {
        if (min_gr < st->fast_gr_s) st->fast_gr_s = min_gr; // attack: instant
        else st->fast_gr_s = peakmax_filter1(st->fast_gr_s, fast_gr_g, min_gr); // release: fixed
        output_gr = st->fast_gr_s;
    } else {
        // 1. slow detector (0.250s attack / 0.500s release -- fixed regardless of the Release param)
        const float slow_attack_g = peakmax_filter1_g(0.250f, st->ref20_mult);
        const float slow_release_g = peakmax_filter1_g(0.500f, st->ref20_mult);
        if (min_gr < st->slow_gr_s) st->slow_gr_s = peakmax_filter1(st->slow_gr_s, slow_attack_g, min_gr);
        else st->slow_gr_s = peakmax_filter1(st->slow_gr_s, slow_release_g, min_gr);

        // 2. combine both detectors
        const float combined_gr = fminf(min_gr, st->slow_gr_s);
        if (combined_gr < st->dynamic_gr_s) {
            st->dynamic_gr_s = combined_gr; // attack: instant
        } else {
            st->dynamic_gr_s = peakmax_filter1(st->dynamic_gr_s, st->dynamic_release_g, combined_gr); // release: adaptive
        }
        output_gr = st->dynamic_gr_s;

        // 3. adapt dynamic_release_g for the NEXT sample, from THIS sample's own result -- a
        // one-sample-delayed adaptive coefficient, exactly matching the original's own order.
        const float dynamic_release = powf(1.0f - output_gr, 4.0f);
        float dynamic_release_g1 = fmaxf(peakmax_filter1_g(fmaxf(dynamic_release, 1e-9f), st->ref20_mult), expf(-1.0f));
        const float dynamic_factor = fminf(st->slow_gr_s * 2.0f - output_gr, 1.0f);
        float dynamic_release_g2;
        if (dynamic_factor < 0.25f) {
            dynamic_release_g2 = 1.0f;
        } else if (dynamic_factor < 0.5f) {
            dynamic_release_g2 = (dynamic_factor - 0.25f) / 0.25f * (st->dynamic_g1 - 1.0f) + 1.0f;
        } else if (dynamic_factor < 0.71f) {
            dynamic_release_g2 = (dynamic_factor - 0.5f) / 0.21f * (st->dynamic_g2 - st->dynamic_g1) + st->dynamic_g1;
        } else {
            dynamic_release_g2 = (dynamic_factor - 0.71f) / 0.29f * (st->dynamic_g3 - st->dynamic_g2) + st->dynamic_g2;
        }
        st->dynamic_release_g = dynamic_release_g1 * dynamic_release_g2;
    }

    // Triangular smoothing (two cascaded boxcars) -- removes the stepping/zipper artifacts a raw
    // sliding-window-minimum's own discrete jumps would otherwise leave in the gain curve.
    output_gr = peakmax_ma_push(&st->smooth1, output_gr);
    output_gr = peakmax_ma_push(&st->smooth2, output_gr);

    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, output_gr)));

    const float delayed_l = peakmax_delay_push(&st->delay_left, in_l);
    const float delayed_r = peakmax_delay_push(&st->delay_right, in_r);

    float out_l = delayed_l * output_gr * output_gain_k;
    float out_r = delayed_r * output_gr * output_gain_k;

    peakmax_dither_process(&st->dither, &out_l, &out_r);

    *l = out_l;
    *r = out_r;
}

// --- FX_TYPE_MULTIFILTER: see its own #define comment above for the full param map and design
// rationale. Building blocks below are a standalone extraction of run_svf_filter's own per-stage
// TPT (topology-preserving-transform) math -- same formulas, same stability floor on resonance --
// generalized to run any number of cascaded stages instead of that function's own hardcoded 1-or-2,
// so this effect can never change ProgramState.filters[5]'s own existing behavior (a completely
// separate function and state struct, sharing only the underlying technique).
#define FX_FILTER_MAX_2POLE_STAGES 8 // 8 stages * 12dB/oct = 96dB/oct, this effect's own steepest Slope option

// One 2-pole TPT SVF stage. a1/a2/a3/k are precomputed once per sample (same cutoff/resonance for
// every cascaded stage) and passed in rather than recomputed per stage.
static inline float fx_multifilter_2pole_stage(float input, float k, float a1, float a2, float a3, int filter_type, float *low_state, float *band_state) {
    float v3 = input - *low_state;
    float v1 = a1 * (*band_state) + a2 * v3; // bandpass output
    float v2 = *low_state + a2 * (*band_state) + a3 * v3; // lowpass output
    *band_state = 2.0f * v1 - *band_state;
    *low_state = 2.0f * v2 - *low_state;
    // Same defense-in-depth hard state clamp as run_svf_filter's own -- see its comment.
    *band_state = fmaxf(-1.0e6f, fminf(1.0e6f, *band_state));
    *low_state = fmaxf(-1.0e6f, fminf(1.0e6f, *low_state));
    float high = input - k * v1 - v2;
    switch (filter_type) {
        case 1:  return v2;        // LPF
        case 2:  return high;      // HPF
        case 3:  return v1;        // BPF
        default: return v2 + high; // Notch (filter_type == 4)
    }
}

// One 1-pole TPT stage, for a Slope choice's odd 6dB remainder (Lowpass/Highpass only -- see this
// effect's own #define comment for why Bandpass/Notch never call this). Same trapezoidal-integrator
// family as the 2-pole stage above (and as this file's own fetcomp_highpass/FairKid67 DC blockers),
// just first-order: g_one is tan(pi*fc/fs), the same prewarped coefficient the 2-pole stages use.
static inline float fx_multifilter_1pole_stage(float input, float g_one, int filter_type, float *z) {
    float G = g_one / (1.0f + g_one);
    float v = (input - *z) * G;
    float lp = v + *z;
    *z = lp + v;
    return (filter_type == 2) ? (input - lp) : lp; // HPF: input minus lowpass; LPF: the lowpass itself
}

typedef struct {
    float low_state[FX_FILTER_MAX_2POLE_STAGES][2];  // [stage][channel]
    float band_state[FX_FILTER_MAX_2POLE_STAGES][2];
    float onepole_z[2]; // [channel] -- only used when the Slope choice needs the odd-6dB extra stage
} FxMultiFilterState;

static void run_fx_multifilter(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxMultiFilterState *st = (FxMultiFilterState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxMultiFilterState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }

    int filter_type = 1 + (int)(fmaxf(0.0f, fminf(1.0f, params->param[0])) * 3.0f + 0.5f); // 0..3 -> 1..4 (LP/HP/BP/Notch)
    if (filter_type < 1) filter_type = 1; else if (filter_type > 4) filter_type = 4;

    float cutoff_hz = fmaxf(40.0f, fminf(18000.0f, fx_eq3_fraction_to_hz(params->param[1])));
    float res_q = fmaxf(0.0f, fminf(100.0f, params->param[2]));

    static const int slope_db_table[12] = { 6, 12, 18, 24, 30, 36, 42, 48, 60, 72, 84, 96 };
    int slope_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[3])) * 11.0f + 0.5f);
    if (slope_idx < 0) slope_idx = 0; else if (slope_idx > 11) slope_idx = 11;
    int total_poles = slope_db_table[slope_idx] / 6;
    int two_pole_stages = total_poles / 2;
    bool has_extra_onepole = ((total_poles % 2) == 1) && (filter_type == 1 || filter_type == 2);
    // Bandpass/Notch have no natural 1-pole equivalent (see this effect's own #define comment) --
    // an odd Slope choice just rounds down for those two types, and 6dB itself (which would
    // otherwise resolve to zero stages, i.e. a silent full bypass) clamps up to the smallest
    // meaningful bandpass/notch this topology can express: one 2-pole stage, 12dB/oct.
    if ((filter_type == 3 || filter_type == 4) && two_pole_stages < 1) two_pole_stages = 1;
    if (two_pole_stages > FX_FILTER_MAX_2POLE_STAGES) two_pole_stages = FX_FILTER_MAX_2POLE_STAGES;

    const float k = fmaxf(0.05f, 2.0f * (1.0f - res_q / 100.0f));
    const float g = tanf((float)M_PI * cutoff_hz / (float)SAMPLE_RATE);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;

    float *chan[2] = { l, r };
    for (int ch = 0; ch < 2; ch++) {
        float sig = *chan[ch];
        for (int s = 0; s < two_pole_stages; s++) {
            sig = fx_multifilter_2pole_stage(sig, k, a1, a2, a3, filter_type, &st->low_state[s][ch], &st->band_state[s][ch]);
        }
        if (has_extra_onepole) {
            sig = fx_multifilter_1pole_stage(sig, g, filter_type, &st->onepole_z[ch]);
        }
        *chan[ch] = sig;
    }
}

// --- FX_TYPE_OPTOCOMP: ported from 4a2a (github.com/aim-qmul/4a2a, MPL-2.0 license) --- see
// FX_TYPE_OPTOCOMP's own #define comment above for the param map and what was left out. Kept
// faithful: the hard-knee power-law gain computer (no threshold-offset-in-dB detector math, no
// separate envelope stage -- literally `min(1, (|x|/thresholdGain)^-slope)` computed fresh every
// sample from the instantaneous absolute input value), the asymmetric attack/release one-pole
// smoothing applied to that resulting gain value itself, and the original's own specific `m2c()`
// time-constant calibration (`1 - exp(-2200/(ms*srate))` -- note ms is used AS GIVEN, not converted
// to seconds first; ported exactly, not "corrected" to a more usual seconds-based convention, since
// this specific formula is what gives the port its actual timing feel). Changed: the original's own
// per-BLOCK array-with-carry implementation (`gBuffer`, sized samplesPerBlock+1, textbook JUCE
// block-processing style) becomes a single persistent float here, since this engine calls every FX
// effect once per SAMPLE, not once per block -- this is an adaptation of the reference's own
// block-oriented plumbing to this engine's per-sample calling convention, not a change to the DSP
// itself (sample-for-sample identical results either way, one state float updated every sample vs.
// the same math spread across a block-sized array with a value carried at the boundary).
typedef struct {
    float gain_smoothed; // starts at 1.0 -- see FxGateState's own comment on why calloc's 0.0
                           // default would wrongly compress the very first block's audio.
    bool initialized;
    float last_gr_db;
} FxOptoCompState;

static void run_fx_optocomp(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxOptoCompState *st = (FxOptoCompState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxOptoCompState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) { st->gain_smoothed = 1.0f; st->initialized = true; }

    const float threshold_db = -60.0f + fmaxf(0.0f, fminf(1.0f, params->param[0])) * 60.0f; // -60..0dB
    const float ratio = 1.0f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 19.0f;           // 1..20
    const float attack_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 99.9f;       // 0.1..100ms
    const float release_ms = 100.0f + fmaxf(0.0f, fminf(1.0f, params->param[3])) * 900.0f;   // 100..1000ms
    const float makeup_db = -12.0f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 24.0f;     // -12..+12dB

    const float threshold_gain = powf(10.0f, threshold_db / 20.0f);
    const float makeup_gain = powf(10.0f, makeup_db / 20.0f);
    const float attack_coef = 1.0f - expf(-2200.0f / (attack_ms * (float)SAMPLE_RATE));
    const float release_coef = 1.0f - expf(-2200.0f / (release_ms * (float)SAMPLE_RATE));
    const float comp_slope = 1.0f - 1.0f / ratio;

    // Shared mono detector/envelope -- see this effect's own #define comment for why.
    const float level = fmaxf(fabsf(*l), fabsf(*r));
    // powf(0, negative) is +Infinity (IEEE 754, not a domain error), and fminf(1.0, Infinity) is
    // 1.0 -- the same implicit "true silence never needs a special case" behavior the original's
    // own std::fmin/std::pow already relies on, ported as-is with no extra guard needed.
    const float gain_target = fminf(1.0f, powf(level / threshold_gain, -comp_slope));
    const float coef = (gain_target < st->gain_smoothed) ? attack_coef : release_coef;
    st->gain_smoothed = gain_target * coef + st->gain_smoothed * (1.0f - coef);

    *l = *l * st->gain_smoothed * makeup_gain;
    *r = *r * st->gain_smoothed * makeup_gain;

    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, st->gain_smoothed)));
}

// --- FX_TYPE_DEESSER: see its own #define comment above for the full design. ---
typedef struct {
    float low_state[2];  // [channel] -- the crossover highpass's own 2-pole TPT state
    float band_state[2]; // [channel]
    float gain_smoothed; // starts at 1.0 -- see FxGateState's own comment on why calloc's 0.0 would be wrong
    bool initialized;
    float last_gr_db;
} FxDeEsserState;

static void run_fx_deesser(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxDeEsserState *st = (FxDeEsserState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxDeEsserState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) { st->gain_smoothed = 1.0f; st->initialized = true; }

    // 1kHz * 16^fraction -> 1kHz..16kHz, log-mapped -- same style as fx_eq3_fraction_to_hz's own
    // 20Hz-20kHz range, just narrowed to where sibilance actually lives.
    const float freq_hz = 1000.0f * powf(16.0f, fmaxf(0.0f, fminf(1.0f, params->param[0])));
    const float threshold_db = -40.0f + fmaxf(0.0f, fminf(1.0f, params->param[1])) * 40.0f; // -40..0dB
    const float ratio = 1.0f + fmaxf(0.0f, fminf(1.0f, params->param[2])) * 19.0f;          // 1..20
    const float attack_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[3])) * 19.9f;      // 0.1..20ms
    const float release_ms = 5.0f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 295.0f;    // 5..300ms
    const bool listen = params->param[5] >= 0.5f;

    // Crossover: one fixed-Q (Butterworth, no resonance -- this is a clean split, not a tone-shaping
    // filter) 2-pole TPT highpass reused directly from FX_TYPE_MULTIFILTER, giving the high
    // (sibilance) band; the low band is `input - high`, exact by construction (see this effect's
    // own #define comment for why that needs no separately-designed lowpass at all).
    const float k = 1.41421356f; // Q = 1/k = 0.7071, Butterworth
    const float g = tanf((float)M_PI * freq_hz / (float)SAMPLE_RATE);
    const float a1 = 1.0f / (1.0f + g * (g + k));
    const float a2 = g * a1;
    const float a3 = g * a2;

    const float high_l = fx_multifilter_2pole_stage(*l, k, a1, a2, a3, 2 /* HPF */, &st->low_state[0], &st->band_state[0]);
    const float high_r = fx_multifilter_2pole_stage(*r, k, a1, a2, a3, 2 /* HPF */, &st->low_state[1], &st->band_state[1]);
    const float low_l = *l - high_l;
    const float low_r = *r - high_r;

    // Same hard-knee power-law gain computer as FX_TYPE_OPTOCOMP, applied to the high band's own
    // level only (self-detecting) -- but this file's own native exponential attack/release
    // convention (FX_TYPE_GATE/FX_TYPE_LIMITER's own `1-exp(-1/(0.001*ms*SAMPLE_RATE))`), not
    // 4a2a's specific calibration, since nothing here is emulating that plugin.
    const float threshold_gain = powf(10.0f, threshold_db / 20.0f);
    const float comp_slope = 1.0f - 1.0f / ratio;
    const float level = fmaxf(fabsf(high_l), fabsf(high_r));
    const float gain_target = fminf(1.0f, powf(level / threshold_gain, -comp_slope));
    const float rate_ms = (gain_target < st->gain_smoothed) ? attack_ms : release_ms;
    const float rate = 1.0f - expf(-1.0f / (0.001f * rate_ms * (float)SAMPLE_RATE));
    st->gain_smoothed += (gain_target - st->gain_smoothed) * rate;

    st->last_gr_db = fmaxf(0.0f, -20.0f * log10f(fmaxf(1e-6f, st->gain_smoothed)));

    if (listen) {
        // Solo the RAW (pre-gain-reduction) high band -- for dialing in Frequency by ear, so
        // Threshold/Ratio can then be trusted to actually be listening to the sibilance.
        *l = high_l;
        *r = high_r;
    } else {
        *l = low_l + high_l * st->gain_smoothed;
        *r = low_r + high_r * st->gain_smoothed;
    }
}

// --- FX_TYPE_HARDTUNE: see its own #define comment above for the full design and what it is/isn't
// a clone of. Three stages: (1) YIN pitch detection, run once per HOP on a mono-summed analysis
// window; (2) scale/key (or MIDI) quantization of the detected pitch to a target ratio, smoothed in
// two independent stages (Tracking Speed on the raw pitch, Retune Speed on the final ratio); (3) a
// period-synchronous two-tap crossfaded delay-line read, PSOLA's own core resynthesis mechanism,
// applied identically to both channels from the one shared detected pitch/ratio (mono detector,
// same "one shared value, never independent per-channel" posture as every other dynamics effect in
// this file, for the same reason -- a real vocal source is usually a single pitched voice, not two
// independently-pitched channels).
//
// PSOLA without explicit pitch marks: the classic algorithm places synthesis grains at hand-picked
// glottal-pulse-aligned "marks." This implementation gets the same period-synchronous effect more
// simply, without ever detecting individual pulses: a phase accumulator advances by `ratio` (the
// pitch-shift factor) each sample and wraps modulo `grain_period` (the CURRENT detected input
// period, in samples) -- two read taps, 50% of a period apart in phase, each windowed by a Hann
// function of its own phase (0 at wrap, peak at mid-period). Because a Hann window's value at phase
// x and at phase (x+0.5 mod 1) sum to exactly 1 (the standard 50%-overlap constant-overlap-add
// property), the two taps crossfade into each other with no gain bump or dip at the wrap point, no
// extra normalization needed. Each tap reads from a per-channel delay line at a small, BOUNDED
// offset behind the write pointer (`-DELAY + phase - grain_period/2`, always within
// `[-DELAY-grain_period/2, -DELAY+grain_period/2]`) -- bounded forever regardless of how long a
// non-1.0 ratio persists, since `phase` wraps rather than accumulating; DELAY is fixed at this
// engine's own absolute-lowest-supported-pitch period, comfortably covering every Voice Range
// preset's own longer periods too.
//
// FX_HARDTUNE_MAX_LAG/MIN_LAG bound the compile-time scratch arrays (sized for this engine's own
// absolute widest search range, 70-1100Hz); the actual per-call search range narrows to whichever
// Voice Range preset is selected.
// -1 = no note currently held (MIDI Target Source mode then falls back to no correction, same as an
// unvoiced input); else the current monophonic MIDI note number. Set by handle_note_on/
// handle_note_off (via g_hardtune_note_stack, declared later where MonoNoteStack is defined --
// these two plain floats live here instead, before run_fx_hardtune's own definition, since that
// function needs them and is defined well before MonoNoteStack is).
static float g_hardtune_held_note = -1.0f;
// Live pitch bend, in SEMITONES (already scaled by Program 0's own pb_range_up/pb_range_down, the
// same convention render_audio_block's own per-Program pitch bend already uses -- see
// dispatch_midi_event's own HDS_MIDI_PITCH_BEND branch). Applied by FX_TYPE_HARDTUNE as a final
// multiplicative adjustment layered on top of its ALREADY-corrected pitch, unconditionally
// (regardless of Target Source) -- see that effect's own comment for why it's never fed back into
// detection or quantization.
static float g_hardtune_bend_semitones = 0.0f;
// Mod Wheel (CC1) and channel aftertouch, mirrored from MidiLiveState purely so FX_TYPE_HARDTUNE's
// own Vibrato Depth can be scaled by a live controller (same "gate an LFO depth with the mod
// wheel" idea as PackLfoParams.mod_source_cc, but this global effect has no MidiLiveState of its
// own to read -- same reason g_hardtune_bend_semitones/g_hardtune_held_note are globals instead).
// Updated by dispatch_midi_event's HDS_MIDI_CC/HDS_MIDI_CHANNEL_PRESSURE branches, right alongside
// their canonical MidiLiveState update.
static float g_hardtune_modwheel = 0.0f;
static float g_hardtune_channel_pressure = 0.0f;

#define FX_HARDTUNE_ABS_MIN_HZ 70.0f
#define FX_HARDTUNE_ABS_MAX_HZ 1100.0f
#define FX_HARDTUNE_MIN_LAG 44  // SAMPLE_RATE/1100Hz
#define FX_HARDTUNE_MAX_LAG 686 // SAMPLE_RATE/70Hz
#define FX_HARDTUNE_YIN_WINDOW 1400 // > FX_HARDTUNE_MAX_LAG with real margin for the difference function's own overlap sum
#define FX_HARDTUNE_YIN_HOP 480     // 10ms at 48kHz -- how often pitch is re-detected
#define FX_HARDTUNE_BUF_SIZE 4096   // power of 2 (cheap masking); comfortably covers YIN_WINDOW + PSOLA's own read-behind depth
#define FX_HARDTUNE_BUF_MASK (FX_HARDTUNE_BUF_SIZE - 1)
#define FX_HARDTUNE_PSOLA_DELAY FX_HARDTUNE_MAX_LAG // samples the PSOLA read stays behind the write pointer -- this IS the effect's fixed added latency

// YIN (de Cheveigne & Kawahara, 2002): difference function -> cumulative mean normalized difference
// function -> absolute-threshold local-minimum search (falling back to the global minimum if no
// candidate clears the threshold) -> parabolic interpolation for sub-sample period accuracy.
// *out_confidence is 1 - (the CMNDF value at the chosen lag), i.e. how convincingly periodic the
// window is -- low for noise/silence/unvoiced consonants, used by the caller as a voiced/unvoiced
// gate. window_size/min_lag/max_lag are all runtime values (Voice Range's own narrowed search
// range), bounded by the compile-time FX_HARDTUNE_MAX_LAG-sized scratch arrays below.
static float hardtune_yin_detect(const float *window, int window_size, int min_lag, int max_lag, float *out_confidence) {
    static const float THRESHOLD = 0.15f;
    float d[FX_HARDTUNE_MAX_LAG + 1];
    float d_prime[FX_HARDTUNE_MAX_LAG + 1];
    // Defensive clamp -- every real caller guarantees 1 <= min_lag <= max_lag <= FX_HARDTUNE_MAX_LAG
    // (Voice Range presets always have min_hz < max_hz), but this keeps every d_prime[] index below
    // provably in-bounds and initialized regardless, rather than relying on that being upheld.
    if (min_lag < 1) min_lag = 1;
    if (max_lag < min_lag) max_lag = min_lag;

    d[0] = 0.0f;
    for (int tau = 1; tau <= max_lag; tau++) {
        float sum = 0.0f;
        int n = window_size - tau;
        for (int i = 0; i < n; i++) {
            float diff = window[i] - window[i + tau];
            sum += diff * diff;
        }
        d[tau] = sum;
    }

    d_prime[0] = 1.0f;
    float running_sum = 0.0f;
    for (int tau = 1; tau <= max_lag; tau++) {
        running_sum += d[tau];
        d_prime[tau] = (running_sum > 1e-9f) ? (d[tau] * (float)tau / running_sum) : 1.0f;
    }

    int tau_est = min_lag; // always a valid, initialized d_prime[] index by construction (see the clamp above)
    bool found = false;
    for (int tau = min_lag; tau <= max_lag; tau++) {
        if (d_prime[tau] < THRESHOLD) {
            while (tau + 1 <= max_lag && d_prime[tau + 1] < d_prime[tau]) tau++;
            tau_est = tau;
            found = true;
            break;
        }
    }
    if (!found) {
        int best = min_lag;
        for (int tau = min_lag + 1; tau <= max_lag; tau++) {
            if (d_prime[tau] < d_prime[best]) best = tau;
        }
        tau_est = best;
    }

    float tau_refined = (float)tau_est;
    if (tau_est > min_lag && tau_est < max_lag) {
        float s0 = d_prime[tau_est - 1], s1 = d_prime[tau_est], s2 = d_prime[tau_est + 1];
        float denom = s0 - 2.0f * s1 + s2;
        if (fabsf(denom) > 1e-9f) {
            float delta = 0.5f * (s0 - s2) / denom;
            tau_refined += fmaxf(-1.0f, fminf(1.0f, delta));
        }
    }

    *out_confidence = 1.0f - fminf(1.0f, d_prime[tau_est]);
    return (float)SAMPLE_RATE / fmaxf(1.0f, tau_refined);
}

// Scale membership tables (1 = in-scale semitone, indexed 0-11 from the root) -- Chromatic accepts
// everything (Key is meaningless in that mode, matching every real pitch corrector's own behavior).
static const int8_t HARDTUNE_SCALE_TABLES[5][12] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, // Chromatic
    {1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1}, // Major
    {1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0}, // Natural Minor
    {1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 0}, // Major Pentatonic
    {1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0}, // Minor Pentatonic
};

// Nearest in-scale MIDI note to a (possibly fractional) detected MIDI note number -- searches
// outward by semitone (0, +1, -1, +2, -2, ...) since every one of the 5 scales above has a member
// within a handful of semitones of any starting point.
static float hardtune_snap_to_scale(float midi_note, int key, int scale_idx) {
    int nearest = (int)floorf(midi_note + 0.5f);
    for (int offset = 0; offset <= 6; offset++) {
        int up = nearest + offset;
        int pc_up = ((up - key) % 12 + 12) % 12;
        if (HARDTUNE_SCALE_TABLES[scale_idx][pc_up]) return (float)up;
        if (offset > 0) {
            int down = nearest - offset;
            int pc_down = ((down - key) % 12 + 12) % 12;
            if (HARDTUNE_SCALE_TABLES[scale_idx][pc_down]) return (float)down;
        }
    }
    return (float)nearest; // unreachable for these 5 tables (all have members within +-6 semitones)
}

// Voice Range presets: (min_hz, max_hz) search windows -- deliberately generous, pragmatic bounds
// (like the real hardware/software presets this mirrors), not strict vocal-science boundaries.
static const float HARDTUNE_VOICE_RANGE_HZ[5][2] = {
    { 70.0f, 350.0f },  // Bass
    { 90.0f, 500.0f },  // Tenor
    { 150.0f, 700.0f }, // Alto
    { 180.0f, 1100.0f },// Soprano
    { 70.0f, 1000.0f }, // Wide
};

typedef struct {
    float buf[2][FX_HARDTUNE_BUF_SIZE]; // [channel][sample], circular via write_pos & MASK
    uint32_t write_pos;

    float yin_window[FX_HARDTUNE_YIN_WINDOW]; // mono-summed scratch, refilled every hop
    int hop_counter;

    float tracked_f0;        // Tracking-Speed-smoothed detected pitch, Hz (held during unvoiced gaps)
    float grain_period;      // current PSOLA grain period in samples, from tracked_f0 (held likewise)
    bool voiced;

    float hop_target_ratio;  // this hop's correction goal (1.0 = none), before Retune Speed smoothing
    float target_ratio;      // Retune-Speed-smoothed ratio actually applied, updated every sample
    float phase;             // 0..grain_period, advances by target_ratio every sample

    float vibrato_phase;     // 0..1, this effect's own dedicated vibrato LFO (see its own comment)

    bool initialized;
} FxHardTuneState;

// Reads a small, BOUNDED (see this effect's own top comment) offset behind the write pointer with
// linear interpolation. Deliberately does NOT convert the absolute (ever-growing) write_pos to a
// float directly -- past roughly 2^24 samples (~5.8 minutes at 48kHz) that would silently lose
// precision and introduce read-position jitter. Only the small bounded `rel_offset` (always within
// a few hundred samples) goes through float math; the actual buffer index stays pure integer/
// unsigned arithmetic, which wraps correctly (and indefinitely) via 2's-complement modular
// arithmetic regardless of how long this effect keeps running.
static float hardtune_read_interp(const float *chan_buf, uint32_t write_pos, float rel_offset) {
    float floor_offset = floorf(rel_offset);
    int32_t int_offset = (int32_t)floor_offset;
    float frac = rel_offset - floor_offset;
    uint32_t base_idx = (uint32_t)((int32_t)write_pos + int_offset);
    float s0 = chan_buf[base_idx & FX_HARDTUNE_BUF_MASK];
    float s1 = chan_buf[(base_idx + 1) & FX_HARDTUNE_BUF_MASK];
    return s0 + (s1 - s0) * frac;
}

static void run_fx_hardtune(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxHardTuneState *st = (FxHardTuneState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxHardTuneState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) {
        st->tracked_f0 = 200.0f; // arbitrary reasonable starting guess, overwritten by the first real detection
        st->grain_period = (float)SAMPLE_RATE / st->tracked_f0;
        st->target_ratio = 1.0f;
        st->hop_target_ratio = 1.0f;
        st->phase = 0.0f;
        st->voiced = false;
        st->initialized = true;
    }

    const int key = (int)(fmaxf(0.0f, fminf(1.0f, params->param[0])) * 11.0f + 0.5f);
    const int scale_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[1])) * 4.0f + 0.5f);
    const bool midi_target = params->param[2] >= 0.5f; // Target Source: Scale/Key (0) vs MIDI (1)
    const float amount = fmaxf(0.0f, fminf(1.0f, params->param[3]));
    const float retune_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[4])) * 199.9f;   // 0.1..200ms
    const float tracking_ms = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[5])) * 499.9f; // 0.1..500ms
    int voice_range_idx = (int)(fmaxf(0.0f, fminf(1.0f, params->param[6])) * 4.0f + 0.5f);
    if (voice_range_idx < 0) voice_range_idx = 0; else if (voice_range_idx > 4) voice_range_idx = 4;
    const float vibrato_rate_hz = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[7])) * 9.9f;  // 0.1..10Hz
    const float vibrato_depth_semitones = fmaxf(0.0f, fminf(1.0f, params->param[8])) * 1.5f; // 0..1.5 semitones peak
    const int vibrato_mod_choice = (int)(fmaxf(0.0f, fminf(1.0f, params->param[9])) * 2.0f + 0.5f); // 0=None,1=Mod Wheel,2=Aftertouch
    const float range_min_hz = HARDTUNE_VOICE_RANGE_HZ[voice_range_idx][0];
    const float range_max_hz = HARDTUNE_VOICE_RANGE_HZ[voice_range_idx][1];
    int min_lag = (int)((float)SAMPLE_RATE / range_max_hz);
    int max_lag = (int)((float)SAMPLE_RATE / range_min_hz);
    if (min_lag < FX_HARDTUNE_MIN_LAG) min_lag = FX_HARDTUNE_MIN_LAG;
    if (max_lag > FX_HARDTUNE_MAX_LAG) max_lag = FX_HARDTUNE_MAX_LAG;

    // Write this sample into both channels' delay lines.
    uint32_t idx = st->write_pos & FX_HARDTUNE_BUF_MASK;
    st->buf[0][idx] = *l;
    st->buf[1][idx] = *r;

    // --- Analysis: YIN once per hop, on a mono-summed window of the most recent YIN_WINDOW samples ---
    st->hop_counter++;
    if (st->hop_counter >= FX_HARDTUNE_YIN_HOP) {
        st->hop_counter = 0;
        float peak = 0.0f;
        for (int i = 0; i < FX_HARDTUNE_YIN_WINDOW; i++) {
            uint32_t src = (uint32_t)((int32_t)st->write_pos - (FX_HARDTUNE_YIN_WINDOW - 1 - i));
            float mono = 0.5f * (st->buf[0][src & FX_HARDTUNE_BUF_MASK] + st->buf[1][src & FX_HARDTUNE_BUF_MASK]);
            st->yin_window[i] = mono;
            peak = fmaxf(peak, fabsf(mono));
        }

        float confidence = 0.0f;
        float detected_hz = hardtune_yin_detect(st->yin_window, FX_HARDTUNE_YIN_WINDOW, min_lag, max_lag, &confidence);
        // Voiced gate: a real level floor (skip near-silence entirely, matching every other effect's
        // own "nothing meaningful to process" posture) AND YIN's own confidence, AND the result must
        // actually land inside this call's search range (a sanity check against the parabolic
        // interpolation nudging a boundary lag estimate just outside it).
        st->voiced = (peak > 0.003f) && (confidence > 0.5f) && detected_hz >= range_min_hz * 0.98f && detected_hz <= range_max_hz * 1.02f;

        if (st->voiced) {
            // Tracking Speed: one-pole smoother in log-frequency domain, updated once per hop (this
            // smoother's own "sample rate" is hops/second, not SAMPLE_RATE -- see its own rate calc).
            const float tracking_hop_rate_hz = (float)SAMPLE_RATE / (float)FX_HARDTUNE_YIN_HOP;
            const float tracking_rate = 1.0f - expf(-1.0f / (0.001f * tracking_ms * tracking_hop_rate_hz));
            float log_tracked = log2f(st->tracked_f0) + (log2f(detected_hz) - log2f(st->tracked_f0)) * tracking_rate;
            st->tracked_f0 = exp2f(log_tracked);
            float period = (float)SAMPLE_RATE / st->tracked_f0;
            st->grain_period = fmaxf((float)min_lag, fminf((float)max_lag, period));
        }
        // Unvoiced: hold tracked_f0/grain_period at their last good values (avoids a spurious jump
        // from noisy detection during a breath/consonant/silence).

        float raw_target_ratio = 1.0f;
        if (st->voiced) {
            if (midi_target) {
                // g_hardtune_held_note (see its own comment) is this engine-wide monophonic
                // last-note-priority tracker, updated by handle_note_on/handle_note_off regardless
                // of which Program a note targets -- if nothing is currently held, fall back to no
                // correction, same posture as an unvoiced input.
                if (g_hardtune_held_note >= 0.0f) {
                    float midi = 69.0f + 12.0f * log2f(st->tracked_f0 / 440.0f);
                    raw_target_ratio = exp2f((g_hardtune_held_note - midi) / 12.0f);
                }
            } else {
                float midi = 69.0f + 12.0f * log2f(st->tracked_f0 / 440.0f);
                float target_midi = hardtune_snap_to_scale(midi, key, scale_idx);
                raw_target_ratio = exp2f((target_midi - midi) / 12.0f);
            }
        }
        // Amount blends in log/semitone space (perceptually correct interpolation, not linear ratio).
        st->hop_target_ratio = exp2f(log2f(raw_target_ratio) * amount);
    }

    // --- Per-sample: Retune Speed glides target_ratio toward this hop's goal ---
    const float retune_rate = 1.0f - expf(-1.0f / (0.001f * retune_ms * (float)SAMPLE_RATE));
    float log_cur = log2f(st->target_ratio) + (log2f(st->hop_target_ratio) - log2f(st->target_ratio)) * retune_rate;
    st->target_ratio = exp2f(log_cur);

    // Live vibrato: this effect's own dedicated sine LFO (a per-slot instance, not the per-Program
    // LFO matrix -- HardTune is a global FX effect with no single Program/voice of its own to hang
    // a routing-matrix destination off of, same reason the MIDI note tracker/pitch bend above are
    // plain globals rather than routed through that matrix). Optional Mod Source scaling (Mod Wheel
    // CC1 or channel aftertouch) gates Depth the same way PackLfoParams.mod_source_cc gates a
    // regular LFO's depth -- unassigned (None, the default) is a pure no-op at full knob depth.
    // Like pitch bend, this is a FINAL multiplicative adjustment on top of the already-corrected
    // ratio, computed after Retune Speed's own smoothing and never fed back into detection/
    // quantization above, so it can never fight the correction either.
    float vibrato_cc_scale = 1.0f;
    if (vibrato_mod_choice == 1) vibrato_cc_scale = g_hardtune_modwheel;
    else if (vibrato_mod_choice == 2) vibrato_cc_scale = g_hardtune_channel_pressure;
    const float vibrato_semitones = sinf(2.0f * (float)M_PI * st->vibrato_phase) * vibrato_depth_semitones * vibrato_cc_scale;
    st->vibrato_phase += vibrato_rate_hz / (float)SAMPLE_RATE;
    if (st->vibrato_phase >= 1.0f) st->vibrato_phase -= 1.0f;

    // Live pitch bend (see g_hardtune_bend_semitones's own comment): a final multiplicative
    // adjustment layered on TOP of the already-corrected ratio, unconditionally -- regardless of
    // Target Source, and never fed back into detection/quantization above, so it can never fight
    // the correction. Not smoothed here beyond whatever smoothing the MIDI pitch-bend wheel's own
    // physical motion already has -- a real-time performance control, not a quantized target.
    const float output_ratio = st->target_ratio * exp2f((g_hardtune_bend_semitones + vibrato_semitones) / 12.0f);

    // --- PSOLA resynthesis: two Hann-crossfaded taps, phase wraps mod grain_period (see this
    // effect's own top comment for why this never drifts unboundedly even under a sustained
    // non-1.0 ratio) ---
    // Phase advances by (output_ratio - 1), NOT output_ratio: the actual buffer read position is
    // write_pos + offset, and write_pos ITSELF already advances by 1 every sample (real time) --
    // phase only needs to supply the DIFFERENCE from real-time so the read position's own total
    // rate of advance through the buffer's content comes out to exactly output_ratio (1 from
    // write_pos's own motion + (output_ratio-1) from phase = output_ratio). At output_ratio=1 this
    // correctly makes phase never move at all: a fixed offset behind write_pos is already a
    // transparent, undelayed-in-pitch copy, needing no wrapping/resets whatsoever.
    st->phase += (output_ratio - 1.0f);
    if (st->phase >= st->grain_period) st->phase -= st->grain_period;
    if (st->phase < 0.0f) st->phase += st->grain_period; // pitch-DOWN corrections drift phase negative

    const float frac_a = st->phase / st->grain_period;
    float frac_b = frac_a + 0.5f;
    if (frac_b >= 1.0f) frac_b -= 1.0f;
    const float win_a = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * frac_a);
    const float win_b = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * frac_b);

    const float offset_a = -(float)FX_HARDTUNE_PSOLA_DELAY + (frac_a * st->grain_period - st->grain_period * 0.5f);
    const float offset_b = -(float)FX_HARDTUNE_PSOLA_DELAY + (frac_b * st->grain_period - st->grain_period * 0.5f);

    *l = hardtune_read_interp(st->buf[0], st->write_pos, offset_a) * win_a
       + hardtune_read_interp(st->buf[0], st->write_pos, offset_b) * win_b;
    *r = hardtune_read_interp(st->buf[1], st->write_pos, offset_a) * win_a
       + hardtune_read_interp(st->buf[1], st->write_pos, offset_b) * win_b;

    st->write_pos++;
}

// --- FX_TYPE_FDNVERB: see its own #define comment above for the full design, provenance, and the
// two dead reference-source params deliberately not carried over. Rate ratio is fixed at compile
// time (unlike the reference's own runtime sampleRate) since this engine's own SAMPLE_RATE always
// is -- every delay length below is the reference's own 44100Hz table scaled by this ratio once.
static const float FX_FDNVERB_RATE_RATIO = (float)SAMPLE_RATE / 44100.0f;

#define FX_FDNVERB_NUM_DELAYS 8
// Power of 2 for cheap masking; comfortably covers this effect's own largest possible base delay
// (2251 samples @ 44100Hz * this engine's rate ratio * Size's own max 1.5x scale, ~3674 samples
// @ 48kHz) plus modulation's own few-sample wobble, with real margin -- deliberately NOT the
// reference's own wasteful flat "2 seconds" allocation, which this design never needs (see the
// max-base-delay arithmetic above).
#define FX_FDNVERB_BUF_SIZE 4096
#define FX_FDNVERB_BUF_MASK (FX_FDNVERB_BUF_SIZE - 1)
#define FX_FDNVERB_NUM_DIFFUSERS 4
// Covers every scaled diffuser delay used below (input diffusers up to ~195 samples, Early
// Reflections' own up to ~178) with real margin -- these are small, fixed, never-modulated
// integer delays (a true allpass comb, not a fractional/interpolated read), so plain modulo
// indexing against each instance's own capacity (not a power-of-2 mask) matches the reference
// exactly.
#define FX_FDNVERB_SMALL_DIFFUSER_CAP 256
#define FX_FDNVERB_ER_TAPS 16
#define FX_FDNVERB_ER_DIFFUSERS 2
// Power of 2 covering the largest scaled Early Reflections tap delay (87.5ms reference tap *
// Size's own max 1.5x scale * 48 samples/ms, ~6300 samples) with real margin.
#define FX_FDNVERB_ER_BUF_SIZE 8192
#define FX_FDNVERB_ER_BUF_MASK (FX_FDNVERB_ER_BUF_SIZE - 1)
// Power of 2 covering this effect's own max Pre-Delay (250ms * 48kHz = 12000 samples) with margin.
#define FX_FDNVERB_PREDELAY_BUF_SIZE 16384
#define FX_FDNVERB_PREDELAY_MASK (FX_FDNVERB_PREDELAY_BUF_SIZE - 1)

// Hadamard 8x8 matrix, pre-normalized by 1/sqrt(8) -- energy-preserving (orthogonal) mixing, the
// same matrix the reference uses verbatim.
static const float FX_FDNVERB_NORM = 0.35355339059327379f; // 1/sqrt(8)
static const float FX_FDNVERB_HADAMARD[FX_FDNVERB_NUM_DELAYS][FX_FDNVERB_NUM_DELAYS] = {
    { 1, 1, 1, 1, 1, 1, 1, 1},
    { 1,-1, 1,-1, 1,-1, 1,-1},
    { 1, 1,-1,-1, 1, 1,-1,-1},
    { 1,-1,-1, 1, 1,-1,-1, 1},
    { 1, 1, 1, 1,-1,-1,-1,-1},
    { 1,-1, 1,-1,-1, 1,-1, 1},
    { 1, 1,-1,-1,-1,-1, 1, 1},
    { 1,-1,-1, 1,-1, 1, 1,-1},
}; // each entry above is implicitly * FX_FDNVERB_NORM (applied once at point of use, not stored
   // 64 times over -- the reference stores it pre-multiplied; this is numerically identical)

// Prime-length delays at 44100Hz (20-51ms range) -- scaled by FX_FDNVERB_RATE_RATIO and Size's own
// sizeScale at point of use, since Size can change live.
static const int FX_FDNVERB_BASE_DELAYS_44100[FX_FDNVERB_NUM_DELAYS] = {
    887, 1097, 1361, 1559, 1709, 1867, 2053, 2251
};
// Per-delay-line LFO rate multipliers and starting phases (radians) -- fixed, decorrelated offsets
// so all 8 lines' modulation never lines up, the reference's own anti-metallic-ringing trick.
static const float FX_FDNVERB_LFO_RATES[FX_FDNVERB_NUM_DELAYS] = {
    0.31f, 0.37f, 0.43f, 0.53f, 0.61f, 0.71f, 0.79f, 0.89f
};
static const float FX_FDNVERB_LFO_PHASES[FX_FDNVERB_NUM_DELAYS] = {
    0.0f, 0.79f, 1.57f, 2.36f, 3.14f, 3.93f, 4.71f, 5.50f
};
// Input diffuser delays at 44100Hz (prime), chained in series ahead of the FDN's own 8 lines.
static const int FX_FDNVERB_DIFFUSER_DELAYS_44100[FX_FDNVERB_NUM_DIFFUSERS] = {53, 89, 127, 179};

// Early Reflections: 16 taps, different L/R times for stereo imaging, alternating polarity gains
// simulating real wall reflections (odd bounce counts invert, same idea a real room's own early
// echoes have). All in ms at 44100Hz-equivalent (scaled by Size, like everything else here).
static const float FX_FDNVERB_ER_TAP_MS_L[FX_FDNVERB_ER_TAPS] = {
    3.5f, 7.1f, 11.2f, 15.7f, 19.3f, 24.1f, 29.8f, 33.5f,
    38.7f, 43.2f, 49.1f, 55.3f, 61.7f, 68.4f, 76.2f, 85.0f
};
static const float FX_FDNVERB_ER_TAP_MS_R[FX_FDNVERB_ER_TAPS] = {
    5.2f, 9.3f, 13.8f, 17.1f, 22.0f, 26.4f, 31.5f, 36.2f,
    41.0f, 46.8f, 52.3f, 58.1f, 64.5f, 71.9f, 79.0f, 87.5f
};
static const float FX_FDNVERB_ER_TAP_GAIN_BASE[FX_FDNVERB_ER_TAPS] = {
    0.85f, -0.72f, 0.65f, -0.58f, 0.52f, -0.46f, 0.41f, -0.37f,
    0.33f, -0.29f, 0.26f, -0.23f, 0.20f, -0.17f, 0.15f, -0.13f
};
// Early Reflections' own 2 input diffusers per channel (different L/R delays for stereo).
static const int FX_FDNVERB_ER_DIFFUSER_DELAYS_L[FX_FDNVERB_ER_DIFFUSERS] = {75, 137};
static const int FX_FDNVERB_ER_DIFFUSER_DELAYS_R[FX_FDNVERB_ER_DIFFUSERS] = {93, 163};

// A small, fixed-integer-delay allpass comb -- used for both the FDN's own input diffusers and
// Early Reflections' own per-channel diffusers (identical structure in the reference, just
// different delay tables). Unlike the FDN's own modulated delay lines, delaySamples here is a
// fixed integer set once at init, never modulated, so plain modulo indexing (against this
// instance's own capacity) needs no interpolation -- matches the reference's AllpassFilter exactly.
typedef struct {
    float buf[FX_FDNVERB_SMALL_DIFFUSER_CAP];
    int cap;           // buffer capacity actually in use (delay_samples + 4, <= the array's own fixed size)
    int write_pos;
    int delay_samples;
    float coeff;
} FdnverbAllpass;

static float fdnverb_allpass_process(FdnverbAllpass *ap, float input) {
    int read_pos = ap->write_pos - ap->delay_samples;
    read_pos %= ap->cap; if (read_pos < 0) read_pos += ap->cap;
    float delayed = ap->buf[read_pos];
    float output = delayed - ap->coeff * input;
    ap->buf[ap->write_pos] = input + ap->coeff * delayed;
    ap->write_pos = (ap->write_pos + 1) % ap->cap;
    return output;
}

// Bounded-offset interpolated circular-buffer read, identical in spirit to hardtune_read_interp
// (see its own comment on why the small relative offset -- never the absolute, ever-growing
// write_pos -- is what goes through float math) but against this effect's own FX_FDNVERB_BUF_MASK,
// kept as this effect's own small helper rather than reaching into HardTune's private one so a
// future change to either effect's own buffer size can never silently affect the other.
static float fdnverb_read_interp(const float *buf, uint32_t write_pos, float rel_offset) {
    float floor_offset = floorf(rel_offset);
    int32_t int_offset = (int32_t)floor_offset;
    float frac = rel_offset - floor_offset;
    uint32_t base_idx = (uint32_t)((int32_t)write_pos + int_offset);
    float s0 = buf[base_idx & FX_FDNVERB_BUF_MASK];
    float s1 = buf[(base_idx + 1) & FX_FDNVERB_BUF_MASK];
    return s0 + (s1 - s0) * frac;
}

typedef struct {
    // --- Pre-delay: plain stereo ring buffer, interpolated read (Pre-Delay is knob-adjustable
    // live, so the read offset is a float, not a fixed integer) ---
    float predelay_buf[2][FX_FDNVERB_PREDELAY_BUF_SIZE];
    uint32_t predelay_write_pos;

    // --- FDN core ---
    float fdn_buf[FX_FDNVERB_NUM_DELAYS][FX_FDNVERB_BUF_SIZE];
    uint32_t fdn_write_pos;  // shared: all 8 lines advance in lockstep, exactly 1/sample, like the reference
    float fdn_lfo_phase[FX_FDNVERB_NUM_DELAYS];
    float fdn_damping_lp_state[FX_FDNVERB_NUM_DELAYS]; // one-pole HF-damping filter, in the feedback path
    FdnverbAllpass input_diffusers[FX_FDNVERB_NUM_DIFFUSERS];

    // --- Early Reflections ---
    float er_buf[2][FX_FDNVERB_ER_BUF_SIZE];
    uint32_t er_write_pos; // shared L/R, same lockstep reasoning as fdn_write_pos
    FdnverbAllpass er_diffusers[2][FX_FDNVERB_ER_DIFFUSERS]; // [channel][diffuser]
    int er_tap_delay[2][FX_FDNVERB_ER_TAPS];
    float er_tap_gain[2][FX_FDNVERB_ER_TAPS];

    bool initialized;
} FxFdnverbState;

static void run_fx_fdnverb(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxFdnverbState *st = (FxFdnverbState *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxFdnverbState));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) {
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) st->fdn_lfo_phase[i] = FX_FDNVERB_LFO_PHASES[i];
        for (int i = 0; i < FX_FDNVERB_NUM_DIFFUSERS; i++) {
            int d = (int)(FX_FDNVERB_DIFFUSER_DELAYS_44100[i] * FX_FDNVERB_RATE_RATIO);
            st->input_diffusers[i].delay_samples = d;
            st->input_diffusers[i].cap = d + 4;
        }
        for (int ch = 0; ch < 2; ch++) {
            const int *dl = (ch == 0) ? FX_FDNVERB_ER_DIFFUSER_DELAYS_L : FX_FDNVERB_ER_DIFFUSER_DELAYS_R;
            for (int i = 0; i < FX_FDNVERB_ER_DIFFUSERS; i++) {
                int d = (int)(dl[i] * FX_FDNVERB_RATE_RATIO);
                st->er_diffusers[ch][i].delay_samples = d;
                st->er_diffusers[ch][i].cap = d + 4;
            }
        }
        st->initialized = true;
    }

    const float rt60 = 0.3f * powf(12.0f / 0.3f, fmaxf(0.0f, fminf(1.0f, params->param[0])));   // 0.3..12s, log taper
    const float room_size = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    const float predelay_ms = fmaxf(0.0f, fminf(1.0f, params->param[2])) * 250.0f;
    const float damping = fmaxf(0.0f, fminf(1.0f, params->param[3]));
    const float diffusion = fmaxf(0.0f, fminf(1.0f, params->param[4]));
    const float mod_rate_hz = 0.1f + fmaxf(0.0f, fminf(1.0f, params->param[5])) * 9.9f;
    const float mod_depth = fmaxf(0.0f, fminf(1.0f, params->param[6]));
    const float early_level = fmaxf(0.0f, fminf(1.0f, params->param[7]));
    const float early_decay = fmaxf(0.0f, fminf(1.0f, params->param[8]));
    const bool freeze = params->param[9] >= 0.5f;

    // --- Pre-delay ---
    uint32_t pd_idx = st->predelay_write_pos & FX_FDNVERB_PREDELAY_MASK;
    st->predelay_buf[0][pd_idx] = *l;
    st->predelay_buf[1][pd_idx] = *r;
    const float predelay_samples = predelay_ms * (float)SAMPLE_RATE / 1000.0f;
    // Reuses fdnverb_read_interp's own bounded-offset trick even though this buffer's mask
    // differs -- the function only ever masks with FX_FDNVERB_BUF_MASK, so a raw manual
    // interpolated read is done here instead rather than parametrizing the mask for one call site.
    {
        float floor_off = floorf(-predelay_samples);
        int32_t int_off = (int32_t)floor_off;
        float frac = -predelay_samples - floor_off;
        uint32_t base = (uint32_t)((int32_t)st->predelay_write_pos + int_off);
        float l0 = st->predelay_buf[0][base & FX_FDNVERB_PREDELAY_MASK];
        float l1 = st->predelay_buf[0][(base + 1) & FX_FDNVERB_PREDELAY_MASK];
        float r0 = st->predelay_buf[1][base & FX_FDNVERB_PREDELAY_MASK];
        float r1 = st->predelay_buf[1][(base + 1) & FX_FDNVERB_PREDELAY_MASK];
        *l = l0 + (l1 - l0) * frac;
        *r = r0 + (r1 - r0) * frac;
    }
    st->predelay_write_pos++;
    const float predelayed_l = *l, predelayed_r = *r;

    // --- Early Reflections (operates on the pre-delayed signal, see this effect's own top
    // comment for why Level applies once, to dry+taps together, rather than the reference's own
    // double-counted version) ---
    {
        float size_scale = 0.3f + room_size * 1.2f;
        float ms_to_samples = (float)SAMPLE_RATE / 1000.0f;
        for (int t = 0; t < FX_FDNVERB_ER_TAPS; t++) {
            int dl = (int)(FX_FDNVERB_ER_TAP_MS_L[t] * size_scale * ms_to_samples); if (dl < 1) dl = 1;
            int dr = (int)(FX_FDNVERB_ER_TAP_MS_R[t] * size_scale * ms_to_samples); if (dr < 1) dr = 1;
            st->er_tap_delay[0][t] = dl;
            st->er_tap_delay[1][t] = dr;
            float time_norm = (float)t / (float)(FX_FDNVERB_ER_TAPS - 1);
            float decay_factor = powf(1.0f - early_decay * 0.6f, time_norm * 3.0f);
            st->er_tap_gain[0][t] = FX_FDNVERB_ER_TAP_GAIN_BASE[t] * decay_factor;
            st->er_tap_gain[1][t] = FX_FDNVERB_ER_TAP_GAIN_BASE[t] * decay_factor;
        }
    }
    float er_result[2];
    for (int ch = 0; ch < 2; ch++) {
        float in = (ch == 0) ? predelayed_l : predelayed_r;
        float diffused = in;
        for (int i = 0; i < FX_FDNVERB_ER_DIFFUSERS; i++) diffused = fdnverb_allpass_process(&st->er_diffusers[ch][i], diffused);
        uint32_t idx = st->er_write_pos & FX_FDNVERB_ER_BUF_MASK;
        st->er_buf[ch][idx] = diffused;
        float tap_sum = 0.0f;
        for (int t = 0; t < FX_FDNVERB_ER_TAPS; t++) {
            uint32_t ridx = (uint32_t)((int32_t)st->er_write_pos - st->er_tap_delay[ch][t]) & FX_FDNVERB_ER_BUF_MASK;
            tap_sum += st->er_buf[ch][ridx] * st->er_tap_gain[ch][t];
        }
        er_result[ch] = (in + tap_sum) * early_level;
    }
    st->er_write_pos++;

    // --- FDN8x8 late reverb (operates on the SAME pre-delayed signal) ---
    float fdn_out_l, fdn_out_r;
    {
        float mono_in = (predelayed_l + predelayed_r) * 0.5f;
        float diff_coeff = 0.3f + diffusion * 0.4f;
        for (int i = 0; i < FX_FDNVERB_NUM_DIFFUSERS; i++) {
            st->input_diffusers[i].coeff = diff_coeff;
            mono_in = fdnverb_allpass_process(&st->input_diffusers[i], mono_in);
        }

        float size_scale = 0.3f + room_size * 1.2f;
        float lp_freq = 1000.0f + (1.0f - damping) * 14000.0f;
        lp_freq = fmaxf(200.0f, fminf((float)SAMPLE_RATE * 0.49f, lp_freq));
        float lp_coeff = expf(-2.0f * (float)M_PI * lp_freq / (float)SAMPLE_RATE);

        float delay_out[FX_FDNVERB_NUM_DELAYS];
        float base_delay[FX_FDNVERB_NUM_DELAYS];
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            base_delay[i] = (float)FX_FDNVERB_BASE_DELAYS_44100[i] * FX_FDNVERB_RATE_RATIO * size_scale;
            // Phase advances THEN is read this same sample, matching the reference's own order.
            float rate_hz = FX_FDNVERB_LFO_RATES[i] * mod_rate_hz;
            st->fdn_lfo_phase[i] += 2.0f * (float)M_PI * rate_hz / (float)SAMPLE_RATE;
            if (st->fdn_lfo_phase[i] >= 2.0f * (float)M_PI) st->fdn_lfo_phase[i] -= 2.0f * (float)M_PI;
            float depth_samples = 0.5f + mod_depth * 3.0f;
            float mod_offset = sinf(st->fdn_lfo_phase[i]) * depth_samples;
            delay_out[i] = fdnverb_read_interp(st->fdn_buf[i], st->fdn_write_pos, -base_delay[i] + mod_offset);
        }

        float filtered[FX_FDNVERB_NUM_DELAYS];
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            st->fdn_damping_lp_state[i] = delay_out[i] * (1.0f - lp_coeff) + st->fdn_damping_lp_state[i] * lp_coeff;
            float feedback_gain = freeze ? 1.0f : powf(10.0f, -3.0f * base_delay[i] / (rt60 * (float)SAMPLE_RATE));
            filtered[i] = st->fdn_damping_lp_state[i] * feedback_gain;
        }

        float mixed[FX_FDNVERB_NUM_DELAYS];
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            float matrix_sum = 0.0f;
            for (int j = 0; j < FX_FDNVERB_NUM_DELAYS; j++) matrix_sum += FX_FDNVERB_HADAMARD[i][j] * FX_FDNVERB_NORM * filtered[j];
            mixed[i] = (1.0f - diffusion) * filtered[i] + diffusion * matrix_sum;
        }

        uint32_t widx = st->fdn_write_pos & FX_FDNVERB_BUF_MASK;
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            float new_sample = tanhf(mixed[i] + mono_in * 0.25f);
            if (!freeze) st->fdn_buf[i][widx] = new_sample;
        }
        st->fdn_write_pos++;

        fdn_out_l = 0.0f; fdn_out_r = 0.0f;
        for (int i = 0; i < FX_FDNVERB_NUM_DELAYS; i++) {
            if (i % 2 == 0) { fdn_out_l += delay_out[i]; fdn_out_r -= delay_out[i] * 0.25f; }
            else            { fdn_out_r += delay_out[i]; fdn_out_l -= delay_out[i] * 0.25f; }
        }
        fdn_out_l *= 0.2f; fdn_out_r *= 0.2f;
    }

    *l = er_result[0] + fdn_out_l;
    *r = er_result[1] + fdn_out_r;
}

// Runs the FX_SLOT_COUNT-slot FX bus in place on one stereo sample. Called once per sample
// (not per voice) on the fully summed mix -- see render_audio_block, where this sits after all
// voices (and the external input) have been added together, and before master_volume is
// applied. Ported effects land as new cases in the inner switch; each one reads/writes its own
// fx_engine[s].state (lazily allocating it on first call after a type change) and is
// responsible for freeing it if it allocated anything, via a matching branch in the "type
// changed" block below. Until effects are ported in, every fx_type other than Off is unreached
// and simply passes the dry signal through unchanged, same as Off.
//
// routing_mode selects how source_id's OWN assigned slots combine. Both of this file's callers
// (backend.c's render_audio_block, hds_fx_plugin.c's process()) always pass 1 (Parallel) --
// neither exposes a way to pick Series anymore (chain_to_next fully expresses what Series used to
// mean, see its own comment on FXSlotParams in fx_types.h), so mode 0 below is effectively dead
// code, kept only because this function's signature still accepts it: 0 = Series, each of this
// source's slots fed the previous one's output (in physical slot-array order -- a slot not
// assigned to this source is simply skipped,
// so Series order among a source's own slots is still exactly array order), *l/*r updated in
// place after every owned slot. FXSlotParams.chain_to_next is entirely IGNORED in this mode --
// every slot is already chained unconditionally, so there's nothing left for it to express.
//
// 1 = Parallel, GROUPED: each owned slot starts a fresh "branch" fed by the SAME original dry
// input, UNLESS the previous owned slot had chain_to_next set, in which case this slot instead
// continues that branch -- fed by the previous slot's own output, not the shared dry input --
// exactly like Series would, just scoped to this one branch. A branch closes (and its final
// result is added into the running sum) the moment a slot doesn't continue it: either the next
// owned slot didn't ask to (chain_to_next left off, the default), or there is no next owned slot.
// The sum is then divided by the number of BRANCHES that closed, not the number of raw slots that
// participated -- chaining N slots together still counts as exactly one branch in the average, the
// same weight a single unchained slot would carry, matching how a parallel mixer bus should treat
// "one processed signal" whether it took one plugin or three to get there. An Off/bypassed slot
// still passes its own dry input through as its output unchanged (same "contributes its own
// unprocessed dry copy" rule this mode has always had -- unlike the flat filter chain's own
// Parallel mode, see render_audio_block's filter dispatch, where an Off slot is excluded from the
// sum entirely) and the branch it's part of continues exactly as if it had done real work.
// With no slot anywhere setting chain_to_next (every FXSlotParams' own default), every owned slot
// opens and immediately closes its own one-slot branch -- reproducing today's original Parallel
// behavior byte-for-byte, just computed through the more general branch bookkeeping.
//
// Both modes: a source using only 1 or 2 slots in Parallel isn't attenuated as if all 12 had
// participated (the divisor is real branch count, not FX_SLOT_COUNT). A source that owns zero
// slots (Series or Parallel alike) is a correct no-op: *l/*r pass through unchanged.
//
// Called once per source per sample (not per voice) -- see render_audio_block, which loops this
// over every source (Program + each ext-in pair) on that source's own dry bus, after all voices
// have been summed (for the Program bus) and before that source's contribution is added into
// whichever output pair(s) it's routed to. fx_engine[] stays indexed by physical slot id
// regardless of which source owns a slot this block, so a slot's lazily-allocated DSP state
// (e.g. a delay's buffered tail) survives a live source reassignment exactly like it already
// survives a live bypass toggle.
// Type-change bookkeeping (free a slot's old effect state when its Type changed) -- a per-
// PHYSICAL-SLOT task, completely independent of which source is dispatching. Used to run inside
// process_fx_chain() itself, once per call; now a separate, explicitly-named step the CALLER runs
// once per block (see render_audio_block, right before its per-sample loop), since a slot's Type
// can only ever change in response to an OSC message drained between blocks, never mid-block --
// with SOURCE_COUNT sources each independently calling process_fx_chain() every single sample,
// redoing this scan from inside that function meant redoing it SOURCE_COUNT times per sample for
// no reason (a real, measured underrun regression at any voice count, since it's a flat per-
// sample tax paid regardless of patch complexity). PRECONDITION for process_fx_chain() below:
// this must be called at least once after any fx_type change and before that slot is dispatched --
// render_audio_block satisfies this once per block; a test or other direct caller driving
// process_fx_chain() outside render_audio_block must call this itself first.
static void sync_fx_engine_types(const FXSlotParams fx_params[FX_SLOT_COUNT], FXSlotEngine fx_engine[FX_SLOT_COUNT]) {
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        int type = (int)fx_params[s].fx_type;
        if (type != fx_engine[s].active_type) {
            // FX_TYPE_NAM's state owns real C++ objects (via the bridge) that a bare free() would
            // leak rather than destroy -- see fx_nam_free_state()'s own comment. Every other
            // effect's state is plain data, safe for the generic free() below exactly as before.
#if HDS_NAM_ENABLED
            if (fx_engine[s].active_type == FX_TYPE_NAM && fx_engine[s].state) {
                fx_nam_free_state((FxNamState *)fx_engine[s].state);
            }
#endif
            free(fx_engine[s].state);
            fx_engine[s].state = NULL;
            fx_engine[s].active_type = type;
        }
    }
}

// sc_snapshot_l/r: every source's own pre-FX dry signal THIS SAME SAMPLE, frozen before any
// source's own FX chain (including this call) has mutated its bus_l/bus_r in place -- see
// FXSlotParams.sidechain_source's own comment for why a live/mutating read would give a slot an
// order-dependent (sometimes pre-FX, sometimes post-FX) view of another source depending purely on
// which SOURCE_COUNT index happens to be processed first. Read-only here; only FX_TYPE_FAIRKID67
// currently uses it, via an explicit index lookup right after dry_l/dry_r below -- every other
// effect ignores it completely, same posture as any other per-slot field it doesn't read.
// gr_db_out[FX_SLOT_COUNT] (required, not NULL-able -- same convention as render_audio_block's own
// ext_in_peak_out): this call writes gr_db_out[s] for every slot s it owns (fx_params[s].fx_source
// == source_id), 0.0f for a bypassed/off/non-GR-capable slot, or that slot's own effect state's
// last_gr_db otherwise. A slot NOT owned by this source_id is left untouched by this call -- since
// render_audio_block calls this once per source and slots partition cleanly by fx_source, every
// slot ends up written by exactly the one call that owns it (see render_audio_block's own zeroing
// of the whole array before its source loop, which covers a source with zero owned slots).
// slot_out_l/r[FX_SLOT_COUNT] (required, not NULL-able, PERSISTENT across calls -- owned by the
// caller's own long-lived engine state, e.g. HdsEngineState/HdsFxPlugin, never a stack array):
// every slot s this call actually processes (owned or not -- see the Off/bypassed early-skip path
// below) gets its own just-computed post-mix output written here unconditionally, in POOL-SLOT
// order rather than per-source. This is what FXSlotParams.input_from_slot reads from (see that
// field's own comment) to let one slot's Input be explicitly another slot's real output, anywhere
// in the shared pool, independent of fx_source/chain_to_next's own same-source array-position
// adjacency. A slot whose OWN source hasn't run yet this sample simply still holds last sample's
// value here -- the same frozen-snapshot tradeoff sc_snapshot_l/r above already accepts.
static void process_fx_chain(const FXSlotParams fx_params[FX_SLOT_COUNT], FXSlotEngine fx_engine[FX_SLOT_COUNT], int routing_mode, int source_id, const float sc_snapshot_l[SOURCE_COUNT], const float sc_snapshot_r[SOURCE_COUNT], float *l, float *r, float gr_db_out[FX_SLOT_COUNT], float slot_out_l[FX_SLOT_COUNT], float slot_out_r[FX_SLOT_COUNT]) {
    // A NaN/Inf sample reaching a slot is never a valid audio value, but several ported effects'
    // own persistent state (e.g. run_fx_tapedelay2's sweepL/R, accumulated from squared input) has
    // no boundary check of its own -- once poisoned, a non-finite value can propagate into a
    // floorf()-derived array index, which is undefined behavior in C and a real crash/heap-
    // corruption vector, not just a silent glitch. Sanitized once here, at the one chokepoint every
    // slot's input already flows through, rather than in each of the ~20 ported effects individually.
    if (!isfinite(*l)) *l = 0.0f;
    if (!isfinite(*r)) *r = 0.0f;
    float parallel_in_l = *l, parallel_in_r = *r;
    float parallel_sum_l = 0.0f, parallel_sum_r = 0.0f;
    int parallel_branch_count = 0;

    // Parallel-only branch state (unused, and harmless, in Series mode): branch_l/r is the
    // running output of whichever branch is currently open; branch_open is false until the first
    // owned slot of a new branch runs; continue_branch is true when the PREVIOUS owned slot's own
    // chain_to_next asked THIS slot to continue its branch rather than start a fresh one.
    float branch_l = parallel_in_l, branch_r = parallel_in_r;
    bool branch_open = false;
    bool continue_branch = false;

    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        if ((int)fx_params[s].fx_source != source_id) continue; // not this source's slot
        int type = fx_engine[s].active_type;

        // Explicit cross-slot Input override (FXSlotParams.input_from_slot) always wins over
        // fx_source/chain_to_next when set -- see that field's own comment. Never trust a
        // self-reference even though the GUI already refuses to let one be picked: wire/state data
        // (a loaded project, a live OSC write) is never assumed sane just because the UI is.
        int input_from = (fx_params[s].input_from_slot >= 0.0f) ? (int)(fx_params[s].input_from_slot + 0.5f) : -1;
        if (input_from == s || input_from < 0 || input_from >= FX_SLOT_COUNT) input_from = -1;

        float dry_l, dry_r;
        if (input_from >= 0) {
            // Starts a fresh one-slot branch fed by another slot's own last-computed output,
            // completely independent of continue_branch -- this slot can never ALSO be a genuine
            // chain_to_next continuation target for real (the GUI only lets input_from_slot be set
            // on a chain head), so there's nothing to preserve by honoring continue_branch here.
            if (branch_open) { parallel_sum_l += branch_l; parallel_sum_r += branch_r; parallel_branch_count++; }
            dry_l = slot_out_l[input_from]; dry_r = slot_out_r[input_from];
            branch_open = true;
        } else if (routing_mode == 0) {
            dry_l = *l; dry_r = *r; // Series -- chain_to_next ignored, always the running chain result
        } else if (continue_branch) {
            dry_l = branch_l; dry_r = branch_r; // Parallel, continuing an already-open branch
        } else if (fx_params[s].bypass != 0.0f || type == FX_TYPE_OFF) {
            // Parallel, NOT continuing a branch, and this slot does nothing (Off or bypassed): a
            // pure dry-passthrough slot like this would otherwise open its own one-slot branch,
            // averaged in at full weight against every real effect sharing this source -- diluting
            // them proportionally to how many unused/Off slots merely happen to share this source,
            // never a musically intended result. This is exactly what a shared FX-slot pool makes
            // likely (e.g. HDS FX's own pool defaults every never-configured slot to the same
            // source) -- confirmed directly: a single real effect sharing a source with 31 default
            // Off slots was audible at roughly 1/32 its real level before this fix. Skip it
            // entirely instead: no branch opens, gr_db_out already defaults to 0.0f for it below,
            // and the NEXT owned slot starts fresh rather than "continuing" a branch that never
            // really existed. A source with ONLY Off/bypassed slots therefore still ends with
            // parallel_branch_count == 0 -- the exact same, already-correct "no owned slots"
            // passthrough this function's own finalization below already handles.
            gr_db_out[s] = 0.0f;
            continue_branch = false;
            slot_out_l[s] = parallel_in_l; slot_out_r[s] = parallel_in_r; // still publish something sane for any OTHER slot's input_from_slot pointed at this one
            continue;
        } else {
            if (branch_open) { parallel_sum_l += branch_l; parallel_sum_r += branch_r; parallel_branch_count++; }
            dry_l = parallel_in_l; dry_r = parallel_in_r; // Parallel, starting a fresh branch
            branch_open = true;
        }

        // Sidechain key lookup (see this function's own sc_snapshot_l/r comment above and
        // FXSlotParams.sidechain_source's own comment): "Same as Source" (the default, and any
        // out-of-range value) self-keys off this slot's own dry input, exactly reproducing
        // ordinary non-external-sidechain compression.
        float sc_l, sc_r;
        int sc_src = (int)fx_params[s].sidechain_source;
        if (sc_src < 0 || sc_src >= SOURCE_COUNT) {
            sc_l = dry_l; sc_r = dry_r;
        } else {
            sc_l = sc_snapshot_l[sc_src]; sc_r = sc_snapshot_r[sc_src];
        }

        float out_l, out_r;
        gr_db_out[s] = 0.0f; // default: bypassed/off/non-GR-capable -- overwritten below for the 4 GR-capable types
        if (fx_params[s].bypass != 0.0f || type == FX_TYPE_OFF) {
            out_l = dry_l; out_r = dry_r; // pass-through, dry
        } else {
            float wet_l = dry_l, wet_r = dry_r;
            switch (type) {
                case FX_TYPE_ENSEMBLE: run_fx_ensemble(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_CREAMCOAT: run_fx_creamcoat(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_DENSITY3: run_fx_density3(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_STEREOCHORUS: run_fx_stereochorus(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_STEREODOUBLER: run_fx_stereodoubler(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_TAPEDELAY2: run_fx_tapedelay2(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_POINTYGUITAR: run_fx_pointyguitar(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_GUITARCONDITIONER: run_fx_guitarconditioner(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_CABS2: run_fx_cabs2(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
#if HDS_NAM_ENABLED
                case FX_TYPE_NAM: run_fx_nam(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
#endif
                case FX_TYPE_GATE:
                    run_fx_gate(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = ((FxGateState *)fx_engine[s].state)->last_gr_db;
                    break;
                case FX_TYPE_EQ3: run_fx_eq3(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_LIMITER:
                    run_fx_limiter(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = ((FxLimiterState *)fx_engine[s].state)->last_gr_db;
                    break;
                case FX_TYPE_FETCOMP:
                    run_fx_fetcomp(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = ((FxFetCompState *)fx_engine[s].state)->last_gr_db;
                    break;
                case FX_TYPE_FAIRKID67:
                    run_fx_fairkid67(&fx_engine[s], &fx_params[s], &wet_l, &wet_r, sc_l, sc_r);
                    if (fx_engine[s].state) gr_db_out[s] = ((FxFairKid67State *)fx_engine[s].state)->last_gr_db;
                    break;
                case FX_TYPE_PEAKMAX:
                    run_fx_peakmax(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = ((FxPeakMaxState *)fx_engine[s].state)->last_gr_db;
                    break;
                case FX_TYPE_MULTIFILTER: run_fx_multifilter(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_OPTOCOMP:
                    run_fx_optocomp(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = ((FxOptoCompState *)fx_engine[s].state)->last_gr_db;
                    break;
                case FX_TYPE_DEESSER:
                    run_fx_deesser(&fx_engine[s], &fx_params[s], &wet_l, &wet_r);
                    if (fx_engine[s].state) gr_db_out[s] = ((FxDeEsserState *)fx_engine[s].state)->last_gr_db;
                    break;
                case FX_TYPE_HARDTUNE: run_fx_hardtune(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_FDNVERB: run_fx_fdnverb(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_SSL4K: run_fx_ssl4k(&fx_engine[s], &fx_params[s], &wet_l, &wet_r); break;
                case FX_TYPE_OFF:
                default:
                    break; // pass-through
            }
            float mix = fmaxf(0.0f, fminf(100.0f, fx_params[s].mix)) / 100.0f;
            out_l = dry_l * (1.0f - mix) + wet_l * mix;
            out_r = dry_r * (1.0f - mix) + wet_r * mix;
        }

        slot_out_l[s] = out_l; slot_out_r[s] = out_r; // see this function's own slot_out_l/r comment -- published unconditionally, regardless of routing_mode

        if (routing_mode == 0) {
            *l = out_l;
            *r = out_r;
        } else {
            branch_l = out_l; branch_r = out_r;
            continue_branch = (fx_params[s].chain_to_next != 0.0f);
        }
    }

    if (routing_mode == 1) {
        if (branch_open) { parallel_sum_l += branch_l; parallel_sum_r += branch_r; parallel_branch_count++; } // close the final still-open branch
        if (parallel_branch_count > 0) {
            *l = parallel_sum_l / (float)parallel_branch_count;
            *r = parallel_sum_r / (float)parallel_branch_count;
        }
        // parallel_branch_count == 0: *l/*r already untouched (still the original dry input),
        // which is the correct "this source owns no slots" passthrough.
    }
}
