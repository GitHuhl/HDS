// osc_protocol.c -- the OSC control protocol: the OSC_PARAMS[] address table (every control's
// address -> SynthSharedControls field), the O(1) dispatch table built from it, the generic
// catch-all handler that applies a float/string to the addressed field, the full-state dump
// (/hds/request_state), multi-instance discovery (/hds/ping, /hds/subscribe), and the periodic
// status pushes (send_osc_status/send_seq_audio_status). Moved verbatim out of backend.c; the
// public entry points are declared in backend.h.
#include "core/backend.h"

// ============================================================================
// OSC Control Protocol
// ============================================================================
// Replaces the old raw-memcpy-of-a-486-float-blob Unix-socket protocol with OSC (Open Sound
// Control, via liblo): every parameter is addressed by name (e.g. "/hds/vco1/shape"), not by
// byte position. This eliminates an entire bug class structurally rather than by hand-kept
// discipline -- a misplaced/mis-ordered field can no longer silently corrupt unrelated data the
// way record_format's wrong struct position once did (it landed 4 bytes into the string region
// and collapsed record_path to an empty string on every recording attempt). Every table entry
// below uses offsetof(SynthSharedControls, ...) directly, so a typo'd field name is a COMPILE
// ERROR, not a silent runtime miswrite -- the core safety property this redesign is for.
//
// Transport: plain UDP via a single non-threaded lo_server, polled once per audio block from
// main()'s loop (see the "while (lo_server_recv_noblock(...))" call site) -- deliberately NOT
// lo_server_thread, preserving the existing invariant that only main()'s own thread ever touches
// active_knobs/g_sample_slots, no new locking required. Incoming control listens on
// --osc-listen-port (default 9000); outgoing status (see send_osc_status()) goes to
// --osc-reply-host/--osc-reply-port (default 127.0.0.1:9001) -- two independent, explicitly
// configured ports, matching how real OSC tools (TouchOSC, Max, Pd, python-osc) already expect
// to be wired up, rather than "reply to whoever last sent a message."

// One row = one address template. A plain scalar has num_indices=0 (address matched verbatim).
// A single-index array (e.g. one LFO property across all LFO_COUNT LFOs) has num_indices=1:
// `address` contains one "%d", `stride` is sizeof(one array element), `count` is the valid
// index range. The one genuinely nested case (FX slot param banks, fx[FX_SLOT_COUNT].
// param[FX_SLOT_PARAM_COUNT]) has num_indices=2: `address` contains two "%d"s, `stride`/`count`
// index the outer array, `stride2`/`count2` index the inner one.
typedef struct {
    const char *address;
    size_t base_offset;
    size_t stride;
    int count;
    int num_indices;
    size_t stride2;
    int count2;
    OscParamKind kind;
    size_t str_len; // OSC_KIND_STRING only: size of the fixed char buffer, for safe truncation
} OscParamEntry;

