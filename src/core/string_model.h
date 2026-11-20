// string_model.h -- the Plucked String oscillator (WAVE_STRING): a dual-polarization digital
// waveguide string with physical exciters. See string_model.c.
#ifndef HDS_STRING_MODEL_H
#define HDS_STRING_MODEL_H

#include "core/backend.h"

// One output sample of oscillator `osc`'s string for one voice. `*slot_ref` is that voice's
// SynthVoice.str_slot[osc]; `*trigger` its str_trigger[osc] (a pending note-on pluck, consumed
// here). `ext_in` is whatever the FM algorithm routes into this oscillator (used as excitation).
// Writes the stereo pair (pan-law weighted like the Unison Saw's) to out_l/out_r; returns mono.
float hds_string_render(HdsStringPool *pool, int *slot_ref, bool *trigger, const StringModelParams *sp,
                        float hz, float ext_in, bool released, int velocity, float *out_l, float *out_r);

// Gives a voice's string slot back to the pool (its Amp EG went idle, or the shape changed).
void hds_string_release(HdsStringPool *pool, int *slot_ref);

// Fills `sp` with the defaults a new Program starts from.
void hds_string_default_params(StringModelParams *sp);

#endif // HDS_STRING_MODEL_H
