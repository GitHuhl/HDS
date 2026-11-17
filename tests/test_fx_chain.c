// test_fx_chain.c -- runs every FX type through fx_chain.o and checks it behaves.
//
// For each FX_TYPE_* (except NAM, which needs a model file) and four parameter/mix/routing/sidechain
// variants: three chained slots process 1 s of deterministic noise on source 0 while a fourth slot
// processes source 1 (used as source 0's sidechain in two variants). Checks every output sample and
// meter is finite (and, for the in-range-parameter variants, bounded), and fingerprints the outputs (tests/golden.txt catches any change in
// the sound). A last case uses a 32-slot, 8-source bus with slots placed above index 12, the shape
// hds_fx.clap uses, to cover the runtime slot_count/source_count paths.
#include "test_common.h"
#include "fx/fx_chain.h"

#include <stdlib.h>

#define MAX_SLOTS 32

static void run_case(const char *name, int type, int variant, int slot_count, int source_count, int first_slot) {
    static FXSlotParams P[MAX_SLOTS];
    static FXSlotEngine E[MAX_SLOTS];
    memset(P, 0, sizeof P);
    for (int s = 0; s < slot_count; s++) { P[s].fx_source = -1; P[s].input_from_slot = -1; P[s].sidechain_source = -1; }
    test_rng_seed(1234u + (uint32_t)(type * 17 + variant));
    float scale = (variant & 1) ? 100.0f : 1.0f;
    for (int k = 0; k < 3; k++) {
        FXSlotParams *sp = &P[first_slot + k];
        sp->fx_type = (float)type; sp->mix = (variant & 2) ? 50.0f : 100.0f;
        for (int i = 0; i < FX_SLOT_PARAM_COUNT; i++) sp->param[i] = test_frand() * scale;
        sp->fx_source = 0; sp->chain_to_next = k < 2; sp->sidechain_source = (variant & 2) ? 1 : -1;
    }
    FXSlotParams *other = &P[first_slot + 3];
    other->fx_type = (float)type; other->fx_source = 1; other->mix = 100;
    for (int i = 0; i < FX_SLOT_PARAM_COUNT; i++) other->param[i] = test_frand();

    srand(7); // Airwindows ports seed their dither from rand() when their state is created
    reset_fx_engine(E, slot_count);
    sync_fx_engine_types(P, E, slot_count);

    float sc_l[8] = {0}, sc_r[8] = {0};
    float gr[MAX_SLOTS] = {0}, dump[MAX_SLOTS] = {0}, so_l[MAX_SLOTS] = {0}, so_r[MAX_SLOTS] = {0};
    TestHash h; test_hash_init(&h);
    int bad = 0; float peak = 0;
    for (int i = 0; i < 48000; i++) {
        float in_l = test_frand() * 2 - 1, in_r = test_frand() * 2 - 1;
        float note = ((i / 4800) % 2) ? 60.0f + (float)(i / 9600) : -1.0f;
        g_hardtune_held_note = note; g_hardtune_bend_semitones = 0.5f; g_hardtune_modwheel = 0.3f;
        g_hardtune_channel_pressure = 0.2f; g_bcastdly_notes_held = note >= 0; g_bcastdly_sustain_held = false;
        for (int src = 0; src < 2; src++) {
            float l = in_l, r = in_r;
            process_fx_chain(P, E, slot_count, variant & 1, src, sc_l, sc_r, source_count, &l, &r, gr, dump, so_l, so_r);
            if (!isfinite(l) || !isfinite(r)) bad++;
            else if (!(variant & 1) && (fabsf(l) > 1000.0f || fabsf(r) > 1000.0f)) bad++; // in-range params: bounded too
            if (fabsf(l) > peak) peak = fabsf(l);
            sc_l[src] = l; sc_r[src] = r;
            test_hash_bytes(&h, &l, sizeof l); test_hash_bytes(&h, &r, sizeof r);
        }
        for (int s = 0; s < slot_count; s++) if (!isfinite(gr[s]) || !isfinite(dump[s])) bad++;
    }
    test_hash_bytes(&h, gr, sizeof gr);
    test_check(bad == 0, name, "%d non-finite or out-of-range output/meter values (peak %g)", bad, peak);
    test_hash_print(&h, name);
}

int main(void) {
    char name[64];
    for (int type = 1; type <= 25; type++) {
        if (type == FX_TYPE_NAM) continue;
        for (int v = 0; v < 4; v++) {
            snprintf(name, sizeof name, "fx_chain/type%02d/v%d", type, v);
            run_case(name, type, v, 12, 2, 0);
        }
    }
    run_case("fx_chain/32slot_8source/eq3", FX_TYPE_EQ3, 0, 32, 8, 20);
    run_case("fx_chain/32slot_8source/fdnverb", FX_TYPE_FDNVERB, 2, 32, 8, 25);
    return test_exit_code();
}
