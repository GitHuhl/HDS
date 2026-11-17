// fx_compressors.c -- Compressors: FET, dbx 160, Fairchild 670 (FairKid67), opto.
// Moved verbatim out of fx_chain.c; each effect's run_fx_*() is called by process_fx_chain() via
// fx_effects.h, and its state struct stays private to this file.
#include "fx_effects.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

void run_fx_fetcomp(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

// Meter readback for process_fx_chain() (keeps FxFetCompState private to this file).
float fx_fetcomp_gr_db(const void *state) { return ((const FxFetCompState *)state)->last_gr_db; }

// --- FX_TYPE_DBX160: see FX_TYPE_DBX160's own comment above (fx_types.h) for the param map, the
// general-knowledge basis for its design, and the two real bugs fixed from its first draft. ---
typedef struct {
    float rms_state;       // one-pole smoothed max(L,R)^2 -- see this effect's own top comment for
                             // why its ~30ms smoothing time IS this effect's whole attack behavior
    float current_gain_db; // 0 (no reduction) down to very negative
    float hpf_lp[2];        // sidechain HPF's own one-pole lowpass state, per channel
    // Real 4x oversampling for the saturation stage: two cascaded 2x zero-stuff+biquad stages (see
    // FX_TYPE_DBX160's own comment in fx_types.h for why two cascaded 2x stages beat one filter run
    // at a straight 4x rate) -- coefficients computed once at init (SAMPLE_RATE is fixed), one z1/z2
    // pair per channel per stage, both on the way up and on the way back down.
    EqBiquadCoeffs os_coeffs_2x, os_coeffs_4x;
    float up1_z1[2], up1_z2[2];     // 1x -> 2x upsample filter state
    float up2_z1[2], up2_z2[2];     // 2x -> 4x upsample filter state
    float down2_z1[2], down2_z2[2]; // 4x -> 2x downsample filter state
    float down1_z1[2], down1_z2[2]; // 2x -> 1x downsample filter state
    bool initialized;
    float last_gr_db; // read back by process_fx_chain() for the GR meter
} FxDbx160State;

static void dbx160_init(FxDbx160State *st) {
    compute_fetcomp_os_lowpass_coeffs(&st->os_coeffs_2x, (float)SAMPLE_RATE * 2.0f);
    compute_fetcomp_os_lowpass_coeffs(&st->os_coeffs_4x, (float)SAMPLE_RATE * 4.0f);
}

// VCA-style cubic waveshaper -- drive scales up with how much gain reduction is currently being
// applied (gain_reduction_db is this sample's own |current_gain_db|, a positive magnitude), the
// general "harder VCA compression grinds more" behavior common to VCA designs (see this effect's
// own top comment: not a curve measured from real 160-family hardware). Memoryless by design, which
// is exactly why it's the one part of this effect that needs oversampling -- everything else here
// only ever changes at the ~30ms RMS smoothing rate, far below anything aliasing could touch.
static inline float dbx160_saturate(float x, float drive, float gain_reduction_db) {
    if (drive <= 0.0f) return x;
    const float dynamic_drive = drive * (1.0f + gain_reduction_db * 0.15f);
    float scaled = x * (1.0f + dynamic_drive * 0.4f);
    scaled = fmaxf(-1.2f, fminf(1.2f, scaled));
    const float distortion = (scaled * scaled * scaled) * (dynamic_drive * 0.12f);
    return scaled - distortion;
}


void run_fx_dbx160(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
    FxDbx160State *st = (FxDbx160State *)engine->state;
    if (!st) {
        st = calloc(1, sizeof(FxDbx160State));
        engine->state = st;
        if (!st) return; // allocation failure -- leave this sample untouched this call
    }
    if (!st->initialized) { dbx160_init(st); st->initialized = true; }

    const float p0 = fmaxf(0.0f, fminf(1.0f, params->param[0]));
    const float p1 = fmaxf(0.0f, fminf(1.0f, params->param[1]));
    const float p2 = fmaxf(0.0f, fminf(1.0f, params->param[2]));
    const float p3 = fmaxf(0.0f, fminf(1.0f, params->param[3]));
    const float p4 = fmaxf(0.0f, fminf(1.0f, params->param[4]));

    const float threshold_db = -60.0f + p0 * 60.0f; // -60..0dB
    const float ratio = 1.0f + p1 * 19.0f;          // 1:1..20:1
    const float makeup_db = -12.0f + p2 * 24.0f;    // +-12dB
    const float saturation = p3;                    // 0..1 drive
    const float hpf_hz = 20.0f + p4 * 280.0f;       // 20..300Hz -- 20Hz is this knob's own honest
                                                     // "off" (see fx_types.h's own comment)
    const float makeup_gain = powf(10.0f, makeup_db / 20.0f);

    // --- Sidechain: per-channel HPF into a shared, stereo-linked detector (max of both channels'
    // instantaneous power, one-pole smoothed) -- same stereo-link convention as FX_TYPE_GATE and
    // FX_TYPE_OPTOCOMP above: one detector value drives both channels' gain equally so the stereo
    // image never skews toward whichever channel happens to be louder at a given instant. ---
    const float hpf_alpha = 1.0f - expf(-2.0f * (float)M_PI * hpf_hz / (float)SAMPLE_RATE);
    const float sc_l = dbx160_hpf(*l, &st->hpf_lp[0], hpf_alpha);
    const float sc_r = dbx160_hpf(*r, &st->hpf_lp[1], hpf_alpha);

    const float rms_alpha = 1.0f - expf(-1.0f / ((float)SAMPLE_RATE * 0.030f)); // ~30ms window --
                             // this smoothing time IS this effect's own effective attack, see this
                             // effect's own top comment for why there's no separate Attack knob
    const float power = fmaxf(sc_l * sc_l, sc_r * sc_r);
    st->rms_state = rms_alpha * power + (1.0f - rms_alpha) * st->rms_state;
    const float rms_db = 10.0f * log10f(fmaxf(1e-12f, st->rms_state)); // == 20*log10(sqrt(power))

    const float overshoot_db = rms_db - threshold_db;
    float target_gain_db = 0.0f;
    if (overshoot_db > 0.0f) target_gain_db = -overshoot_db * (1.0f - 1.0f / ratio);

    const float release_db_per_sample = 120.0f / (float)SAMPLE_RATE; // fixed -- no Release knob,
                             // see this effect's own top comment
    if (target_gain_db < st->current_gain_db) {
        st->current_gain_db = target_gain_db; // attack: instant -- the smoothed detector above is
                                                // the only lag this effect has, by design
    } else {
        st->current_gain_db = fminf(0.0f, st->current_gain_db + release_db_per_sample);
    }
    const float gain_linear = powf(10.0f, st->current_gain_db / 20.0f);
    const float gr_mag_db = fabsf(st->current_gain_db);

    // --- Gain cell + saturation, real 4x oversampling (see FX_TYPE_DBX160's own comment in
    // fx_types.h for why this is two cascaded 2x stages rather than one filter run at a straight 4x
    // rate). Outer loop: 1x<->2x. Inner loop: 2x<->4x, where the saturator itself actually runs. ---
    const float dry_l = *l * gain_linear;
    const float dry_r = *r * gain_linear;
    float out_l = dry_l, out_r = dry_r;
    for (int stage1 = 0; stage1 < 2; stage1++) {
        const float in1_l = (stage1 == 0) ? (dry_l * 2.0f) : 0.0f;
        const float in1_r = (stage1 == 0) ? (dry_r * 2.0f) : 0.0f;
        const float up1_l = run_eq_biquad(in1_l, &st->os_coeffs_2x, &st->up1_z1[0], &st->up1_z2[0]);
        const float up1_r = run_eq_biquad(in1_r, &st->os_coeffs_2x, &st->up1_z1[1], &st->up1_z2[1]);

        float kept2x_l = 0.0f, kept2x_r = 0.0f; // this stage1 iteration's 4x->2x decimated sample
        for (int stage2 = 0; stage2 < 2; stage2++) {
            const float in2_l = (stage2 == 0) ? (up1_l * 2.0f) : 0.0f;
            const float in2_r = (stage2 == 0) ? (up1_r * 2.0f) : 0.0f;
            const float up2_l = run_eq_biquad(in2_l, &st->os_coeffs_4x, &st->up2_z1[0], &st->up2_z2[0]);
            const float up2_r = run_eq_biquad(in2_r, &st->os_coeffs_4x, &st->up2_z1[1], &st->up2_z2[1]);

            const float sat_l = dbx160_saturate(up2_l, saturation, gr_mag_db);
            const float sat_r = dbx160_saturate(up2_r, saturation, gr_mag_db);

            const float down2_l = run_eq_biquad(sat_l, &st->os_coeffs_4x, &st->down2_z1[0], &st->down2_z2[0]);
            const float down2_r = run_eq_biquad(sat_r, &st->os_coeffs_4x, &st->down2_z1[1], &st->down2_z2[1]);
            if (stage2 == 1) { kept2x_l = down2_l; kept2x_r = down2_r; } // decimate 4x -> 2x
        }

        const float down1_l = run_eq_biquad(kept2x_l, &st->os_coeffs_2x, &st->down1_z1[0], &st->down1_z2[0]);
        const float down1_r = run_eq_biquad(kept2x_r, &st->os_coeffs_2x, &st->down1_z1[1], &st->down1_z2[1]);
        if (stage1 == 1) { out_l = down1_l; out_r = down1_r; } // decimate 2x -> 1x
    }

    *l = out_l * makeup_gain;
    *r = out_r * makeup_gain;
    st->last_gr_db = fmaxf(0.0f, -st->current_gain_db);
}

// Meter readback for process_fx_chain() (keeps FxDbx160State private to this file).
float fx_dbx160_gr_db(const void *state) { return ((const FxDbx160State *)state)->last_gr_db; }

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

void run_fx_fairkid67(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r, float sc_l, float sc_r) {
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

// Meter readback for process_fx_chain() (keeps FxFairKid67State private to this file).
float fx_fairkid67_gr_db(const void *state) { return ((const FxFairKid67State *)state)->last_gr_db; }

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

void run_fx_optocomp(FXSlotEngine *engine, const FXSlotParams *params, float *l, float *r) {
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

// Meter readback for process_fx_chain() (keeps FxOptoCompState private to this file).
float fx_optocomp_gr_db(const void *state) { return ((const FxOptoCompState *)state)->last_gr_db; }