#define OSC_SCALAR(addr, field) \
    { addr, offsetof(SynthSharedControls, field), 0, 1, 0, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_ARRAY(addr, field, cnt) \
    { addr, offsetof(SynthSharedControls, field), sizeof(((SynthSharedControls *)0)->field[0]), cnt, 1, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_SUBARRAY(addr, arrfield, subtype, subfield, cnt) \
    { addr, offsetof(SynthSharedControls, arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->arrfield[0]), cnt, 1, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_NESTED(addr, arrfield, subtype, subfield, cnt1, cnt2) \
    { addr, offsetof(SynthSharedControls, arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->arrfield[0]), cnt1, 2, sizeof(float), cnt2, OSC_KIND_FLOAT, 0 }
#define OSC_STRING(addr, field, len) \
    { addr, offsetof(SynthSharedControls, field), 0, 1, 0, 0, 0, OSC_KIND_STRING, len }
#define OSC_STRING_SUBARRAY(addr, arrfield, subtype, subfield, cnt, len) \
    { addr, offsetof(SynthSharedControls, arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->arrfield[0]), cnt, 1, 0, 0, OSC_KIND_STRING, len }

// Program-scoped field variants (Combi mode, see ProgramState's own comment): identical shape to
// the macros above, but the base offset threads through SynthSharedControls.programs[N] before
// reaching the field. The _AT forms take an explicit compile-time program index N (used to
// generate the 8 real Combi lane address rows, /hds/combi/{0-7}/...); the plain (no _AT) forms
// default to SINGLE_PROGRAM_SLOT (index 8), so the existing, unprefixed addresses (e.g.
// /hds/vco1/shape) now address Single Program mode's own dedicated ProgramState -- NOT any Combi
// lane's storage, per the mutual-exclusivity fix (see ProgramState's own comment). Single Program
// mode and every existing saved .hdsprog are unaffected by this indirection -- same addresses,
// same wire format, just a different (and now correctly independent) target underneath.
#define OSC_PSCALAR_AT(addr, field, n) \
    { addr, offsetof(SynthSharedControls, programs[n].field), 0, 1, 0, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_PARRAY_AT(addr, field, cnt, n) \
    { addr, offsetof(SynthSharedControls, programs[n].field), sizeof(((SynthSharedControls *)0)->programs[n].field[0]), cnt, 1, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_PSUBARRAY_AT(addr, arrfield, subtype, subfield, cnt, n) \
    { addr, offsetof(SynthSharedControls, programs[n].arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->programs[n].arrfield[0]), cnt, 1, 0, 0, OSC_KIND_FLOAT, 0 }
#define OSC_PNESTED_AT(addr, arrfield, subtype, subfield, cnt1, cnt2, n) \
    { addr, offsetof(SynthSharedControls, programs[n].arrfield) + offsetof(subtype, subfield), sizeof(((SynthSharedControls *)0)->programs[n].arrfield[0]), cnt1, 2, sizeof(float), cnt2, OSC_KIND_FLOAT, 0 }
#define OSC_PSCALAR(addr, field) OSC_PSCALAR_AT(addr, field, SINGLE_PROGRAM_SLOT)
#define OSC_PARRAY(addr, field, cnt) OSC_PARRAY_AT(addr, field, cnt, SINGLE_PROGRAM_SLOT)
#define OSC_PSUBARRAY(addr, arrfield, subtype, subfield, cnt) OSC_PSUBARRAY_AT(addr, arrfield, subtype, subfield, cnt, SINGLE_PROGRAM_SLOT)
#define OSC_PNESTED(addr, arrfield, subtype, subfield, cnt1, cnt2) OSC_PNESTED_AT(addr, arrfield, subtype, subfield, cnt1, cnt2, SINGLE_PROGRAM_SLOT)

static const OscParamEntry OSC_PARAMS[] = {
    // --- Baseline ---
    OSC_PSCALAR("/hds/is_polyphonic", is_polyphonic),
    OSC_PSCALAR_AT("/hds/combi/0/is_polyphonic", is_polyphonic, 0),
    OSC_PSCALAR_AT("/hds/combi/1/is_polyphonic", is_polyphonic, 1),
    OSC_PSCALAR_AT("/hds/combi/2/is_polyphonic", is_polyphonic, 2),
    OSC_PSCALAR_AT("/hds/combi/3/is_polyphonic", is_polyphonic, 3),
    OSC_PSCALAR_AT("/hds/combi/4/is_polyphonic", is_polyphonic, 4),
    OSC_PSCALAR_AT("/hds/combi/5/is_polyphonic", is_polyphonic, 5),
    OSC_PSCALAR_AT("/hds/combi/6/is_polyphonic", is_polyphonic, 6),
    OSC_PSCALAR_AT("/hds/combi/7/is_polyphonic", is_polyphonic, 7),
    OSC_PSCALAR("/hds/vco1/shape", vco1_shape),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/shape", vco1_shape, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/shape", vco1_shape, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/shape", vco1_shape, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/shape", vco1_shape, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/shape", vco1_shape, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/shape", vco1_shape, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/shape", vco1_shape, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/shape", vco1_shape, 7),
    OSC_PSCALAR("/hds/vco2/shape", vco2_shape),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/shape", vco2_shape, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/shape", vco2_shape, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/shape", vco2_shape, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/shape", vco2_shape, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/shape", vco2_shape, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/shape", vco2_shape, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/shape", vco2_shape, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/shape", vco2_shape, 7),
    OSC_PSCALAR("/hds/fm/intensity", fm_intensity),
    OSC_PSCALAR_AT("/hds/combi/0/fm/intensity", fm_intensity, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/intensity", fm_intensity, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/intensity", fm_intensity, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/intensity", fm_intensity, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/intensity", fm_intensity, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/intensity", fm_intensity, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/intensity", fm_intensity, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/intensity", fm_intensity, 7),
    OSC_PSCALAR("/hds/vco1/vol", vco1_vol),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/vol", vco1_vol, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/vol", vco1_vol, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/vol", vco1_vol, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/vol", vco1_vol, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/vol", vco1_vol, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/vol", vco1_vol, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/vol", vco1_vol, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/vol", vco1_vol, 7),
    OSC_PSCALAR("/hds/vco2/vol", vco2_vol),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/vol", vco2_vol, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/vol", vco2_vol, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/vol", vco2_vol, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/vol", vco2_vol, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/vol", vco2_vol, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/vol", vco2_vol, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/vol", vco2_vol, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/vol", vco2_vol, 7),
    OSC_PSCALAR("/hds/portamento_time_ms", portamento_time_ms),
    OSC_PSCALAR_AT("/hds/combi/0/portamento_time_ms", portamento_time_ms, 0),
    OSC_PSCALAR_AT("/hds/combi/1/portamento_time_ms", portamento_time_ms, 1),
    OSC_PSCALAR_AT("/hds/combi/2/portamento_time_ms", portamento_time_ms, 2),
    OSC_PSCALAR_AT("/hds/combi/3/portamento_time_ms", portamento_time_ms, 3),
    OSC_PSCALAR_AT("/hds/combi/4/portamento_time_ms", portamento_time_ms, 4),
    OSC_PSCALAR_AT("/hds/combi/5/portamento_time_ms", portamento_time_ms, 5),
    OSC_PSCALAR_AT("/hds/combi/6/portamento_time_ms", portamento_time_ms, 6),
    OSC_PSCALAR_AT("/hds/combi/7/portamento_time_ms", portamento_time_ms, 7),
    OSC_SCALAR("/hds/master_volume", master_volume),
    OSC_SCALAR("/hds/transpose_semitones", transpose_semitones),
    OSC_SCALAR("/hds/tuning/enable", tuning_enable),
    OSC_SCALAR("/hds/tuning/reference_note", tuning_reference_note),
    OSC_SCALAR("/hds/tuning/reference_freq", tuning_reference_freq),
    OSC_PSCALAR("/hds/vco1/pan", vco1_pan),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/pan", vco1_pan, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/pan", vco1_pan, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/pan", vco1_pan, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/pan", vco1_pan, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/pan", vco1_pan, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/pan", vco1_pan, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/pan", vco1_pan, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/pan", vco1_pan, 7),
    OSC_PSCALAR("/hds/vco2/pan", vco2_pan),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/pan", vco2_pan, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/pan", vco2_pan, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/pan", vco2_pan, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/pan", vco2_pan, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/pan", vco2_pan, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/pan", vco2_pan, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/pan", vco2_pan, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/pan", vco2_pan, 7),
    OSC_PSCALAR("/hds/vco1/fine_tune", vco1_fine_tune),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/fine_tune", vco1_fine_tune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/fine_tune", vco1_fine_tune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/fine_tune", vco1_fine_tune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/fine_tune", vco1_fine_tune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/fine_tune", vco1_fine_tune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/fine_tune", vco1_fine_tune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/fine_tune", vco1_fine_tune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/fine_tune", vco1_fine_tune, 7),
    OSC_PSCALAR("/hds/vco2/pitch_offset", vco2_pitch_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/pitch_offset", vco2_pitch_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/pitch_offset", vco2_pitch_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/pitch_offset", vco2_pitch_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/pitch_offset", vco2_pitch_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/pitch_offset", vco2_pitch_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/pitch_offset", vco2_pitch_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/pitch_offset", vco2_pitch_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/pitch_offset", vco2_pitch_offset, 7),
    OSC_PSCALAR("/hds/vco2/fine_tune", vco2_fine_tune),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/fine_tune", vco2_fine_tune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/fine_tune", vco2_fine_tune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/fine_tune", vco2_fine_tune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/fine_tune", vco2_fine_tune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/fine_tune", vco2_fine_tune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/fine_tune", vco2_fine_tune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/fine_tune", vco2_fine_tune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/fine_tune", vco2_fine_tune, 7),

    // --- Envelopes (eg[ENV_COUNT], PackEGParams: d=delay h=hold a=attack dec=decay s=sustain
    // r=release al=attack_start_pct pk=attack_peak_pct -- see tick_adsr()) ---
    OSC_PSUBARRAY("/hds/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/delay", eg, PackEGParams, d, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/hold", eg, PackEGParams, h, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/attack", eg, PackEGParams, a, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/decay", eg, PackEGParams, dec, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/sustain", eg, PackEGParams, s, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/release", eg, PackEGParams, r, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/release", eg, PackEGParams, r, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/attack_start_pct", eg, PackEGParams, al, ENV_COUNT, 7),
    OSC_PSUBARRAY("/hds/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eg/%d/attack_peak_pct", eg, PackEGParams, pk, ENV_COUNT, 7),
    OSC_PARRAY("/hds/eg/%d/topology_mode", eg_mode, ENV_COUNT),
    OSC_PARRAY_AT("/hds/combi/0/eg/%d/topology_mode", eg_mode, ENV_COUNT, 0),
    OSC_PARRAY_AT("/hds/combi/1/eg/%d/topology_mode", eg_mode, ENV_COUNT, 1),
    OSC_PARRAY_AT("/hds/combi/2/eg/%d/topology_mode", eg_mode, ENV_COUNT, 2),
    OSC_PARRAY_AT("/hds/combi/3/eg/%d/topology_mode", eg_mode, ENV_COUNT, 3),
    OSC_PARRAY_AT("/hds/combi/4/eg/%d/topology_mode", eg_mode, ENV_COUNT, 4),
    OSC_PARRAY_AT("/hds/combi/5/eg/%d/topology_mode", eg_mode, ENV_COUNT, 5),
    OSC_PARRAY_AT("/hds/combi/6/eg/%d/topology_mode", eg_mode, ENV_COUNT, 6),
    OSC_PARRAY_AT("/hds/combi/7/eg/%d/topology_mode", eg_mode, ENV_COUNT, 7),

    // --- Filters: 5 flat, interchangeable slots (filters[5]), chained in Series or
    // summed in Parallel (filter_chain_mode) -- see the filter section simplification's
    // own comments on ProgramState.filters/FilterParams. ---
    OSC_PSCALAR("/hds/filter/chain_mode", filter_chain_mode),
    OSC_PSCALAR_AT("/hds/combi/0/filter/chain_mode", filter_chain_mode, 0),
    OSC_PSCALAR_AT("/hds/combi/1/filter/chain_mode", filter_chain_mode, 1),
    OSC_PSCALAR_AT("/hds/combi/2/filter/chain_mode", filter_chain_mode, 2),
    OSC_PSCALAR_AT("/hds/combi/3/filter/chain_mode", filter_chain_mode, 3),
    OSC_PSCALAR_AT("/hds/combi/4/filter/chain_mode", filter_chain_mode, 4),
    OSC_PSCALAR_AT("/hds/combi/5/filter/chain_mode", filter_chain_mode, 5),
    OSC_PSCALAR_AT("/hds/combi/6/filter/chain_mode", filter_chain_mode, 6),
    OSC_PSCALAR_AT("/hds/combi/7/filter/chain_mode", filter_chain_mode, 7),
    OSC_PSUBARRAY("/hds/filter/%d/cutoff", filters, FilterParams, cutoff, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/cutoff", filters, FilterParams, cutoff, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/res_q", filters, FilterParams, res_q, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/res_q", filters, FilterParams, res_q, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/res_q", filters, FilterParams, res_q, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/res_q", filters, FilterParams, res_q, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/res_q", filters, FilterParams, res_q, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/res_q", filters, FilterParams, res_q, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/res_q", filters, FilterParams, res_q, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/res_q", filters, FilterParams, res_q, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/res_q", filters, FilterParams, res_q, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/slope", filters, FilterParams, slope_poles, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/slope", filters, FilterParams, slope_poles, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/slope", filters, FilterParams, slope_poles, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/slope", filters, FilterParams, slope_poles, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/slope", filters, FilterParams, slope_poles, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/slope", filters, FilterParams, slope_poles, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/slope", filters, FilterParams, slope_poles, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/slope", filters, FilterParams, slope_poles, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/slope", filters, FilterParams, slope_poles, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/type", filters, FilterParams, filter_type, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/type", filters, FilterParams, filter_type, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/type", filters, FilterParams, filter_type, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/type", filters, FilterParams, filter_type, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/type", filters, FilterParams, filter_type, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/type", filters, FilterParams, filter_type, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/type", filters, FilterParams, filter_type, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/type", filters, FilterParams, filter_type, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/type", filters, FilterParams, filter_type, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/key_track", filters, FilterParams, key_track, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/key_track", filters, FilterParams, key_track, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/key_track", filters, FilterParams, key_track, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/key_track", filters, FilterParams, key_track, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/key_track", filters, FilterParams, key_track, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/key_track", filters, FilterParams, key_track, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/key_track", filters, FilterParams, key_track, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/key_track", filters, FilterParams, key_track, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/key_track", filters, FilterParams, key_track, 5, 7),
    OSC_PSUBARRAY("/hds/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5),
    OSC_PSUBARRAY_AT("/hds/combi/0/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/filter/%d/cc_source", filters, FilterParams, cutoff_mod_source_cc, 5, 7),

    // --- Post-filter 3-band EQ (eq_bands[3], EqBandParams -- 0=Low Shelf, 1=Mid Peak, 2=High
    // Shelf, see EqBandParams' own comment) ---
    OSC_PSUBARRAY("/hds/eq/%d/freq", eq_bands, EqBandParams, freq, 3),
    OSC_PSUBARRAY_AT("/hds/combi/0/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eq/%d/freq", eq_bands, EqBandParams, freq, 3, 7),
    OSC_PSUBARRAY("/hds/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3),
    OSC_PSUBARRAY_AT("/hds/combi/0/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eq/%d/gain", eq_bands, EqBandParams, gain_db, 3, 7),
    OSC_PSUBARRAY("/hds/eq/%d/q", eq_bands, EqBandParams, q, 3),
    OSC_PSUBARRAY_AT("/hds/combi/0/eq/%d/q", eq_bands, EqBandParams, q, 3, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/eq/%d/q", eq_bands, EqBandParams, q, 3, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/eq/%d/q", eq_bands, EqBandParams, q, 3, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/eq/%d/q", eq_bands, EqBandParams, q, 3, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/eq/%d/q", eq_bands, EqBandParams, q, 3, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/eq/%d/q", eq_bands, EqBandParams, q, 3, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/eq/%d/q", eq_bands, EqBandParams, q, 3, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/eq/%d/q", eq_bands, EqBandParams, q, 3, 7),

    // --- LFOs (lfo[LFO_COUNT], PackLfoParams) ---
    OSC_PSUBARRAY("/hds/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/wave", lfo, PackLfoParams, wave_type, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/rate", lfo, PackLfoParams, rate_hz, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/depth", lfo, PackLfoParams, mod_depth, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/key_sync", lfo, PackLfoParams, key_sync, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/dest", lfo, PackLfoParams, dest_target, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/phase_offset", lfo, PackLfoParams, phase_offset, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/mod_source_cc", lfo, PackLfoParams, mod_source_cc, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/per_key", lfo, PackLfoParams, per_key_mode, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/key_track", lfo, PackLfoParams, key_track, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/vel_track", lfo, PackLfoParams, vel_track, LFO_COUNT, 7),
    OSC_PSUBARRAY("/hds/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/tempo_sync", lfo, PackLfoParams, tempo_sync_division, LFO_COUNT, 7),

    // --- Assignable EG modulators ---
    OSC_PARRAY("/hds/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT),
    OSC_PARRAY_AT("/hds/combi/0/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 0),
    OSC_PARRAY_AT("/hds/combi/1/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 1),
    OSC_PARRAY_AT("/hds/combi/2/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 2),
    OSC_PARRAY_AT("/hds/combi/3/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 3),
    OSC_PARRAY_AT("/hds/combi/4/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 4),
    OSC_PARRAY_AT("/hds/combi/5/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 5),
    OSC_PARRAY_AT("/hds/combi/6/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 6),
    OSC_PARRAY_AT("/hds/combi/7/eg_mod/%d/intensity", eg_intensity, ASSIGNABLE_EG_COUNT, 7),
    OSC_PARRAY("/hds/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT),
    OSC_PARRAY_AT("/hds/combi/0/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 0),
    OSC_PARRAY_AT("/hds/combi/1/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 1),
    OSC_PARRAY_AT("/hds/combi/2/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 2),
    OSC_PARRAY_AT("/hds/combi/3/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 3),
    OSC_PARRAY_AT("/hds/combi/4/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 4),
    OSC_PARRAY_AT("/hds/combi/5/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 5),
    OSC_PARRAY_AT("/hds/combi/6/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 6),
    OSC_PARRAY_AT("/hds/combi/7/eg_mod/%d/dest", eg_dest_target, ASSIGNABLE_EG_COUNT, 7),

    // --- Unison Saw ---
    OSC_PSCALAR("/hds/vco1/unison/detune", vco1_unison_saw_detune),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/unison/detune", vco1_unison_saw_detune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/unison/detune", vco1_unison_saw_detune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/unison/detune", vco1_unison_saw_detune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/unison/detune", vco1_unison_saw_detune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/unison/detune", vco1_unison_saw_detune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/unison/detune", vco1_unison_saw_detune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/unison/detune", vco1_unison_saw_detune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/unison/detune", vco1_unison_saw_detune, 7),
    OSC_PSCALAR("/hds/vco1/unison/width", vco1_unison_saw_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/unison/width", vco1_unison_saw_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/unison/width", vco1_unison_saw_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/unison/width", vco1_unison_saw_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/unison/width", vco1_unison_saw_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/unison/width", vco1_unison_saw_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/unison/width", vco1_unison_saw_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/unison/width", vco1_unison_saw_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/unison/width", vco1_unison_saw_width, 7),
    OSC_PSCALAR("/hds/vco2/unison/detune", vco2_unison_saw_detune),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/unison/detune", vco2_unison_saw_detune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/unison/detune", vco2_unison_saw_detune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/unison/detune", vco2_unison_saw_detune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/unison/detune", vco2_unison_saw_detune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/unison/detune", vco2_unison_saw_detune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/unison/detune", vco2_unison_saw_detune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/unison/detune", vco2_unison_saw_detune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/unison/detune", vco2_unison_saw_detune, 7),
    OSC_PSCALAR("/hds/vco2/unison/width", vco2_unison_saw_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/unison/width", vco2_unison_saw_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/unison/width", vco2_unison_saw_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/unison/width", vco2_unison_saw_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/unison/width", vco2_unison_saw_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/unison/width", vco2_unison_saw_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/unison/width", vco2_unison_saw_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/unison/width", vco2_unison_saw_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/unison/width", vco2_unison_saw_width, 7),
    OSC_PSCALAR("/hds/vco3/unison/detune", vco3_unison_saw_detune),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/unison/detune", vco3_unison_saw_detune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/unison/detune", vco3_unison_saw_detune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/unison/detune", vco3_unison_saw_detune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/unison/detune", vco3_unison_saw_detune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/unison/detune", vco3_unison_saw_detune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/unison/detune", vco3_unison_saw_detune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/unison/detune", vco3_unison_saw_detune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/unison/detune", vco3_unison_saw_detune, 7),
    OSC_PSCALAR("/hds/vco3/unison/width", vco3_unison_saw_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/unison/width", vco3_unison_saw_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/unison/width", vco3_unison_saw_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/unison/width", vco3_unison_saw_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/unison/width", vco3_unison_saw_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/unison/width", vco3_unison_saw_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/unison/width", vco3_unison_saw_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/unison/width", vco3_unison_saw_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/unison/width", vco3_unison_saw_width, 7),
    OSC_PSCALAR("/hds/vco4/unison/detune", vco4_unison_saw_detune),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/unison/detune", vco4_unison_saw_detune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/unison/detune", vco4_unison_saw_detune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/unison/detune", vco4_unison_saw_detune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/unison/detune", vco4_unison_saw_detune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/unison/detune", vco4_unison_saw_detune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/unison/detune", vco4_unison_saw_detune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/unison/detune", vco4_unison_saw_detune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/unison/detune", vco4_unison_saw_detune, 7),
    OSC_PSCALAR("/hds/vco4/unison/width", vco4_unison_saw_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/unison/width", vco4_unison_saw_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/unison/width", vco4_unison_saw_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/unison/width", vco4_unison_saw_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/unison/width", vco4_unison_saw_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/unison/width", vco4_unison_saw_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/unison/width", vco4_unison_saw_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/unison/width", vco4_unison_saw_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/unison/width", vco4_unison_saw_width, 7),

    OSC_PSCALAR("/hds/mono_glide_random", mono_glide_random),
    OSC_PSCALAR_AT("/hds/combi/0/mono_glide_random", mono_glide_random, 0),
    OSC_PSCALAR_AT("/hds/combi/1/mono_glide_random", mono_glide_random, 1),
    OSC_PSCALAR_AT("/hds/combi/2/mono_glide_random", mono_glide_random, 2),
    OSC_PSCALAR_AT("/hds/combi/3/mono_glide_random", mono_glide_random, 3),
    OSC_PSCALAR_AT("/hds/combi/4/mono_glide_random", mono_glide_random, 4),
    OSC_PSCALAR_AT("/hds/combi/5/mono_glide_random", mono_glide_random, 5),
    OSC_PSCALAR_AT("/hds/combi/6/mono_glide_random", mono_glide_random, 6),
    OSC_PSCALAR_AT("/hds/combi/7/mono_glide_random", mono_glide_random, 7),
    OSC_PSCALAR("/hds/vco1/mute", vco1_mute),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/mute", vco1_mute, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/mute", vco1_mute, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/mute", vco1_mute, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/mute", vco1_mute, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/mute", vco1_mute, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/mute", vco1_mute, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/mute", vco1_mute, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/mute", vco1_mute, 7),
    OSC_PSCALAR("/hds/vco2/mute", vco2_mute),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/mute", vco2_mute, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/mute", vco2_mute, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/mute", vco2_mute, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/mute", vco2_mute, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/mute", vco2_mute, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/mute", vco2_mute, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/mute", vco2_mute, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/mute", vco2_mute, 7),
    OSC_PSCALAR("/hds/hard_sync_mode", hard_sync_mode),
    OSC_PSCALAR_AT("/hds/combi/0/hard_sync_mode", hard_sync_mode, 0),
    OSC_PSCALAR_AT("/hds/combi/1/hard_sync_mode", hard_sync_mode, 1),
    OSC_PSCALAR_AT("/hds/combi/2/hard_sync_mode", hard_sync_mode, 2),
    OSC_PSCALAR_AT("/hds/combi/3/hard_sync_mode", hard_sync_mode, 3),
    OSC_PSCALAR_AT("/hds/combi/4/hard_sync_mode", hard_sync_mode, 4),
    OSC_PSCALAR_AT("/hds/combi/5/hard_sync_mode", hard_sync_mode, 5),
    OSC_PSCALAR_AT("/hds/combi/6/hard_sync_mode", hard_sync_mode, 6),
    OSC_PSCALAR_AT("/hds/combi/7/hard_sync_mode", hard_sync_mode, 7),

    // --- Arpeggiator (see the Arpeggiator section on ProgramState + poll_arpeggiator() for the
    // full design) ---
    OSC_PSCALAR("/hds/arp/enabled", arp_enabled),
    OSC_PSCALAR_AT("/hds/combi/0/arp/enabled", arp_enabled, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/enabled", arp_enabled, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/enabled", arp_enabled, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/enabled", arp_enabled, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/enabled", arp_enabled, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/enabled", arp_enabled, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/enabled", arp_enabled, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/enabled", arp_enabled, 7),
    OSC_PSCALAR("/hds/arp/style", arp_style),
    OSC_PSCALAR_AT("/hds/combi/0/arp/style", arp_style, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/style", arp_style, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/style", arp_style, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/style", arp_style, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/style", arp_style, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/style", arp_style, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/style", arp_style, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/style", arp_style, 7),
    OSC_PSCALAR("/hds/arp/octave_range", arp_octave_range),
    OSC_PSCALAR_AT("/hds/combi/0/arp/octave_range", arp_octave_range, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/octave_range", arp_octave_range, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/octave_range", arp_octave_range, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/octave_range", arp_octave_range, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/octave_range", arp_octave_range, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/octave_range", arp_octave_range, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/octave_range", arp_octave_range, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/octave_range", arp_octave_range, 7),
    OSC_PSCALAR("/hds/arp/rate", arp_rate),
    OSC_PSCALAR_AT("/hds/combi/0/arp/rate", arp_rate, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/rate", arp_rate, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/rate", arp_rate, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/rate", arp_rate, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/rate", arp_rate, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/rate", arp_rate, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/rate", arp_rate, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/rate", arp_rate, 7),
    OSC_PSCALAR("/hds/arp/gate_pct", arp_gate_pct),
    OSC_PSCALAR_AT("/hds/combi/0/arp/gate_pct", arp_gate_pct, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/gate_pct", arp_gate_pct, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/gate_pct", arp_gate_pct, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/gate_pct", arp_gate_pct, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/gate_pct", arp_gate_pct, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/gate_pct", arp_gate_pct, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/gate_pct", arp_gate_pct, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/gate_pct", arp_gate_pct, 7),
    OSC_PSCALAR("/hds/arp/hold", arp_hold),
    OSC_PSCALAR_AT("/hds/combi/0/arp/hold", arp_hold, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/hold", arp_hold, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/hold", arp_hold, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/hold", arp_hold, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/hold", arp_hold, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/hold", arp_hold, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/hold", arp_hold, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/hold", arp_hold, 7),
    OSC_PSCALAR("/hds/arp/velocity", arp_velocity),
    OSC_PSCALAR_AT("/hds/combi/0/arp/velocity", arp_velocity, 0),
    OSC_PSCALAR_AT("/hds/combi/1/arp/velocity", arp_velocity, 1),
    OSC_PSCALAR_AT("/hds/combi/2/arp/velocity", arp_velocity, 2),
    OSC_PSCALAR_AT("/hds/combi/3/arp/velocity", arp_velocity, 3),
    OSC_PSCALAR_AT("/hds/combi/4/arp/velocity", arp_velocity, 4),
    OSC_PSCALAR_AT("/hds/combi/5/arp/velocity", arp_velocity, 5),
    OSC_PSCALAR_AT("/hds/combi/6/arp/velocity", arp_velocity, 6),
    OSC_PSCALAR_AT("/hds/combi/7/arp/velocity", arp_velocity, 7),

    OSC_PSCALAR("/hds/vco1/phase_offset", vco1_phase_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/phase_offset", vco1_phase_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/phase_offset", vco1_phase_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/phase_offset", vco1_phase_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/phase_offset", vco1_phase_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/phase_offset", vco1_phase_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/phase_offset", vco1_phase_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/phase_offset", vco1_phase_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/phase_offset", vco1_phase_offset, 7),
    OSC_PSCALAR("/hds/vco2/phase_offset", vco2_phase_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/phase_offset", vco2_phase_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/phase_offset", vco2_phase_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/phase_offset", vco2_phase_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/phase_offset", vco2_phase_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/phase_offset", vco2_phase_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/phase_offset", vco2_phase_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/phase_offset", vco2_phase_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/phase_offset", vco2_phase_offset, 7),
    OSC_PSCALAR("/hds/vco1/pulse_width", vco1_pulse_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/pulse_width", vco1_pulse_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/pulse_width", vco1_pulse_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/pulse_width", vco1_pulse_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/pulse_width", vco1_pulse_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/pulse_width", vco1_pulse_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/pulse_width", vco1_pulse_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/pulse_width", vco1_pulse_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/pulse_width", vco1_pulse_width, 7),
    OSC_PSCALAR("/hds/vco2/pulse_width", vco2_pulse_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/pulse_width", vco2_pulse_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/pulse_width", vco2_pulse_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/pulse_width", vco2_pulse_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/pulse_width", vco2_pulse_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/pulse_width", vco2_pulse_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/pulse_width", vco2_pulse_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/pulse_width", vco2_pulse_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/pulse_width", vco2_pulse_width, 7),

    OSC_PSCALAR("/hds/vco3/vol", vco3_vol),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/vol", vco3_vol, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/vol", vco3_vol, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/vol", vco3_vol, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/vol", vco3_vol, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/vol", vco3_vol, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/vol", vco3_vol, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/vol", vco3_vol, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/vol", vco3_vol, 7),
    OSC_PSCALAR("/hds/vco3/pan", vco3_pan),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/pan", vco3_pan, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/pan", vco3_pan, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/pan", vco3_pan, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/pan", vco3_pan, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/pan", vco3_pan, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/pan", vco3_pan, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/pan", vco3_pan, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/pan", vco3_pan, 7),
    OSC_PSCALAR("/hds/vco3/mute", vco3_mute),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/mute", vco3_mute, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/mute", vco3_mute, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/mute", vco3_mute, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/mute", vco3_mute, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/mute", vco3_mute, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/mute", vco3_mute, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/mute", vco3_mute, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/mute", vco3_mute, 7),
    OSC_PSCALAR("/hds/vco3/pulse_width", vco3_pulse_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/pulse_width", vco3_pulse_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/pulse_width", vco3_pulse_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/pulse_width", vco3_pulse_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/pulse_width", vco3_pulse_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/pulse_width", vco3_pulse_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/pulse_width", vco3_pulse_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/pulse_width", vco3_pulse_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/pulse_width", vco3_pulse_width, 7),
    OSC_PSCALAR("/hds/vco4/vol", vco4_vol),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/vol", vco4_vol, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/vol", vco4_vol, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/vol", vco4_vol, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/vol", vco4_vol, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/vol", vco4_vol, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/vol", vco4_vol, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/vol", vco4_vol, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/vol", vco4_vol, 7),
    OSC_PSCALAR("/hds/vco4/pan", vco4_pan),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/pan", vco4_pan, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/pan", vco4_pan, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/pan", vco4_pan, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/pan", vco4_pan, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/pan", vco4_pan, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/pan", vco4_pan, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/pan", vco4_pan, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/pan", vco4_pan, 7),
    OSC_PSCALAR("/hds/vco4/mute", vco4_mute),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/mute", vco4_mute, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/mute", vco4_mute, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/mute", vco4_mute, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/mute", vco4_mute, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/mute", vco4_mute, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/mute", vco4_mute, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/mute", vco4_mute, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/mute", vco4_mute, 7),
    OSC_PSCALAR("/hds/vco4/pulse_width", vco4_pulse_width),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/pulse_width", vco4_pulse_width, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/pulse_width", vco4_pulse_width, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/pulse_width", vco4_pulse_width, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/pulse_width", vco4_pulse_width, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/pulse_width", vco4_pulse_width, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/pulse_width", vco4_pulse_width, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/pulse_width", vco4_pulse_width, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/pulse_width", vco4_pulse_width, 7),

    // SFZ per-zone amp envelope override checkboxes -- see ProgramState.vco1_force_own_amp_eg's
    // own comment.
    OSC_PSCALAR("/hds/vco1/force_own_amp_eg", vco1_force_own_amp_eg),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/force_own_amp_eg", vco1_force_own_amp_eg, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/force_own_amp_eg", vco1_force_own_amp_eg, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/force_own_amp_eg", vco1_force_own_amp_eg, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/force_own_amp_eg", vco1_force_own_amp_eg, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/force_own_amp_eg", vco1_force_own_amp_eg, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/force_own_amp_eg", vco1_force_own_amp_eg, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/force_own_amp_eg", vco1_force_own_amp_eg, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/force_own_amp_eg", vco1_force_own_amp_eg, 7),
    OSC_PSCALAR("/hds/vco2/force_own_amp_eg", vco2_force_own_amp_eg),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/force_own_amp_eg", vco2_force_own_amp_eg, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/force_own_amp_eg", vco2_force_own_amp_eg, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/force_own_amp_eg", vco2_force_own_amp_eg, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/force_own_amp_eg", vco2_force_own_amp_eg, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/force_own_amp_eg", vco2_force_own_amp_eg, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/force_own_amp_eg", vco2_force_own_amp_eg, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/force_own_amp_eg", vco2_force_own_amp_eg, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/force_own_amp_eg", vco2_force_own_amp_eg, 7),
    OSC_PSCALAR("/hds/vco3/force_own_amp_eg", vco3_force_own_amp_eg),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/force_own_amp_eg", vco3_force_own_amp_eg, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/force_own_amp_eg", vco3_force_own_amp_eg, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/force_own_amp_eg", vco3_force_own_amp_eg, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/force_own_amp_eg", vco3_force_own_amp_eg, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/force_own_amp_eg", vco3_force_own_amp_eg, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/force_own_amp_eg", vco3_force_own_amp_eg, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/force_own_amp_eg", vco3_force_own_amp_eg, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/force_own_amp_eg", vco3_force_own_amp_eg, 7),
    OSC_PSCALAR("/hds/vco4/force_own_amp_eg", vco4_force_own_amp_eg),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/force_own_amp_eg", vco4_force_own_amp_eg, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/force_own_amp_eg", vco4_force_own_amp_eg, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/force_own_amp_eg", vco4_force_own_amp_eg, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/force_own_amp_eg", vco4_force_own_amp_eg, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/force_own_amp_eg", vco4_force_own_amp_eg, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/force_own_amp_eg", vco4_force_own_amp_eg, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/force_own_amp_eg", vco4_force_own_amp_eg, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/force_own_amp_eg", vco4_force_own_amp_eg, 7),

    OSC_ARRAY("/hds/ext_in/%d/enable", ext_in_enable, EXT_IN_PAIR_COUNT),
    OSC_ARRAY("/hds/ext_in/%d/gain", ext_in_gain, EXT_IN_PAIR_COUNT),
    OSC_ARRAY("/hds/ext_in/%d/stereo", ext_in_stereo, EXT_IN_PAIR_COUNT),
    OSC_ARRAY("/hds/drum_bus/%d/gain", drum_bus_gain, DRUM_BUS_COUNT),
    OSC_SCALAR("/hds/gui/routing_tab_visible", gui_routing_tab_visible),

    // --- FX Bus (fx[FX_SLOT_COUNT], FXSlotParams) ---
    OSC_SUBARRAY("/hds/fx/%d/type", fx, FXSlotParams, fx_type, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/mix", fx, FXSlotParams, mix, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/bypass", fx, FXSlotParams, bypass, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/source", fx, FXSlotParams, fx_source, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/chain_to_next", fx, FXSlotParams, chain_to_next, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/sidechain_source", fx, FXSlotParams, sidechain_source, FX_SLOT_COUNT),
    OSC_SUBARRAY("/hds/fx/%d/input_from_slot", fx, FXSlotParams, input_from_slot, FX_SLOT_COUNT),
    OSC_NESTED("/hds/fx/%d/param/%d", fx, FXSlotParams, param, FX_SLOT_COUNT, FX_SLOT_PARAM_COUNT),
    // FX_TYPE_NAM's own loaded-file paths -- plain string fields, generically saved/restored by
    // CLAP state (see clap_state_save/load's own K/T-line walk over this whole table) with no
    // special-casing needed there at all. Setting one of these live has no side effect by itself
    // (same as every other OSC_STRING_SUBARRAY field, e.g. Sequencer's own audio_path) -- the
    // actual model/IR (re)load happens once per block in poll_fx_nam_requests(), which notices the
    // path changed and does the real work, whether it changed via a live OSC message or a CLAP
    // state restore replaying this same field. See that function's own comment.
    OSC_STRING_SUBARRAY("/hds/fx/%d/nam_model_path", fx, FXSlotParams, nam_model_path, FX_SLOT_COUNT, SAMPLE_PATH_LEN),
    OSC_STRING_SUBARRAY("/hds/fx/%d/nam_ir_path", fx, FXSlotParams, nam_ir_path, FX_SLOT_COUNT, SAMPLE_PATH_LEN),

    // --- Multi-output hardware routing: which output pair(s) each source lands on ---
    OSC_NESTED("/hds/routing/%d/out/%d", source_routing, SourceRouting, enable, SOURCE_COUNT, OUTPUT_PAIR_MAX),

    OSC_PSCALAR("/hds/pb/range_up", pb_range_up),
    OSC_PSCALAR_AT("/hds/combi/0/pb/range_up", pb_range_up, 0),
    OSC_PSCALAR_AT("/hds/combi/1/pb/range_up", pb_range_up, 1),
    OSC_PSCALAR_AT("/hds/combi/2/pb/range_up", pb_range_up, 2),
    OSC_PSCALAR_AT("/hds/combi/3/pb/range_up", pb_range_up, 3),
    OSC_PSCALAR_AT("/hds/combi/4/pb/range_up", pb_range_up, 4),
    OSC_PSCALAR_AT("/hds/combi/5/pb/range_up", pb_range_up, 5),
    OSC_PSCALAR_AT("/hds/combi/6/pb/range_up", pb_range_up, 6),
    OSC_PSCALAR_AT("/hds/combi/7/pb/range_up", pb_range_up, 7),
    OSC_PSCALAR("/hds/pb/range_down", pb_range_down),
    OSC_PSCALAR_AT("/hds/combi/0/pb/range_down", pb_range_down, 0),
    OSC_PSCALAR_AT("/hds/combi/1/pb/range_down", pb_range_down, 1),
    OSC_PSCALAR_AT("/hds/combi/2/pb/range_down", pb_range_down, 2),
    OSC_PSCALAR_AT("/hds/combi/3/pb/range_down", pb_range_down, 3),
    OSC_PSCALAR_AT("/hds/combi/4/pb/range_down", pb_range_down, 4),
    OSC_PSCALAR_AT("/hds/combi/5/pb/range_down", pb_range_down, 5),
    OSC_PSCALAR_AT("/hds/combi/6/pb/range_down", pb_range_down, 6),
    OSC_PSCALAR_AT("/hds/combi/7/pb/range_down", pb_range_down, 7),
    OSC_PSCALAR("/hds/sustain/polarity", sustain_pedal_polarity),
    OSC_PSCALAR_AT("/hds/combi/0/sustain/polarity", sustain_pedal_polarity, 0),
    OSC_PSCALAR_AT("/hds/combi/1/sustain/polarity", sustain_pedal_polarity, 1),
    OSC_PSCALAR_AT("/hds/combi/2/sustain/polarity", sustain_pedal_polarity, 2),
    OSC_PSCALAR_AT("/hds/combi/3/sustain/polarity", sustain_pedal_polarity, 3),
    OSC_PSCALAR_AT("/hds/combi/4/sustain/polarity", sustain_pedal_polarity, 4),
    OSC_PSCALAR_AT("/hds/combi/5/sustain/polarity", sustain_pedal_polarity, 5),
    OSC_PSCALAR_AT("/hds/combi/6/sustain/polarity", sustain_pedal_polarity, 6),
    OSC_PSCALAR_AT("/hds/combi/7/sustain/polarity", sustain_pedal_polarity, 7),
    OSC_SCALAR("/hds/audio/latency_ms", audio_latency_ms),
    OSC_SCALAR("/hds/audio/polyphony", active_voice_limit),
    OSC_SCALAR("/hds/audio/block_size", block_size_frames),

    OSC_SCALAR("/hds/record/enable", record_enable),
    OSC_SCALAR("/hds/record/include_synth_output", record_include_synth_output),
    OSC_SCALAR("/hds/record/downmix_stereo", record_downmix_stereo),
    OSC_ARRAY("/hds/record/channel/%d/enable", record_input_channel_enable, RECORD_MAX_CHANNELS),
    OSC_SCALAR("/hds/record/format", record_format),
    OSC_SCALAR("/hds/record/wavpack_hybrid", record_wavpack_hybrid),
    OSC_SCALAR("/hds/record/wavpack_correction_file", record_wavpack_correction_file),
    OSC_SCALAR("/hds/record/wavpack_hybrid_bitrate", record_wavpack_hybrid_bitrate),
    OSC_SCALAR("/hds/record/wavpack_compression_level", record_wavpack_compression_level),

    OSC_PSCALAR("/hds/tempo/bpm", tempo_bpm),
    OSC_PSCALAR_AT("/hds/combi/0/tempo/bpm", tempo_bpm, 0),
    OSC_PSCALAR_AT("/hds/combi/1/tempo/bpm", tempo_bpm, 1),
    OSC_PSCALAR_AT("/hds/combi/2/tempo/bpm", tempo_bpm, 2),
    OSC_PSCALAR_AT("/hds/combi/3/tempo/bpm", tempo_bpm, 3),
    OSC_PSCALAR_AT("/hds/combi/4/tempo/bpm", tempo_bpm, 4),
    OSC_PSCALAR_AT("/hds/combi/5/tempo/bpm", tempo_bpm, 5),
    OSC_PSCALAR_AT("/hds/combi/6/tempo/bpm", tempo_bpm, 6),
    OSC_PSCALAR_AT("/hds/combi/7/tempo/bpm", tempo_bpm, 7),
    OSC_PSCALAR("/hds/tempo/marker_count", tempo_marker_count),
    OSC_PSCALAR_AT("/hds/combi/0/tempo/marker_count", tempo_marker_count, 0),
    OSC_PSCALAR_AT("/hds/combi/1/tempo/marker_count", tempo_marker_count, 1),
    OSC_PSCALAR_AT("/hds/combi/2/tempo/marker_count", tempo_marker_count, 2),
    OSC_PSCALAR_AT("/hds/combi/3/tempo/marker_count", tempo_marker_count, 3),
    OSC_PSCALAR_AT("/hds/combi/4/tempo/marker_count", tempo_marker_count, 4),
    OSC_PSCALAR_AT("/hds/combi/5/tempo/marker_count", tempo_marker_count, 5),
    OSC_PSCALAR_AT("/hds/combi/6/tempo/marker_count", tempo_marker_count, 6),
    OSC_PSCALAR_AT("/hds/combi/7/tempo/marker_count", tempo_marker_count, 7),
    OSC_PARRAY("/hds/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX),
    OSC_PARRAY_AT("/hds/combi/0/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 0),
    OSC_PARRAY_AT("/hds/combi/1/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 1),
    OSC_PARRAY_AT("/hds/combi/2/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 2),
    OSC_PARRAY_AT("/hds/combi/3/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 3),
    OSC_PARRAY_AT("/hds/combi/4/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 4),
    OSC_PARRAY_AT("/hds/combi/5/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 5),
    OSC_PARRAY_AT("/hds/combi/6/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 6),
    OSC_PARRAY_AT("/hds/combi/7/tempo/marker/%d/time", tempo_marker_time, TEMPO_MARKER_MAX, 7),
    OSC_PARRAY("/hds/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX),
    OSC_PARRAY_AT("/hds/combi/0/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 0),
    OSC_PARRAY_AT("/hds/combi/1/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 1),
    OSC_PARRAY_AT("/hds/combi/2/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 2),
    OSC_PARRAY_AT("/hds/combi/3/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 3),
    OSC_PARRAY_AT("/hds/combi/4/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 4),
    OSC_PARRAY_AT("/hds/combi/5/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 5),
    OSC_PARRAY_AT("/hds/combi/6/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 6),
    OSC_PARRAY_AT("/hds/combi/7/tempo/marker/%d/bpm", tempo_marker_bpm, TEMPO_MARKER_MAX, 7),

    OSC_PSCALAR("/hds/vco3/shape", vco3_shape),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/shape", vco3_shape, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/shape", vco3_shape, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/shape", vco3_shape, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/shape", vco3_shape, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/shape", vco3_shape, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/shape", vco3_shape, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/shape", vco3_shape, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/shape", vco3_shape, 7),
    OSC_PSCALAR("/hds/vco3/fine_tune", vco3_fine_tune),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/fine_tune", vco3_fine_tune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/fine_tune", vco3_fine_tune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/fine_tune", vco3_fine_tune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/fine_tune", vco3_fine_tune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/fine_tune", vco3_fine_tune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/fine_tune", vco3_fine_tune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/fine_tune", vco3_fine_tune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/fine_tune", vco3_fine_tune, 7),
    OSC_PSCALAR("/hds/vco3/pitch_offset", vco3_pitch_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/pitch_offset", vco3_pitch_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/pitch_offset", vco3_pitch_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/pitch_offset", vco3_pitch_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/pitch_offset", vco3_pitch_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/pitch_offset", vco3_pitch_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/pitch_offset", vco3_pitch_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/pitch_offset", vco3_pitch_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/pitch_offset", vco3_pitch_offset, 7),
    OSC_PSCALAR("/hds/vco3/phase_offset", vco3_phase_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/phase_offset", vco3_phase_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/phase_offset", vco3_phase_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/phase_offset", vco3_phase_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/phase_offset", vco3_phase_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/phase_offset", vco3_phase_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/phase_offset", vco3_phase_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/phase_offset", vco3_phase_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/phase_offset", vco3_phase_offset, 7),
    OSC_PSCALAR("/hds/vco4/shape", vco4_shape),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/shape", vco4_shape, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/shape", vco4_shape, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/shape", vco4_shape, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/shape", vco4_shape, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/shape", vco4_shape, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/shape", vco4_shape, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/shape", vco4_shape, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/shape", vco4_shape, 7),
    OSC_PSCALAR("/hds/vco4/fine_tune", vco4_fine_tune),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/fine_tune", vco4_fine_tune, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/fine_tune", vco4_fine_tune, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/fine_tune", vco4_fine_tune, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/fine_tune", vco4_fine_tune, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/fine_tune", vco4_fine_tune, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/fine_tune", vco4_fine_tune, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/fine_tune", vco4_fine_tune, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/fine_tune", vco4_fine_tune, 7),
    OSC_PSCALAR("/hds/vco4/pitch_offset", vco4_pitch_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/pitch_offset", vco4_pitch_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/pitch_offset", vco4_pitch_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/pitch_offset", vco4_pitch_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/pitch_offset", vco4_pitch_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/pitch_offset", vco4_pitch_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/pitch_offset", vco4_pitch_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/pitch_offset", vco4_pitch_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/pitch_offset", vco4_pitch_offset, 7),
    OSC_PSCALAR("/hds/vco4/phase_offset", vco4_phase_offset),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/phase_offset", vco4_phase_offset, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/phase_offset", vco4_phase_offset, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/phase_offset", vco4_phase_offset, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/phase_offset", vco4_phase_offset, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/phase_offset", vco4_phase_offset, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/phase_offset", vco4_phase_offset, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/phase_offset", vco4_phase_offset, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/phase_offset", vco4_phase_offset, 7),

    OSC_PSCALAR("/hds/fm/algorithm", fm_algorithm),
    OSC_PSCALAR_AT("/hds/combi/0/fm/algorithm", fm_algorithm, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/algorithm", fm_algorithm, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/algorithm", fm_algorithm, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/algorithm", fm_algorithm, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/algorithm", fm_algorithm, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/algorithm", fm_algorithm, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/algorithm", fm_algorithm, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/algorithm", fm_algorithm, 7),
    OSC_PSCALAR("/hds/fm/depth/3_1", fm_depth_3_1),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/3_1", fm_depth_3_1, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/3_1", fm_depth_3_1, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/3_1", fm_depth_3_1, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/3_1", fm_depth_3_1, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/3_1", fm_depth_3_1, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/3_1", fm_depth_3_1, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/3_1", fm_depth_3_1, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/3_1", fm_depth_3_1, 7),
    OSC_PSCALAR("/hds/fm/depth/3_2", fm_depth_3_2),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/3_2", fm_depth_3_2, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/3_2", fm_depth_3_2, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/3_2", fm_depth_3_2, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/3_2", fm_depth_3_2, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/3_2", fm_depth_3_2, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/3_2", fm_depth_3_2, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/3_2", fm_depth_3_2, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/3_2", fm_depth_3_2, 7),
    OSC_PSCALAR("/hds/fm/depth/4_1", fm_depth_4_1),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/4_1", fm_depth_4_1, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/4_1", fm_depth_4_1, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/4_1", fm_depth_4_1, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/4_1", fm_depth_4_1, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/4_1", fm_depth_4_1, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/4_1", fm_depth_4_1, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/4_1", fm_depth_4_1, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/4_1", fm_depth_4_1, 7),
    OSC_PSCALAR("/hds/fm/depth/4_3", fm_depth_4_3),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/4_3", fm_depth_4_3, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/4_3", fm_depth_4_3, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/4_3", fm_depth_4_3, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/4_3", fm_depth_4_3, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/4_3", fm_depth_4_3, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/4_3", fm_depth_4_3, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/4_3", fm_depth_4_3, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/4_3", fm_depth_4_3, 7),
    OSC_PSCALAR("/hds/fm/depth/1_2", fm_depth_1_2),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/1_2", fm_depth_1_2, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/1_2", fm_depth_1_2, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/1_2", fm_depth_1_2, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/1_2", fm_depth_1_2, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/1_2", fm_depth_1_2, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/1_2", fm_depth_1_2, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/1_2", fm_depth_1_2, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/1_2", fm_depth_1_2, 7),
    OSC_PSCALAR("/hds/fm/depth/3_4", fm_depth_3_4),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/3_4", fm_depth_3_4, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/3_4", fm_depth_3_4, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/3_4", fm_depth_3_4, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/3_4", fm_depth_3_4, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/3_4", fm_depth_3_4, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/3_4", fm_depth_3_4, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/3_4", fm_depth_3_4, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/3_4", fm_depth_3_4, 7),
    OSC_PSCALAR("/hds/fm/depth/1_3", fm_depth_1_3),
    OSC_PSCALAR_AT("/hds/combi/0/fm/depth/1_3", fm_depth_1_3, 0),
    OSC_PSCALAR_AT("/hds/combi/1/fm/depth/1_3", fm_depth_1_3, 1),
    OSC_PSCALAR_AT("/hds/combi/2/fm/depth/1_3", fm_depth_1_3, 2),
    OSC_PSCALAR_AT("/hds/combi/3/fm/depth/1_3", fm_depth_1_3, 3),
    OSC_PSCALAR_AT("/hds/combi/4/fm/depth/1_3", fm_depth_1_3, 4),
    OSC_PSCALAR_AT("/hds/combi/5/fm/depth/1_3", fm_depth_1_3, 5),
    OSC_PSCALAR_AT("/hds/combi/6/fm/depth/1_3", fm_depth_1_3, 6),
    OSC_PSCALAR_AT("/hds/combi/7/fm/depth/1_3", fm_depth_1_3, 7),

    OSC_PSCALAR("/hds/vco1/pitch", vco1_pitch),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/pitch", vco1_pitch, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/pitch", vco1_pitch, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/pitch", vco1_pitch, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/pitch", vco1_pitch, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/pitch", vco1_pitch, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/pitch", vco1_pitch, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/pitch", vco1_pitch, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/pitch", vco1_pitch, 7),
    OSC_PSCALAR("/hds/vco2/pitch", vco2_pitch),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/pitch", vco2_pitch, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/pitch", vco2_pitch, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/pitch", vco2_pitch, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/pitch", vco2_pitch, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/pitch", vco2_pitch, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/pitch", vco2_pitch, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/pitch", vco2_pitch, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/pitch", vco2_pitch, 7),
    OSC_PSCALAR("/hds/vco3/pitch", vco3_pitch),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/pitch", vco3_pitch, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/pitch", vco3_pitch, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/pitch", vco3_pitch, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/pitch", vco3_pitch, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/pitch", vco3_pitch, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/pitch", vco3_pitch, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/pitch", vco3_pitch, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/pitch", vco3_pitch, 7),
    OSC_PSCALAR("/hds/vco4/pitch", vco4_pitch),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/pitch", vco4_pitch, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/pitch", vco4_pitch, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/pitch", vco4_pitch, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/pitch", vco4_pitch, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/pitch", vco4_pitch, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/pitch", vco4_pitch, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/pitch", vco4_pitch, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/pitch", vco4_pitch, 7),

    // Monophonic Noise (Noise shape only -- see ProgramState.vco1_mono_noise's own comment).
    OSC_PSCALAR("/hds/vco1/mono_noise", vco1_mono_noise),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/mono_noise", vco1_mono_noise, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/mono_noise", vco1_mono_noise, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/mono_noise", vco1_mono_noise, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/mono_noise", vco1_mono_noise, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/mono_noise", vco1_mono_noise, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/mono_noise", vco1_mono_noise, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/mono_noise", vco1_mono_noise, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/mono_noise", vco1_mono_noise, 7),
    OSC_PSCALAR("/hds/vco2/mono_noise", vco2_mono_noise),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/mono_noise", vco2_mono_noise, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/mono_noise", vco2_mono_noise, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/mono_noise", vco2_mono_noise, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/mono_noise", vco2_mono_noise, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/mono_noise", vco2_mono_noise, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/mono_noise", vco2_mono_noise, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/mono_noise", vco2_mono_noise, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/mono_noise", vco2_mono_noise, 7),
    OSC_PSCALAR("/hds/vco3/mono_noise", vco3_mono_noise),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/mono_noise", vco3_mono_noise, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/mono_noise", vco3_mono_noise, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/mono_noise", vco3_mono_noise, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/mono_noise", vco3_mono_noise, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/mono_noise", vco3_mono_noise, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/mono_noise", vco3_mono_noise, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/mono_noise", vco3_mono_noise, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/mono_noise", vco3_mono_noise, 7),
    OSC_PSCALAR("/hds/vco4/mono_noise", vco4_mono_noise),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/mono_noise", vco4_mono_noise, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/mono_noise", vco4_mono_noise, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/mono_noise", vco4_mono_noise, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/mono_noise", vco4_mono_noise, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/mono_noise", vco4_mono_noise, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/mono_noise", vco4_mono_noise, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/mono_noise", vco4_mono_noise, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/mono_noise", vco4_mono_noise, 7),

    // --- Velocity -> Amp, per oscillator (see ProgramState.vco{1-4}_vel_to_amp's own comment) ---
    OSC_PSCALAR("/hds/vco1/vel_to_amp", vco1_vel_to_amp),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/vel_to_amp", vco1_vel_to_amp, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/vel_to_amp", vco1_vel_to_amp, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/vel_to_amp", vco1_vel_to_amp, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/vel_to_amp", vco1_vel_to_amp, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/vel_to_amp", vco1_vel_to_amp, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/vel_to_amp", vco1_vel_to_amp, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/vel_to_amp", vco1_vel_to_amp, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/vel_to_amp", vco1_vel_to_amp, 7),
    OSC_PSCALAR("/hds/vco2/vel_to_amp", vco2_vel_to_amp),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/vel_to_amp", vco2_vel_to_amp, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/vel_to_amp", vco2_vel_to_amp, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/vel_to_amp", vco2_vel_to_amp, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/vel_to_amp", vco2_vel_to_amp, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/vel_to_amp", vco2_vel_to_amp, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/vel_to_amp", vco2_vel_to_amp, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/vel_to_amp", vco2_vel_to_amp, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/vel_to_amp", vco2_vel_to_amp, 7),
    OSC_PSCALAR("/hds/vco3/vel_to_amp", vco3_vel_to_amp),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/vel_to_amp", vco3_vel_to_amp, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/vel_to_amp", vco3_vel_to_amp, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/vel_to_amp", vco3_vel_to_amp, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/vel_to_amp", vco3_vel_to_amp, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/vel_to_amp", vco3_vel_to_amp, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/vel_to_amp", vco3_vel_to_amp, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/vel_to_amp", vco3_vel_to_amp, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/vel_to_amp", vco3_vel_to_amp, 7),
    OSC_PSCALAR("/hds/vco4/vel_to_amp", vco4_vel_to_amp),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/vel_to_amp", vco4_vel_to_amp, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/vel_to_amp", vco4_vel_to_amp, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/vel_to_amp", vco4_vel_to_amp, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/vel_to_amp", vco4_vel_to_amp, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/vel_to_amp", vco4_vel_to_amp, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/vel_to_amp", vco4_vel_to_amp, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/vel_to_amp", vco4_vel_to_amp, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/vel_to_amp", vco4_vel_to_amp, 7),

    // --- Curve/Drive, per oscillator (see ProgramState.vco{1-4}_curve_drive's own comment) ---
    OSC_PSCALAR("/hds/vco1/curve_drive", vco1_curve_drive),
    OSC_PSCALAR_AT("/hds/combi/0/vco1/curve_drive", vco1_curve_drive, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco1/curve_drive", vco1_curve_drive, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco1/curve_drive", vco1_curve_drive, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco1/curve_drive", vco1_curve_drive, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco1/curve_drive", vco1_curve_drive, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco1/curve_drive", vco1_curve_drive, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco1/curve_drive", vco1_curve_drive, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco1/curve_drive", vco1_curve_drive, 7),
    OSC_PSCALAR("/hds/vco2/curve_drive", vco2_curve_drive),
    OSC_PSCALAR_AT("/hds/combi/0/vco2/curve_drive", vco2_curve_drive, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco2/curve_drive", vco2_curve_drive, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco2/curve_drive", vco2_curve_drive, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco2/curve_drive", vco2_curve_drive, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco2/curve_drive", vco2_curve_drive, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco2/curve_drive", vco2_curve_drive, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco2/curve_drive", vco2_curve_drive, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco2/curve_drive", vco2_curve_drive, 7),
    OSC_PSCALAR("/hds/vco3/curve_drive", vco3_curve_drive),
    OSC_PSCALAR_AT("/hds/combi/0/vco3/curve_drive", vco3_curve_drive, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco3/curve_drive", vco3_curve_drive, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco3/curve_drive", vco3_curve_drive, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco3/curve_drive", vco3_curve_drive, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco3/curve_drive", vco3_curve_drive, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco3/curve_drive", vco3_curve_drive, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco3/curve_drive", vco3_curve_drive, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco3/curve_drive", vco3_curve_drive, 7),
    OSC_PSCALAR("/hds/vco4/curve_drive", vco4_curve_drive),
    OSC_PSCALAR_AT("/hds/combi/0/vco4/curve_drive", vco4_curve_drive, 0),
    OSC_PSCALAR_AT("/hds/combi/1/vco4/curve_drive", vco4_curve_drive, 1),
    OSC_PSCALAR_AT("/hds/combi/2/vco4/curve_drive", vco4_curve_drive, 2),
    OSC_PSCALAR_AT("/hds/combi/3/vco4/curve_drive", vco4_curve_drive, 3),
    OSC_PSCALAR_AT("/hds/combi/4/vco4/curve_drive", vco4_curve_drive, 4),
    OSC_PSCALAR_AT("/hds/combi/5/vco4/curve_drive", vco4_curve_drive, 5),
    OSC_PSCALAR_AT("/hds/combi/6/vco4/curve_drive", vco4_curve_drive, 6),
    OSC_PSCALAR_AT("/hds/combi/7/vco4/curve_drive", vco4_curve_drive, 7),

    // --- Curve/Drive, per LFO (see PackLfoParams.curve_drive's own comment) ---
    OSC_PSUBARRAY("/hds/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT),
    OSC_PSUBARRAY_AT("/hds/combi/0/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 0),
    OSC_PSUBARRAY_AT("/hds/combi/1/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 1),
    OSC_PSUBARRAY_AT("/hds/combi/2/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 2),
    OSC_PSUBARRAY_AT("/hds/combi/3/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 3),
    OSC_PSUBARRAY_AT("/hds/combi/4/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 4),
    OSC_PSUBARRAY_AT("/hds/combi/5/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 5),
    OSC_PSUBARRAY_AT("/hds/combi/6/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 6),
    OSC_PSUBARRAY_AT("/hds/combi/7/lfo/%d/curve_drive", lfo, PackLfoParams, curve_drive, LFO_COUNT, 7),

    // --- Combi/Sequencer scaffolding (see SEQ_LANE_COUNT's own comment) ---
    OSC_SCALAR("/hds/performance_mode", performance_mode),
    OSC_SUBARRAY("/hds/seq/%d/lane_type", seq_lanes, SeqLaneState, lane_type, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/volume", seq_lanes, SeqLaneState, volume, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/pan", seq_lanes, SeqLaneState, pan, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/midi_channel", seq_lanes, SeqLaneState, midi_channel, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/mode", seq_lanes, SeqLaneState, seq_mode, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/mute", seq_lanes, SeqLaneState, mute, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/rec_armed", seq_lanes, SeqLaneState, rec_armed, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/rec_overdub", seq_lanes, SeqLaneState, rec_overdub, SEQ_LANE_COUNT),
    OSC_STRING_SUBARRAY("/hds/seq/%d/audio_path", seq_lanes, SeqLaneState, audio_path, SEQ_LANE_COUNT, SEQ_AUDIO_PATH_LEN),
    OSC_SUBARRAY("/hds/seq/%d/audio_offset_sec", seq_lanes, SeqLaneState, audio_offset_sec, SEQ_LANE_COUNT),
    OSC_SUBARRAY("/hds/seq/%d/audio_preview_trigger", seq_lanes, SeqLaneState, audio_preview_trigger, SEQ_LANE_COUNT),
    OSC_SCALAR("/hds/seq/transport/play", seq_transport_play_trigger),
    OSC_SCALAR("/hds/seq/transport/pause", seq_transport_pause_trigger),
    OSC_SCALAR("/hds/seq/transport/stop", seq_transport_stop_trigger),
    OSC_SCALAR("/hds/seq/transport/seek_start", seq_transport_seek_start_trigger),
    OSC_SCALAR("/hds/seq/transport/seek_back", seq_transport_seek_back_trigger),
    OSC_SCALAR("/hds/seq/transport/seek_fwd", seq_transport_seek_fwd_trigger),
    OSC_SCALAR("/hds/seq/transport/seek_to_target_sec", seq_transport_seek_to_target_sec),
    OSC_SCALAR("/hds/seq/transport/seek_to", seq_transport_seek_to_trigger),
    OSC_SCALAR("/hds/seq/transport/metronome", metronome_enable),
    OSC_SCALAR("/hds/seq/quantize/enable", seq_quantize_enable),
    OSC_SCALAR("/hds/seq/quantize/resolution", seq_quantize_resolution),
    OSC_SCALAR("/hds/seq/sync_source", seq_sync_source),

    OSC_ARRAY("/hds/combi/%d/midi_channel", combi_lane_midi_channel, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/key_lo", combi_lane_key_lo, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/key_hi", combi_lane_key_hi, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/vel_lo", combi_lane_vel_lo, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/vel_hi", combi_lane_vel_hi, COMBI_PROGRAM_COUNT),
    OSC_ARRAY("/hds/combi/%d/transpose", combi_lane_transpose, COMBI_PROGRAM_COUNT),

    OSC_STRING("/hds/device/output", output_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/device/input", input_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/device/midi", midi_device, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/instance_name", instance_name, AUDIO_DEVICE_NAME_LEN),
    OSC_STRING("/hds/record/path", record_path, RECORD_PATH_LEN),
};
#define OSC_PARAMS_COUNT (sizeof(OSC_PARAMS) / sizeof(OSC_PARAMS[0]))

OscDispatchSlot osc_dispatch_table[OSC_DISPATCH_TABLE_SIZE];

// FNV-1a: fast, simple, and more than good enough for a fixed, known-at-startup key set with no
// adversarial-input concern (OSC addresses are our own compile-time-generated strings).
static uint32_t osc_hash_address(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static void osc_dispatch_insert(const char *address, size_t offset, OscParamKind kind, size_t str_len) {
    uint32_t h = osc_hash_address(address) & (OSC_DISPATCH_TABLE_SIZE - 1);
    for (int probes = 0; probes < OSC_DISPATCH_TABLE_SIZE; probes++) {
        OscDispatchSlot *slot = &osc_dispatch_table[h];
        if (!slot->occupied) {
            snprintf(slot->address, sizeof(slot->address), "%s", address);
            slot->offset = offset; slot->kind = kind; slot->str_len = str_len; slot->occupied = true;
            return;
        }
        // Every concrete address OSC_PARAMS expands to must be unique (the whole point of the
        // table) -- a collision on the STRING itself (not just the hash) means two OSC_PARAMS
        // rows generate the same wire address, an authoring bug in the table that must be caught
        // at startup, never silently overwritten.
        if (strcmp(slot->address, address) == 0) {
            fprintf(stderr, "[osc] FATAL: duplicate OSC_PARAMS address '%s' -- two table rows generate the same concrete address\n", address);
            exit(1);
        }
        h = (h + 1) & (OSC_DISPATCH_TABLE_SIZE - 1);
    }
    fprintf(stderr, "[osc] FATAL: dispatch table full inserting '%s' -- raise OSC_DISPATCH_TABLE_SIZE\n", address);
    exit(1);
}

// Mirrors dump_osc_params_addresses.c's own expansion loop exactly (same nested-index shape),
// since that's the same enumeration this table's correctness depends on matching.
void build_osc_dispatch_table(void) {
    for (size_t i = 0; i < OSC_PARAMS_COUNT; i++) {
        const OscParamEntry *e = &OSC_PARAMS[i];
        int n1 = (e->num_indices >= 1) ? e->count : 1;
        int n2 = (e->num_indices == 2) ? e->count2 : 1;
        for (int idx1 = 0; idx1 < n1; idx1++) {
            for (int idx2 = 0; idx2 < n2; idx2++) {
                char addr[128];
                size_t offset = e->base_offset;
                if (e->num_indices == 0) {
                    snprintf(addr, sizeof(addr), "%s", e->address);
                } else if (e->num_indices == 1) {
                    snprintf(addr, sizeof(addr), e->address, idx1);
                    offset += (size_t)idx1 * e->stride;
                } else {
                    snprintf(addr, sizeof(addr), e->address, idx1, idx2);
                    offset += (size_t)idx1 * e->stride + (size_t)idx2 * e->stride2;
                }
                osc_dispatch_insert(addr, offset, e->kind, e->str_len);
            }
        }
    }
}

const OscDispatchSlot *osc_dispatch_lookup(const char *address) {
    uint32_t h = osc_hash_address(address) & (OSC_DISPATCH_TABLE_SIZE - 1);
    for (int probes = 0; probes < OSC_DISPATCH_TABLE_SIZE; probes++) {
        const OscDispatchSlot *slot = &osc_dispatch_table[h];
        if (!slot->occupied) return NULL;
        if (strcmp(slot->address, address) == 0) return slot;
        h = (h + 1) & (OSC_DISPATCH_TABLE_SIZE - 1);
    }
    return NULL;
}

// hds.clap's own native Combi-lane Program picker (hds_apply_program_fields_to_combi_lane()/
// hds_load_program_file_to_combi_lane()) lives in hds_clap_combi_draw.c, not here -- unlike the
// JSON reader/hds_scan_programs_folder() just above (genuinely shared with hds_standalone.c's own
// --program flag / arrow-key cycling), loading a saved .hdsprog onto a Combi lane is a CLAP-only
// concept (hds_standalone.c has no Combi lanes of its own to load one onto), so it stays out of
// this shared file entirely -- keeps hds_standalone.c's own build free of an
// always-unused-there function, and keeps this file's own scope to what both TUs actually need.

// The single generic OSC method (registered with path=NULL, type=NULL -- liblo calls this for
// every incoming message that no more specific method claims). O(1) average-case hash lookup via
// osc_dispatch_table (see above) -- an out-of-range index (e.g. "/hds/lfo/99/rate") simply was
// never inserted into the table, so it correctly falls through to "unrecognized address" below
// rather than needing its own explicit bounds check.
int generic_osc_handler(const char *path, const char *types, lo_arg **argv, int argc,
                                lo_message msg, void *user_data) {
    (void)msg;
    SynthSharedControls *knobs = (SynthSharedControls *)user_data;
    const OscDispatchSlot *slot = osc_dispatch_lookup(path);
    if (!slot) {
        fprintf(stderr, "[osc] unrecognized address '%s' (types='%s') ignored\n", path, types);
        return 0;
    }
    if (slot->kind == OSC_KIND_FLOAT) {
        if (argc != 1 || types[0] != 'f') { fprintf(stderr, "[osc] '%s' expects one float arg, got types='%s'\n", path, types); return 0; }
        *(float *)((char *)knobs + slot->offset) = argv[0]->f;
    } else { // OSC_KIND_STRING
        if (argc != 1 || types[0] != 's') { fprintf(stderr, "[osc] '%s' expects one string arg, got types='%s'\n", path, types); return 0; }
        snprintf((char *)knobs + slot->offset, slot->str_len, "%s", (const char *)&argv[0]->s);
    }
    return 0;
}

void osc_error_handler(int num, const char *msg, const char *path) {
    fprintf(stderr, "[osc] liblo error %d (%s): %s\n", num, path ? path : "?", msg);
}

// --- "/hds/request_state": lets a controller that just connected pull the CURRENT live state
// instead of blindly pushing its own GUI defaults (which used to silently overwrite, say, a DAW-
// restored CLAP patch the instant a naive boot-time broadcast of every knob's own default landed --
// see the fix that added this whole request/reply pair). Reuses osc_dispatch_table
// (the exact same address->offset map generic_osc_handler() itself reads), so this can never
// name-drift from what the live knob addresses actually mean. ---

// osc_dispatch_table holds ~4100 concrete addresses -- sent as one giant bundle that would
// exceed UDP's ~65507-byte max datagram payload (measured well over 100KB for the full set,
// vs. transmit_knob_packet()'s own much smaller ~40KB bundle, which only covers the ~1150
// addresses this frontend build actually has live widgets for). Chunked into many small bundles
// instead -- the same "many small sends, not one unbounded one" shape the Sample Bank zone
// catalog protocol already uses for its own unbounded-size data.
#define HDS_STATE_DUMP_CHUNK_SIZE 150

// Sends every currently-occupied osc_dispatch_table address's live value to reply_addr, then one
// terminating sentinel message so the receiver knows the dump is complete (rather than guessing
// via a timeout) -- see README.md's OSC protocol section for the full request/reply shape.
static void send_full_state_dump(lo_address reply_addr, const SynthSharedControls *knobs) {
    if (!reply_addr) return;
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    int in_bundle = 0;
    for (int i = 0; i < OSC_DISPATCH_TABLE_SIZE; i++) {
        const OscDispatchSlot *slot = &osc_dispatch_table[i];
        if (!slot->occupied) continue;
        lo_message m = lo_message_new();
        if (slot->kind == OSC_KIND_FLOAT) {
            lo_message_add_float(m, *(const float *)((const char *)knobs + slot->offset));
        } else {
            lo_message_add_string(m, (const char *)knobs + slot->offset);
        }
        lo_bundle_add_message(b, slot->address, m);
        if (++in_bundle >= HDS_STATE_DUMP_CHUNK_SIZE) {
            lo_send_bundle(reply_addr, b);
            lo_bundle_free_recursive(b);
            b = lo_bundle_new(LO_TT_IMMEDIATE);
            in_bundle = 0;
        }
    }
    if (in_bundle > 0) lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b);
    lo_send(reply_addr, "/hds/status/state_dump_complete", "");
}

int request_state_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argv; (void)argc;
    HdsStateRequestContext *ctx = (HdsStateRequestContext *)user_data;
    // Reply to whoever actually sent this request (lo_message_get_source), not ctx->reply_addr's
    // own fixed, startup-configured target -- this is what lets a controller pull an instance's
    // full state regardless of whether it's the one --osc-reply-host/--osc-reply-port (or CLAP's
    // own paired port) originally pointed at, e.g. after scanning for and connecting to a
    // DIFFERENT already-running instance than the one it started out pointed at (see ping_handler's
    // own comment below). The source address liblo hands back here is exactly the socket the
    // request arrived FROM, so a controller MUST send this one message from its own actual
    // listening socket, not an unrelated ephemeral send-only port nothing is listening on.
    lo_address source = lo_message_get_source(msg);
    send_full_state_dump(source, ctx->knobs);
    return 0;
}

// --- Multi-instance discovery: "/hds/ping" (discovery) and "/hds/subscribe" (redirect this
// instance's own ongoing status pushes) -- for a controller managing more than one running
// instance (e.g. one per track) to find and address each by its own instance_name. Both are
// registered per-instance (not via the generic catch-all), same reasoning as request_state above.

// A controller scanning for reachable instances blasts this at a candidate host:port list and
// collects whoever answers within a short timeout -- "replied just now" IS the liveness proof,
// no separate registry/heartbeat bookkeeping needed. Reply-to-sender (see request_state_handler's
// own comment) -- a controller MUST send this from its own actual listening socket so the reply
// lands back at its real listening port, not an unrelated scan-only socket.
int ping_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argv; (void)argc;
    const SynthSharedControls *knobs = (const SynthSharedControls *)user_data;
    lo_address source = lo_message_get_source(msg);
    if (!source) return 0;
    lo_send(source, "/hds/pong", "s", knobs->instance_name);
    return 0;
}

int subscribe_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types; (void)argv; (void)argc;
    HdsSubscribeContext *ctx = (HdsSubscribeContext *)user_data;
    lo_address source = lo_message_get_source(msg);
    if (!source) return 0;
    const char *host = lo_address_get_hostname(source);
    const char *port = lo_address_get_port(source);
    if (!host || !port) return 0;
    lo_address new_target = lo_address_new(host, port);
    if (!new_target) return 0;
    if (*ctx->target) lo_address_free(*ctx->target);
    *ctx->target = new_target;
    return 0;
}

// Sends the GUI-status snapshot as one OSC bundle (one UDP datagram, 8 messages) to the reply
// address configured at startup. Fire-and-forget, same as the rest of this UDP-based protocol:
// if nothing is listening on the reply port, lo_send_bundle simply has nowhere to deliver to --
// a no-op, not an error, same as the old stream-socket's behavior whenever client_fd wasn't
// connected.
void send_osc_status(lo_address reply_addr, const EngineStatus *status) {
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    lo_message m;
    m = lo_message_new(); lo_message_add_float(m, status->peak); lo_bundle_add_message(b, "/hds/status/peak", m);
    m = lo_message_new(); lo_message_add_float(m, status->is_recording); lo_bundle_add_message(b, "/hds/status/is_recording", m);
    m = lo_message_new(); lo_message_add_float(m, status->elapsed_seconds); lo_bundle_add_message(b, "/hds/status/elapsed_seconds", m);
    m = lo_message_new(); lo_message_add_float(m, status->record_error); lo_bundle_add_message(b, "/hds/status/record_error", m);
    m = lo_message_new(); lo_message_add_float(m, status->detected_input_channels); lo_bundle_add_message(b, "/hds/status/detected_input_channels", m);
    m = lo_message_new(); lo_message_add_float(m, status->detected_output_channels); lo_bundle_add_message(b, "/hds/status/detected_output_channels", m);
    m = lo_message_new(); lo_message_add_float(m, status->tempo_bpm_live); lo_bundle_add_message(b, "/hds/status/tempo_bpm_live", m);
    m = lo_message_new(); lo_message_add_float(m, status->seq_elapsed_seconds); lo_bundle_add_message(b, "/hds/status/seq_elapsed_seconds", m);
    m = lo_message_new(); lo_message_add_float(m, status->seq_transport_state); lo_bundle_add_message(b, "/hds/status/seq_transport_state", m);
    for (int p = 0; p < EXT_IN_PAIR_COUNT; p++) {
        char addr[40];
        snprintf(addr, sizeof(addr), "/hds/status/ext_in_peak/%d", p);
        m = lo_message_new(); lo_message_add_float(m, status->ext_in_peak[p]); lo_bundle_add_message(b, addr, m);
    }
    m = lo_message_new(); lo_message_add_float(m, status->midi_clock_locked); lo_bundle_add_message(b, "/hds/status/midi_clock_locked", m);
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        char addr[40];
        snprintf(addr, sizeof(addr), "/hds/status/fx_gr/%d", s);
        m = lo_message_new(); lo_message_add_float(m, status->fx_gain_reduction_db[s]); lo_bundle_add_message(b, addr, m);
    }
    for (int s = 0; s < FX_SLOT_COUNT; s++) {
        char addr[48];
        snprintf(addr, sizeof(addr), "/hds/status/fx_dump_avail/%d", s);
        m = lo_message_new(); lo_message_add_float(m, status->fx_dump_avail_sec[s]); lo_bundle_add_message(b, addr, m);
    }
    lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b); // frees the bundle AND every message added to it
}

// Per-lane "Load Audio" feedback (see g_seq_audio/poll_seq_audio_load_requests). Separate from
// send_osc_status()'s fixed-shape bundle above since this is a variable-length, mostly-empty
// set -- only lanes with a non-idle load_state (at most SEQ_LANE_COUNT, in practice usually 0)
// get a message, so an unused Sequencer session costs nothing extra on this reply port. Sent at
// the same vu_decimator cadence as send_osc_status(), from the same call site.
void send_seq_audio_status(HdsStreamsState *streams, lo_address reply_addr) {
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    bool any = false;
    for (int l = 0; l < SEQ_LANE_COUNT; l++) {
        int state = atomic_load_explicit(&streams->seq_audio[l].load_state, memory_order_acquire);
        if (state == SEQ_AUDIO_EMPTY) continue;
        any = true;
        char addr[64];
        snprintf(addr, sizeof(addr), "/hds/status/seq_audio/%d/state", l);
        lo_message m = lo_message_new(); lo_message_add_float(m, (float)state); lo_bundle_add_message(b, addr, m);
        if (state == SEQ_AUDIO_READY) {
            snprintf(addr, sizeof(addr), "/hds/status/seq_audio/%d/frames", l);
            m = lo_message_new(); lo_message_add_float(m, (float)streams->seq_audio[l].frame_count); lo_bundle_add_message(b, addr, m);
            snprintf(addr, sizeof(addr), "/hds/status/seq_audio/%d/channels", l);
            m = lo_message_new(); lo_message_add_float(m, (float)streams->seq_audio[l].channels); lo_bundle_add_message(b, addr, m);
            snprintf(addr, sizeof(addr), "/hds/status/seq_audio/%d/sample_rate", l);
            m = lo_message_new(); lo_message_add_float(m, (float)streams->seq_audio[l].sample_rate); lo_bundle_add_message(b, addr, m);
        }
    }
    if (any) lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b);
}
