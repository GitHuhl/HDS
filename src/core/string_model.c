// string_model.c -- the Plucked String oscillator (WAVE_STRING).
//
// A digital waveguide string: the string's travelling waves live in one delay loop per
// polarization (one round trip nut -> bridge -> nut, ages 0..ni), with everything the bridge does
// lumped at the loop's end: a fractional-delay allpass that tunes the loop exactly, a cascade of
// allpasses for stiffness (dispersion: upper partials travel faster, so they come out sharp),
// and a one-pole loss filter whose DC gain sets the decay time and whose pole sets how much
// faster the highs die. The delays of the loss and stiffness filters at the fundamental are
// measured and taken out of the loop length, so the note stays in tune at any setting.
//
// A point x along the string (0 = nut, 1 = bridge) appears twice in the loop: on the left-going
// wave at age half*(1-x) (stored negated: the bridge's inversion is folded into the storage
// convention) and on the right-going wave at age half*(1+x). The string's displacement there is
// their difference, which is how the model reads a pickup, feels a hammer or bow, touches a
// harmonic node, or lets the string hit a fret -- all at real positions along the string.
//
// Two transverse polarizations with slightly different tuning and damping, coupled at the bridge
// by a small rotation, give the slow beating and two-stage decay of real strings. A three-mode
// resonant body is mixed into the output.
//
// Exciters: Pluck and Pick write a triangular displacement (rounded by brightness/hardness/
// velocity) straight into the loop at note-on -- the ideal plucked-string initial state -- with a
// short noise scrape for the pick. Hammer is a nonlinear felt mass-spring that pushes against the
// string's own displacement until it bounces off. Noise injects a filtered burst. Bow runs a
// stick-slip friction curve against the string's velocity while the key is held. Any oscillator
// the FM algorithm routes into this one is injected as extra excitation (Ext In).
//
// Everything runs on the audio thread with no allocation: lines come from the engine's
// HdsStringPool. Coefficients are recomputed every STRING_CTL_INTERVAL samples (pitch bend,
// vibrato, glide, release and the tension-modulation pitch shift all go through that).
#include "core/string_model.h"

#include <math.h>
#include <string.h>

#define STRING_CTL_INTERVAL 16
#define STRING_OUT_GAIN 5.0f      // calibrated so a default pluck sits near a plain Saw's level
#define STRING_BODY_GAIN 2.5f
#define STRING_POL2_SHARE 0.6f     // second polarization's share of the excitation (pluck angle)

static const float BODY_FREQ[STRING_BODY_MODES] = {104.0f, 203.0f, 395.0f};
static const float BODY_BW[STRING_BODY_MODES]   = {14.0f, 22.0f, 45.0f};
static const float BODY_GAIN[STRING_BODY_MODES] = {1.0f, 0.7f, 0.5f};

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float clamp01(float v) { return clampf(v, 0.0f, 1.0f); }

