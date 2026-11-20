// test_string_model.c -- the Plucked String oscillator's model on its own (string_model.c only):
// tuning across the keyboard, decay and release times, the harmonic-node touch, and stability of
// every exciter at extreme settings. Pass/fail checks only (no golden hashes): the full-engine
// renders of the same oscillator are fingerprinted by test_clap_patches.
#include "core/string_model.h"
#include "test_common.h"

static HdsStringPool g_pool;
#define N_MAX (48000 * 3)
static float g_buf[N_MAX];

static void render(const StringModelParams *sp, double hz, int n, int release_at, int velocity) {
    int slot = -1; bool trig = true;
    for (int i = 0; i < n; i++) {
        float l, r;
        g_buf[i] = hds_string_render(&g_pool, &slot, &trig, sp, (float)hz, 0.0f, i >= release_at, velocity, &l, &r);
    }
    hds_string_release(&g_pool, &slot);
}

// Magnitude of frequency f over g_buf[from, from+n) (Hann window).
static double mag_at(int from, int n, double f) {
    double re = 0, im = 0;
    for (int i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / (n - 1));
        re += w * g_buf[from + i] * cos(2 * M_PI * f * i / 48000.0);
        im += w * g_buf[from + i] * sin(2 * M_PI * f * i / 48000.0);
    }
    return sqrt(re * re + im * im);
}
// Strongest frequency within +-3% of `near` (0.05% steps, then a finer pass).
static double peak_freq(int from, int n, double near) {
    double best = 0, bf = near;
    for (double f = near * 0.97; f <= near * 1.03; f += near * 0.0005) { double m = mag_at(from, n, f); if (m > best) { best = m; bf = f; } }
    double c = bf;
    for (double f = c - near * 0.0005; f <= c + near * 0.0005; f += near * 0.00002) { double m = mag_at(from, n, f); if (m > best) { best = m; bf = f; } }
    return bf;
}
static double rms(int from, int n) { double s = 0; for (int i = from; i < from + n; i++) s += (double)g_buf[i] * g_buf[i]; return sqrt(s / n); }

static StringModelParams plain(void) {
    StringModelParams sp; hds_string_default_params(&sp);
    sp.pol_detune = sp.pol_coupling = sp.pol_width = 0; sp.nonlinear = 0; sp.body = 0; sp.stiffness = 0;
    return sp;
}