static inline float rng_bipolar(unsigned int *s) {
    *s = *s * 1664525u + 1013904223u;
    return (float)(*s >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

// Phase delay (samples) at radian frequency w of the first-order allpass (c + z^-1)/(1 + c z^-1).
static float allpass_phase_delay(float c, float w) {
    float sw = sinf(w), cw = cosf(w);
    float phase = -atan2f(sw, c + cw) + atan2f(c * sw, 1.0f + c * cw);
    return -phase / w;
}

// --- Positions along the string ---
static inline float pol_read(const HdsStringPol *P, int w, float age) {
    if (age < 0.0f) age = 0.0f;
    if (age > (float)(P->ni - 2)) age = (float)(P->ni - 2);
    int a0 = (int)age; float fr = age - (float)a0;
    float v0 = P->line[(w - 1 - a0) & STRING_LINE_MASK];
    float v1 = P->line[(w - 2 - a0) & STRING_LINE_MASK];
    return v0 + fr * (v1 - v0);
}
static inline void pol_add(HdsStringPol *P, int w, float age, float v) {
    if (age < 0.0f) age = 0.0f;
    if (age > (float)(P->ni - 2)) age = (float)(P->ni - 2);
    int a0 = (int)age; float fr = age - (float)a0;
    P->line[(w - 1 - a0) & STRING_LINE_MASK] += v * (1.0f - fr);
    P->line[(w - 2 - a0) & STRING_LINE_MASK] += v * fr;
}
// Displacement at position x (0 nut .. 1 bridge).
static inline float pol_disp(const HdsStringPol *P, int w, float half, float x) {
    return pol_read(P, w, half * (1.0f + x)) - pol_read(P, w, half * (1.0f - x));
}
// Adds displacement d at position x (half to each travelling wave).
static inline void pol_push(HdsStringPol *P, int w, float half, float x, float d) {
    pol_add(P, w, half * (1.0f + x), 0.5f * d);
    pol_add(P, w, half * (1.0f - x), -0.5f * d);
}

// --- Coefficients ---
static void update_coeffs(HdsStringVoice *s, const StringModelParams *sp, float hz) {
    float f = clampf(hz, 16.0f, 7000.0f);
    // Tension modulation: a string vibrating hard is stretched, so it plays sharp, and settles
    // back to pitch as it decays.
    // At 100 a hard pluck starts ~25 cents sharp; the default (15) a few cents.
    f *= 1.0f + fminf(0.025f, (sp->nonlinear / 100.0f) * 0.4f * s->energy);
    // Redesign only when something audible moved: pitch by more than ~0.3 cent, the release
    // crossfade, or any control.
    if (s->cache_valid && fabsf(f / s->cache_f - 1.0f) < 0.0002f && fabsf(s->rel_mix - s->cache_rel) < 0.002f
        && memcmp(&s->cache_sp, sp, sizeof(*sp)) == 0) return;
    s->cache_valid = true; s->cache_f = f; s->cache_rel = s->rel_mix; s->cache_sp = *sp;

    float t60 = clampf(sp->decay, 0.05f, 30.0f) * powf(261.63f / f, clamp01(sp->decay_keytrack / 100.0f));
    float tr = clampf(sp->release, 0.01f, 10.0f);
    if (tr > t60) tr = t60; // releasing never makes the string ring longer
    float T = expf(logf(t60) + (logf(tr) - logf(t60)) * s->rel_mix);

    // Damping: how much shorter the decay is 8 partials up than at the fundamental (0 = same,
    // 100 = 1/30th). The damper (release) also mutes highs first.
    float d = clamp01(sp->damping / 100.0f);
    d += (1.0f - d) * 0.5f * s->rel_mix;
    float hi_ratio = 1.0f - 0.967f * powf(d, 0.8f);
    // Stiffness -> inharmonicity B (partial n sounds at n*f0*sqrt(1 + B n^2); a piano's bass
    // strings are around 1e-4, a stiff treble wire 1e-2), realized by designing the dispersion
    // allpasses per note so that partial n_ref lands where B puts it.
    float stiff = clamp01(sp->stiffness / 100.0f);
    float c = 0.0f;
    if (stiff > 0.005f) {
        float B = 0.01f * powf(stiff, 2.0f);
        float w0 = 2.0f * (float)M_PI * f / (float)SAMPLE_RATE;
        int n_ref = 8;
        while (n_ref > 2 && (float)n_ref * w0 * sqrtf(1.0f + B * (float)(n_ref * n_ref)) > 0.85f * (float)M_PI) n_ref--;
        float stretch = sqrtf(1.0f + B * (float)(n_ref * n_ref));
        float wn = (float)n_ref * w0 * stretch;
        float need = ((float)SAMPLE_RATE / f) * (1.0f - 1.0f / stretch) / (float)STRING_DISP_STAGES; // per stage
        float lo = -0.9f, hi = 0.0f;
        for (int it = 0; it < 20; it++) {
            float mid = 0.5f * (lo + hi);
            float got = allpass_phase_delay(mid, w0) - allpass_phase_delay(mid, wn);
            if (got < need) hi = mid; else lo = mid;
        }
        c = 0.5f * (lo + hi);
    }

    bool pol2 = sp->pol_detune > 0.01f || sp->pol_coupling > 0.01f || sp->pol_width > 0.01f;
    if (pol2 && !s->pol2_on) memset(&s->pol[1], 0, sizeof(s->pol[1])); // starts silent, not with stale audio
    s->pol2_on = pol2;

    for (int p = 0; p < (pol2 ? 2 : 1); p++) {
        HdsStringPol *P = &s->pol[p];
        float fp = p ? f * exp2f(clampf(sp->pol_detune, 0.0f, 20.0f) / 1200.0f) : f;
        float Tp = p ? T * 0.85f : T;
        float w = 2.0f * (float)M_PI * fp / (float)SAMPLE_RATE;
        // The loss filter is applied once per period, so it is designed per note: its gain at the
        // fundamental gives exactly Tp, and its gain 8 partials up (or near Nyquist) gives
        // Tp * hi_ratio. Solved for the one-pole coefficient by bisection.
        float g0 = powf(10.0f, -3.0f / (Tp * fp));
        float rp = p ? hi_ratio * 0.8f : hi_ratio;
        float gh = powf(10.0f, -3.0f / (Tp * rp * fp));
        float wh = fminf(8.0f * w, 0.9f * (float)M_PI);
        float q = gh / g0; // wanted |H(wh)| / |H(w)|
        float lo = 0.0f, hi = 0.995f, ap = 0.0f;
        for (int it = 0; it < 18; it++) {
            ap = 0.5f * (lo + hi);
            float hw = 1.0f / sqrtf(1.0f - 2.0f * ap * cosf(w) + ap * ap);
            float hh = 1.0f / sqrtf(1.0f - 2.0f * ap * cosf(wh) + ap * ap);
            if (hh / hw > q) lo = ap; else hi = ap;
        }
        float h_w = (1.0f - ap) / sqrtf(1.0f - 2.0f * ap * cosf(w) + ap * ap);
        P->loss_a = ap;
        P->loss_g = fminf(0.99999f, g0 / h_w); // DC gain; with (1-a) below, gain at the fundamental is g0
        float d_loss = atan2f(ap * sinf(w), 1.0f - ap * cosf(w)) / w;
        P->disp_c = c;
        float d_disp = (c != 0.0f) ? (float)STRING_DISP_STAGES * allpass_phase_delay(c, w) : 0.0f;
        float rest = (float)SAMPLE_RATE / fp - d_loss - d_disp;
        int ni = (int)floorf(rest - 0.5f);
        if (ni < 4) ni = 4;
        if (ni > STRING_LINE_LEN - 8) ni = STRING_LINE_LEN - 8;
        float delta = clampf(rest - (float)ni, 0.1f, 1.9f);
        // Thiran's first-order allpass is exact only at DC; two corrections make its delay exact
        // at the fundamental, where tuning is heard.
        float target = delta, dd = delta;
        for (int it = 0; it < 2; it++) {
            float eta = clampf((1.0f - dd) / (1.0f + dd), -0.95f, 0.95f);
            dd += target - allpass_phase_delay(eta, w);
            dd = clampf(dd, 0.05f, 1.95f);
        }
        P->frac_eta = clampf((1.0f - dd) / (1.0f + dd), -0.95f, 0.95f);
        P->ni = ni;
    }
    s->half_len = 0.5f * (float)s->pol[0].ni;
    // Coupling is a rate, not a per-period amount: energy moves between the two polarizations at
    // up to 0.6 Hz (Coupling 100), the same at every pitch. Applied once per period, so the angle
    // per pass scales with the period. (Per-period rotation made the exchange 20x too fast at
    // default settings and pitch-dependent: an audible tremolo.)
    float th = 2.0f * (float)M_PI * 0.6f * clamp01(sp->pol_coupling / 100.0f) / f;
    s->rot_c = cosf(th); s->rot_s = sinf(th);
    s->f0 = f;

    float scale = exp2f((50.0f - clampf(sp->body_size, 0.0f, 100.0f)) / 50.0f);
    for (int m = 0; m < STRING_BODY_MODES; m++) {
        float r = expf(-(float)M_PI * BODY_BW[m] * scale / (float)SAMPLE_RATE);
        float fw = 2.0f * (float)M_PI * fminf(BODY_FREQ[m] * scale, 8000.0f) / (float)SAMPLE_RATE;
        s->body_a1[m] = -2.0f * r * cosf(fw);
        s->body_a2[m] = r * r;
        s->body_b0[m] = 0.5f * (1.0f - r * r);
    }
}

// --- Pool ---
static int claim_slot(HdsStringPool *pool, int *slot_ref) {
    int best = -1;
    for (int i = 0; i < STRING_POOL_SIZE; i++) {
        HdsStringVoice *s = &pool->v[i];
        if (!s->owner || *s->owner != i) { best = i; break; }
    }
    if (best < 0) { // every slot sounding: steal the oldest note's string
        unsigned int oldest_age = 0;
        for (int i = 0; i < STRING_POOL_SIZE; i++) {
            unsigned int age = pool->clock - pool->v[i].stamp;
            if (best < 0 || age > oldest_age) { best = i; oldest_age = age; }
        }
        *pool->v[best].owner = -1;
    }
    HdsStringVoice *s = &pool->v[best];
    memset(s, 0, sizeof(*s));
    s->owner = slot_ref;
    s->stamp = ++pool->clock;
    s->rng = 0x9e3779b9u ^ ((unsigned int)best * 2654435761u) ^ pool->clock;
    *slot_ref = best;
    return best;
}

void hds_string_release(HdsStringPool *pool, int *slot_ref) {
    int idx = *slot_ref;
    if (idx >= 0 && idx < STRING_POOL_SIZE && pool->v[idx].owner == slot_ref) pool->v[idx].owner = NULL;
    *slot_ref = -1;
}

// --- Note-on excitation ---
// Triangular displacement peaking at x0 (rounded by a forward-backward one-pole along the string,
// `width` samples wide), added into both travelling waves.
static void add_pluck_shape(HdsStringPool *pool, HdsStringVoice *s, int p, float x0, float amp, float width) {
    HdsStringPol *P = &s->pol[p];
    int n = P->ni;
    float half = 0.5f * (float)n;
    float *sh = pool->scratch;
    for (int a = 0; a < n; a++) {
        bool left = (float)a < half;
        float x = left ? 1.0f - (float)a / half : (float)a / half - 1.0f;
        x = clamp01(x);
        float y = (x < x0) ? x / x0 : (1.0f - x) / (1.0f - x0);
        sh[a] = (left ? -0.5f : 0.5f) * amp * y;
    }
    float k = expf(-1.0f / fmaxf(0.3f, width));
    float st = 0.0f;
    for (int a = 0; a < n; a++) { st += (1.0f - k) * (sh[a] - st); sh[a] = st; }
    st = 0.0f;
    for (int a = n - 1; a >= 0; a--) { st += (1.0f - k) * (sh[a] - st); sh[a] = st; }
    for (int a = 0; a < n; a++) P->line[(s->w - 1 - a) & STRING_LINE_MASK] += sh[a];
}

static void excite(HdsStringPool *pool, HdsStringVoice *s, const StringModelParams *sp, int velocity, bool fresh) {
    s->vel01 = clamp01((float)velocity / 127.0f);
    int type = (int)sp->exciter;
    if (type < 0 || type >= STRING_EXC_COUNT) type = STRING_EXC_PLUCK;
    s->exc_type = type;
    float bright = clamp01(sp->exc_bright / 100.0f + (sp->exc_vel_bright / 100.0f) * (s->vel01 - 0.65f) * 1.2f);
    float hard = clamp01(sp->exc_hardness / 100.0f);
    float noise = clamp01(sp->exc_noise / 100.0f);
    float fc = 150.0f * exp2f(bright * 7.0f);
    s->exc_lp_a = expf(-2.0f * (float)M_PI * fminf(fc, 20000.0f) / (float)SAMPLE_RATE);
    float x0 = clampf(sp->pluck_pos / 100.0f, 0.01f, 0.5f);

    // Re-striking a ringing string: a finger, pick or hammer stops most of what was there first.
    if (!fresh && type != STRING_EXC_BOW) {
        for (int p = 0; p < 2; p++) for (int i = 0; i < STRING_LINE_LEN; i++) s->pol[p].line[i] *= 0.35f;
    }
    s->exc_t = 0; s->exc_len = 0; s->exc_amp = 0.0f; s->hammer_on = false;

    s->exc_env = 1.0f;
    switch (type) {
        case STRING_EXC_PLUCK:
        case STRING_EXC_PICK: {
            float sharp = clamp01(0.55f * bright + 0.45f * hard + (type == STRING_EXC_PICK ? 0.35f : 0.0f));
            float width = 0.3f + (1.0f - sharp) * (1.0f - sharp) * 0.12f * (float)s->pol[0].ni; // relative to the string, so the tone holds across the keyboard
            add_pluck_shape(pool, s, 0, x0, 0.5f, width);
            if (s->pol2_on) add_pluck_shape(pool, s, 1, x0, 0.5f * STRING_POL2_SHARE, width);
            s->exc_len = (int)((float)SAMPLE_RATE * (0.0008f + (1.0f - hard) * 0.003f));
            s->exc_amp = noise * (type == STRING_EXC_PICK ? 0.25f : 0.08f);
            break;
        }
        case STRING_EXC_HAMMER: {
            float contact = 20.0f + powf(1.0f - hard, 1.5f) * 260.0f; // samples: ~0.4 ms hard .. ~6 ms soft
            // A hammer is never in contact for longer than a fraction of the string's period (on a
            // real instrument the treble hammers are smaller and harder for exactly this reason).
            contact = fminf(contact, fmaxf(4.0f, 0.4f * (float)s->pol[0].ni));
            s->hammer_m_inv = 1.0f / contact;
            s->hammer_k = 0.3f + hard * hard * 30.0f;
            s->hammer_vel = (0.15f + 0.85f * s->vel01) * 4.0f / contact;
            s->hammer_pos = pol_disp(&s->pol[0], s->w, s->half_len, x0);
            s->hammer_on = true;
            s->exc_len = (int)contact;
            s->exc_amp = noise * 0.03f;
            break;
        }
        case STRING_EXC_NOISE: {
            s->exc_len = (int)((float)SAMPLE_RATE * (0.002f + (1.0f - hard) * (1.0f - hard) * 0.08f));
            s->exc_amp = fminf(0.5f, (0.25f + 0.75f * s->vel01) * 14.0f / sqrtf((float)s->exc_len));
            break;
        }
        case STRING_EXC_BOW:
        default:
            s->exc_len = (int)((float)SAMPLE_RATE * 0.03f);
            s->exc_amp = noise * 0.01f;
            break;
    }
    s->exc_decay = s->exc_len > 0 ? expf(-5.0f / (float)s->exc_len) : 0.0f;
}

// --- Per sample ---
// Every recursive filter here would otherwise decay into denormal floats (orders of magnitude
// slower on x86) while a string rings out; the loop gets a tiny alternating offset (~-360 dBFS)
// that keeps everything downstream of it in normal range without ever being audible.
static inline float bridge(HdsStringPol *P, int w) {
    float x = P->line[(w - P->ni) & STRING_LINE_MASK];
    float y = P->frac_eta * x + P->frac_x1 - P->frac_eta * P->frac_y1;
    P->frac_x1 = x; P->frac_y1 = y;
    if (P->disp_c != 0.0f) {
        float c = P->disp_c;
        for (int st = 0; st < STRING_DISP_STAGES; st++) {
            float out = c * y + P->disp_x1[st] - c * P->disp_y1[st];
            P->disp_x1[st] = y; P->disp_y1[st] = out; y = out;
        }
    }
    y = P->loss_g * (1.0f - P->loss_a) * y + P->loss_a * P->loss_y1 + ((w & 1) ? 1e-18f : -1e-18f);
    P->loss_y1 = y;
    return y;
}

// Pulls the string toward zero displacement at x by `amt` of its current value (both waves).
static inline void pol_damp_at(HdsStringPol *P, int w, float half, float x, float amt) {
    float y = pol_disp(P, w, half, x);
    pol_push(P, w, half, x, -amt * y);
}

// The string hitting a fret near the nut: displacement beyond `th` is mostly pushed back.
static inline void pol_buzz(HdsStringPol *P, int w, float half, float th) {
    const float xb = 0.04f;
    float y = pol_disp(P, w, half, xb);
    if (y > th) pol_push(P, w, half, xb, -0.9f * (y - th));
    else if (y < -th) pol_push(P, w, half, xb, -0.9f * (y + th));
}

float hds_string_render(HdsStringPool *pool, int *slot_ref, bool *trigger, const StringModelParams *sp,
                        float hz, float ext_in, bool released, int velocity, float *out_l, float *out_r) {
    int idx = *slot_ref;
    if (idx >= 0 && (idx >= STRING_POOL_SIZE || pool->v[idx].owner != slot_ref)) { idx = -1; *slot_ref = -1; }
    if (*trigger) {
        *trigger = false;
        bool fresh = idx < 0;
        if (fresh) idx = claim_slot(pool, slot_ref);
        HdsStringVoice *s = &pool->v[idx];
        s->rel_mix = 0.0f;
        s->cache_valid = false;
        update_coeffs(s, sp, hz);
        s->ctl_count = STRING_CTL_INTERVAL;
        excite(pool, s, sp, velocity, fresh);
    }
    if (idx < 0) { if (out_l) *out_l = 0.0f; if (out_r) *out_r = 0.0f; return 0.0f; }
    HdsStringVoice *s = &pool->v[idx];

    s->rel_mix += ((released ? 1.0f : 0.0f) - s->rel_mix) * 0.002f;
    if (--s->ctl_count <= 0) { update_coeffs(s, sp, hz); s->ctl_count = STRING_CTL_INTERVAL; }

    // 1. Bridge: both loops complete one more sample, then the polarizations exchange energy.
    float y0 = bridge(&s->pol[0], s->w);
    float y1 = s->pol2_on ? bridge(&s->pol[1], s->w) : 0.0f;
    if (s->pol2_on) {
        if (s->rot_s != 0.0f) {
            float a = s->rot_c * y0 + s->rot_s * y1, b = -s->rot_s * y0 + s->rot_c * y1;
            y0 = a; y1 = b;
        }
        s->pol[1].line[s->w & STRING_LINE_MASK] = y1;
    }
    s->pol[0].line[s->w & STRING_LINE_MASK] = y0;
    s->w = (s->w + 1) & STRING_LINE_MASK;
    int w = s->w;
    float half = s->half_len;
    int npol = s->pol2_on ? 2 : 1;

    // 2. Touch points along the string: a harmonic node, a fret.
    int node = (int)(sp->harmonic_node + 0.5f);
    float hamt = clamp01(sp->harmonic_amt / 100.0f);
    if (node >= 2 && hamt > 0.0f) {
        float xh = 1.0f / (float)node;
        for (int p = 0; p < npol; p++) pol_damp_at(&s->pol[p], w, half, xh, 0.25f * hamt);
    }
    float buzz = clamp01(sp->buzz / 100.0f);
    if (buzz > 0.0f) {
        float th = 0.02f + (1.0f - buzz) * 0.3f;
        for (int p = 0; p < npol; p++) pol_buzz(&s->pol[p], w, half, th);
    }

    // 3. Excitation at the pluck/strike/bow point.
    float x0 = clampf(sp->pluck_pos / 100.0f, 0.01f, 0.5f);
    float e = 0.0f;
    if (s->exc_t < s->exc_len) {
        if (s->exc_t == 0) s->exc_env = 1.0f;
        e += s->exc_amp * s->exc_env * rng_bipolar(&s->rng);
        s->exc_env *= s->exc_decay;
        s->exc_t++;
    }
    if (s->hammer_on) {
        float ys = pol_disp(&s->pol[0], w, half, x0);
        float comp = s->hammer_pos - ys;
        float force = comp > 0.0f ? fminf(0.25f, s->hammer_k * comp * sqrtf(comp)) : 0.0f; // felt: stiffens as it compresses
        s->hammer_vel -= force * s->hammer_m_inv;
        s->hammer_pos += s->hammer_vel;
        if ((comp <= 0.0f && s->hammer_vel < 0.0f) || s->exc_t > 20000) s->hammer_on = false;
        e += force;
    }
    e += ext_in * clamp01(sp->exc_input / 100.0f) * 0.05f;
    s->exc_lp += (1.0f - s->exc_lp_a) * (e - s->exc_lp);
    if (fabsf(s->exc_lp) < 1e-12f) s->exc_lp = 0.0f; // settles to exactly 0 (no denormals, and step 3 is skipped)
    e = s->exc_lp;
    if (s->exc_type == STRING_EXC_BOW) {
        float target = released ? 0.0f : 1.0f;
        s->bow_env += (target - s->bow_env) * (target > s->bow_env ? 0.0008f : 0.0015f);
        // Bowed-waveguide friction: the incoming waves at the bow point are the string's velocity
        // there; the bow sticks (drags the string along) while the difference is small and slips
        // past it once it grows -- the friction curve below (steeper with less pressure).
        float vs = pol_disp(&s->pol[0], w, half, x0);
        float vb = s->bow_env * (0.03f + 0.14f * clamp01(sp->bow_speed / 100.0f)) * (0.4f + 0.6f * s->vel01);
        float dv = vb - vs;
        float slope = 5.0f - 4.0f * clamp01(sp->bow_pressure / 100.0f);
        float u = fabsf(dv * slope) + 0.75f, u2 = u * u;
        float stick = 1.0f / (u2 * u2);
        e += dv * fminf(1.0f, stick);
    }
    if (fabsf(e) > 1e-12f) {
        pol_push(&s->pol[0], w, half, x0, e);
        if (s->pol2_on) pol_push(&s->pol[1], w, half, x0, e * STRING_POL2_SHARE);
    }

    // 4. Pickup, body, output.
    float xp = clampf(sp->pickup_pos / 100.0f, 0.01f, 0.5f);
    float p0 = pol_disp(&s->pol[0], w, half, xp);
    float p1 = s->pol2_on ? pol_disp(&s->pol[1], w, half, xp) : 0.0f;
    s->energy += 0.0005f * (4.0f * p0 * p0 - s->energy);

    float mono = s->pol2_on ? (p0 + STRING_POL2_SHARE * p1) / (1.0f + STRING_POL2_SHARE) : p0;
    float body = clamp01(sp->body / 100.0f);
    if (body > 0.0f) {
        float bsum = 0.0f;
        for (int m = 0; m < STRING_BODY_MODES; m++) {
            float y = s->body_b0[m] * (mono - s->body_x2[m]) - s->body_a1[m] * s->body_y1[m] - s->body_a2[m] * s->body_y2[m];
            s->body_x2[m] = s->body_x1[m]; s->body_x1[m] = mono;
            s->body_y2[m] = s->body_y1[m]; s->body_y1[m] = y;
            bsum += BODY_GAIN[m] * y;
        }
        mono += body * STRING_BODY_GAIN * bsum;
    }
    float side = s->pol2_on ? clamp01(sp->pol_width / 100.0f) * 0.5f * (p0 - p1) : 0.0f;

    // DC blockers (an off-center pluck or a bow leaves a slowly decaying offset).
    float m_out = mono - s->dc_x1 + 0.995f * s->dc_y1; s->dc_x1 = mono; s->dc_y1 = m_out;
    float s_out = side - s->dc_x1b + 0.995f * s->dc_y1b; s->dc_x1b = side; s->dc_y1b = s_out;
    float gain = STRING_OUT_GAIN;
    // A bowed string settles at an amplitude that grows with pitch; level it across the keyboard.
    if (s->exc_type == STRING_EXC_BOW) gain *= 0.3f * sqrtf(261.63f / fmaxf(s->f0, 20.0f));
    m_out *= gain; s_out *= gain;
    // Safety: extreme settings (everything at maximum) can stack up; above 1.5 the output bends
    // smoothly toward 2.5 instead of growing without bound. A normal pluck peaks well below 1.5.
    if (fabsf(m_out) > 1.5f) { float ex = fabsf(m_out) - 1.5f; m_out = copysignf(1.5f + ex / (1.0f + ex), m_out); }
    if (out_l) *out_l = 0.70710678f * (m_out + s_out);
    if (out_r) *out_r = 0.70710678f * (m_out - s_out);
    return m_out;
}

void hds_string_default_params(StringModelParams *sp) {
    memset(sp, 0, sizeof(*sp));
    sp->exciter = (float)STRING_EXC_PLUCK;
    sp->exc_hardness = 50.0f; sp->exc_bright = 65.0f; sp->exc_vel_bright = 50.0f; sp->exc_noise = 10.0f;
    sp->exc_input = 0.0f; sp->bow_pressure = 50.0f; sp->bow_speed = 50.0f;
    sp->pluck_pos = 18.0f; sp->pickup_pos = 12.0f;
    sp->decay = 4.0f; sp->damping = 40.0f; sp->decay_keytrack = 50.0f; sp->release = 0.25f;
    sp->stiffness = 10.0f; sp->nonlinear = 15.0f;
    sp->harmonic_node = 0.0f; sp->harmonic_amt = 50.0f; sp->buzz = 0.0f;
    sp->pol_detune = 1.5f; sp->pol_coupling = 5.0f; sp->pol_width = 40.0f;
    sp->body = 35.0f; sp->body_size = 50.0f;
}