int main(void) {
    // 1. Tuning: the fundamental within 1 cent from E1 to E7, at default damping and at high damping.
    float dampings[2] = { 40.0f, 90.0f };
    for (int d = 0; d < 2; d++) {
        StringModelParams sp = plain(); sp.damping = dampings[d];
        double worst = 0; int worst_key = 0;
        for (int key = 28; key <= 100; key += 6) {
            double hz = 440.0 * pow(2.0, (key - 69) / 12.0);
            render(&sp, hz, 48000, N_MAX, 100);
            double c = 1200.0 * log2(peak_freq(4800, 24000, hz) / hz);
            if (fabs(c) > fabs(worst)) { worst = c; worst_key = key; }
        }
        char name[64]; snprintf(name, sizeof name, "string_model/tuning/damping_%d", (int)dampings[d]);
        test_check(fabs(worst) < 1.0, name, "worst fundamental error %+.2f cents (MIDI %d)", worst, worst_key);
    }

    // 2. Decay: with key tracking off, the level falls 60 dB in about Decay seconds (within 25%).
    {
        StringModelParams sp = plain(); sp.decay = 1.5f; sp.decay_keytrack = 0;
        render(&sp, 261.63, N_MAX, N_MAX, 100);
        double a = rms(4800, 4800), b = rms(4800 + 48000, 4800);
        double t60 = 60.0 / (20.0 * log10(a / b));
        test_check(t60 > 1.5 * 0.75 && t60 < 1.5 * 1.25, "string_model/decay_time", "measured T60 %.2f s for Decay 1.5 s", t60);
    }
    // 3. Release: once released, it dies far faster than while held.
    {
        StringModelParams sp = plain(); sp.decay = 8.0f; sp.release = 0.2f;
        render(&sp, 196.0, 48000, 9600, 100);
        double held = rms(4800, 2400), later = rms(9600 + 19200, 2400); // 0.4 s after release
        test_check(later < held * 0.02, "string_model/release_damps", "0.4 s after release the level is %.1f%% of the held level (Release 0.2 s)", 100.0 * later / held);
    }
    // 4. Harmonic node at 1/2: the fundamental (and odd partials) are damped, the 2nd partial rings.
    {
        StringModelParams sp = plain(); sp.decay = 6.0f;
        render(&sp, 146.83, 48000, N_MAX, 100);
        double free_ratio = mag_at(9600, 24000, 146.83) / mag_at(9600, 24000, 293.66);
        sp.harmonic_node = 2; sp.harmonic_amt = 80;
        render(&sp, 146.83, 48000, N_MAX, 100);
        double touched_ratio = mag_at(9600, 24000, 146.83) / mag_at(9600, 24000, 293.66);
        test_check(touched_ratio < free_ratio * 0.1, "string_model/harmonic_node", "fundamental/2nd ratio %.3f touched vs %.3f free", touched_ratio, free_ratio);
    }
    // 5. Every exciter, every control at its minimum and its maximum, lowest/middle/highest
    //    notes: always finite and bounded.
    {
        const size_t offs[] = { offsetof(StringModelParams, exc_hardness), offsetof(StringModelParams, exc_bright), offsetof(StringModelParams, exc_vel_bright),
            offsetof(StringModelParams, exc_noise), offsetof(StringModelParams, exc_input), offsetof(StringModelParams, bow_pressure), offsetof(StringModelParams, bow_speed),
            offsetof(StringModelParams, pluck_pos), offsetof(StringModelParams, pickup_pos), offsetof(StringModelParams, decay), offsetof(StringModelParams, damping),
            offsetof(StringModelParams, decay_keytrack), offsetof(StringModelParams, release), offsetof(StringModelParams, stiffness), offsetof(StringModelParams, nonlinear),
            offsetof(StringModelParams, harmonic_node), offsetof(StringModelParams, harmonic_amt), offsetof(StringModelParams, buzz), offsetof(StringModelParams, pol_detune),
            offsetof(StringModelParams, pol_coupling), offsetof(StringModelParams, pol_width), offsetof(StringModelParams, body), offsetof(StringModelParams, body_size) };
        const float lo[] = {0,0,0,0,0,0,0,1,1,0.05f,0,0,0.01f,0,0,0,0,0,0,0,0,0,0}, hi[] = {100,100,100,100,100,100,100,50,50,30,100,100,10,100,100,9,100,100,20,100,100,100,100};
        const double notes[] = { 27.5, 261.63, 4186.0 };
        int bad = 0; float peak = 0;
        for (int ex = 0; ex < STRING_EXC_COUNT; ex++) for (int extreme = 0; extreme < 2; extreme++) for (int k = 0; k < 3; k++) {
            StringModelParams sp; hds_string_default_params(&sp); sp.exciter = (float)ex;
            for (size_t i = 0; i < sizeof offs / sizeof offs[0]; i++) *(float *)((char *)&sp + offs[i]) = extreme ? hi[i] : lo[i];
            render(&sp, notes[k], 48000, 36000, 127);
            for (int i = 0; i < 48000; i++) { if (!isfinite(g_buf[i])) bad++; else if (fabsf(g_buf[i]) > peak) peak = fabsf(g_buf[i]); }
        }
        test_check(bad == 0 && peak < 4.0f, "string_model/stable_at_extremes", "%d non-finite samples, peak %.3f", bad, peak);
    }
    return test_exit_code();
}
