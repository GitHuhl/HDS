// sfz_parser.h -- the native SFZ parser's interface (sfz_parser.c), used by backend.c's
// hds_load_sfz_for_osc() to turn a parsed file's regions into Sample Bank zones.
#ifndef HDS_SFZ_PARSER_H
#define HDS_SFZ_PARSER_H

#include "core/backend.h"

// One <region>'s recognized opcodes (SFZ inheritance: a region starts as a copy of its enclosing
// <group>/<global> and each opcode line overwrites its own field, last-value-wins). A bounded
// struct-of-optional-fields instead of
// a generic string map, since HDS's supported opcode set is fixed and small. Text-valued fields
// are used wherever the raw string needs deferred numeric interpretation (lokey/hikey/
// pitch_keycenter as note-name-or-int; loop_start/loop_end/offset/end as frame counts, which need
// total_frames -- only known once sample= resolves -- before they can become a 0-1 fraction).
#define SFZ_TEXT_FIELD_LEN 32
// SFZ_MAX_VELCURVE_POINTS itself now lives up near SampleSlot's own velcurve_point[]/velcurve_gain[]
// fields (which need it too, and are declared well before this point in the file) -- see that
// #define's own comment, unchanged.
typedef struct {
    bool has_sample;              char sample[SAMPLE_PATH_LEN];
    bool has_key;                  char key[SFZ_TEXT_FIELD_LEN];
    bool has_lokey;                char lokey[SFZ_TEXT_FIELD_LEN];
    bool has_hikey;                char hikey[SFZ_TEXT_FIELD_LEN];
    bool has_pitch_keycenter;      char pitch_keycenter[SFZ_TEXT_FIELD_LEN];
    bool has_pitch_keytrack;       float pitch_keytrack;
    bool has_lovel;                int lovel;
    bool has_hivel;                int hivel;
    bool has_loop_mode;            char loop_mode[24];
    bool has_loop_start;           char loop_start[SFZ_TEXT_FIELD_LEN];
    bool has_loop_end;             char loop_end[SFZ_TEXT_FIELD_LEN];
    bool has_loop_crossfade;       float loop_crossfade; // seconds
    bool has_offset;               char offset[SFZ_TEXT_FIELD_LEN];
    bool has_end;                  char end[SFZ_TEXT_FIELD_LEN];
    bool has_tune;                 float tune;
    bool has_pitch;                float pitch;
    bool has_volume;               float volume;
    bool has_pan;                  float pan;
    bool has_direction;            char direction[16];
    // group=/off_by=: real SFZ polyphony/choke-group semantics (e.g. a closed hi-hat region's
    // off_by= matching an open hi-hat region's own group= chokes the open hat the instant the
    // closed one triggers) -- see apply_choke_group()'s own comment. Deliberately NOT the same
    // thing round-robin cycling uses any more (see seq_length/seq_position below and
    // hds_load_sfz_for_osc's own comment on why bucketing by group= was never spec-correct in the
    // first place). Real SFZ group ids are always plain non-negative integers.
    bool has_group;                int group;
    bool has_off_by;               int off_by;
    bool has_seq_length;           int seq_length;
    // seq_position (1-based): explicit round-robin ordinal. Without this, cycling order can only
    // ever be "the order regions happen to appear in the file" -- usually the same thing, but not
    // guaranteed, and not what the opcode is actually FOR. Defaults to 0 (meaning "unspecified,
    // fall back to file-encounter order") -- see find_matching_sample_slot's own comment.
    bool has_seq_position;         int seq_position;
    // lorand/hirand: an alternative (or complementary -- both can be set on the same region) to
    // round-robin, matching only when a per-note-on random draw in [0,1) falls inside [lorand,
    // hirand). Defaults to the full [0,1) range when unset, i.e. "always matches on this axis."
    bool has_lorand;               float lorand;
    bool has_hirand;               float hirand;
    // trigger=: "attack" (default, unset) is the ordinary note-on match every existing zone
    // already gets; "release"/"release_key" fire on note-OFF instead (see
    // fire_release_trigger_zones()'s own comment); "first"/"legato" restrict an ordinary note-on
    // match to only the first key of a phrase / only an overlapping legato note respectively (the
    // same distinction Mono Legato already tracks per-Program -- see
    // hds_sfz_trigger_matches_note_context()).
    bool has_trigger;              char trigger[16];
    // sw_lokey/sw_hikey/sw_last/sw_default: keyswitching -- see HdsEngineState.keyswitch_note's
    // own comment and hds_sfz_trigger_matches_keyswitch(). A key inside [sw_lokey,sw_hikey] is a
    // keyswitch trigger for this Program's own oscillator, not an ordinary playable note: pressing
    // it updates keyswitch_note and produces no sound of its own (matches every real keyswitch
    // library's own convention). Regions that specify sw_lokey/sw_hikey (on ANY region in the
    // file, control-block-wide in practice, but tracked per-region here rather than adding a THIRD
    // accumulator scope, since <global>/<group> inheritance already gets it into ordinary regions
    // as if it were any other opcode) become eligible for an ordinary note only while the last
    // keyswitch pressed falls inside the region's OWN declared keyswitch range from sw_default
    // (used before ANY keyswitch has ever been pressed) or sw_last (a legacy-but-common single-
    // value alternative to the lokey/hikey pair, meaning "matches only this exact keyswitch note").
    bool has_sw_lokey;             char sw_lokey[SFZ_TEXT_FIELD_LEN];
    bool has_sw_hikey;             char sw_hikey[SFZ_TEXT_FIELD_LEN];
    bool has_sw_last;              char sw_last[SFZ_TEXT_FIELD_LEN];
    bool has_sw_default;           char sw_default[SFZ_TEXT_FIELD_LEN];
    // xfin_lovel/xfin_hivel, xfout_lovel/xfout_hivel (+ the xfin_lokey/xfin_hikey/xfout_lokey/
    // xfout_hikey key-range equivalents): velocity/key-range CROSSFADE layering -- see
    // render_sample_osc's own crossfade-layer mixing comment and SampleSlot.xfade_*'s own fields.
    // Unlike a hard lovel/hivel boundary (one zone or the other, never both), these let two (or
    // more) regions BOTH sound simultaneously across a shared boundary range, mixed by a linear
    // gain ramp -- e.g. a soft velocity layer fading out exactly as a hard layer fades in, instead
    // of an audible hard switch at one fixed velocity.
    bool has_xfin_lovel;           int xfin_lovel;
    bool has_xfin_hivel;           int xfin_hivel;
    bool has_xfout_lovel;          int xfout_lovel;
    bool has_xfout_hivel;          int xfout_hivel;
    bool has_xfin_lokey;           char xfin_lokey[SFZ_TEXT_FIELD_LEN];
    bool has_xfin_hikey;           char xfin_hikey[SFZ_TEXT_FIELD_LEN];
    bool has_xfout_lokey;          char xfout_lokey[SFZ_TEXT_FIELD_LEN];
    bool has_xfout_hikey;          char xfout_hikey[SFZ_TEXT_FIELD_LEN];
    // rt_decay: extra release-stage decay (dB/second) applied only to a trigger=release zone,
    // scaled by how long the original note was actually held -- see fire_release_trigger_zones()'s
    // own comment. Meaningless on an ordinary attack-triggered zone.
    bool has_rt_decay;             float rt_decay;
    // delay: seconds to wait after note-on before this region's own playback actually starts
    // (silence, not a slow attack) -- see start_sample_osc's own delay-arming comment.
    bool has_delay;                float delay;
    // ampeg_*: per-zone amp envelope -- strictly EITHER this zone's own SFZ-defined envelope OR
    // the Program's own Play-tab Amp EG for this oscillator, never a blend of the two (see
    // ProgramState.vco1_force_own_amp_eg's own comment for the one explicit switch between them).
    // When this zone's own envelope is the one in effect, any ampeg_* stage it does NOT set uses
    // SFZ's own standard default for that stage (0 for delay/attack/hold/decay/start, 100 for
    // sustain, an effectively-instant release) -- never the Program's own per-stage knob value.
    // ampeg_start is SFZ's name for the envelope's own starting level (0-100), the same "Attack
    // Start Level" concept PackEGParams.al already has.
    bool has_ampeg_delay;          float ampeg_delay;   // seconds
    bool has_ampeg_attack;         float ampeg_attack;  // seconds
    bool has_ampeg_hold;           float ampeg_hold;    // seconds
    bool has_ampeg_decay;          float ampeg_decay;   // seconds
    bool has_ampeg_sustain;        float ampeg_sustain; // 0-100
    bool has_ampeg_release;        float ampeg_release; // seconds
    bool has_ampeg_start;          float ampeg_start;   // 0-100
    // note_polyphony: caps how many voices may simultaneously hold a claimed stream against THIS
    // one zone (by zone_id) -- see start_sample_osc's own note_polyphony gate. 0/unset = no cap
    // (today's own unlimited behavior).
    bool has_note_polyphony;       int note_polyphony;
    // amp_velcurve_<N>=<value>: a custom, non-linear velocity-to-gain response curve, as explicit
    // (velocity 0-127, gain 0-1) breakpoints -- piecewise-linearly interpolated between them at
    // note-on (see sfz_velcurve_gain()). velcurve_point/velcurve_gain run in parallel, always kept
    // sorted by ascending point as opcodes are applied (sfz_apply_opcode's own insertion-sort) so
    // sfz_velcurve_gain() never needs to sort at note-on time.
    int velcurve_count;
    int velcurve_point[SFZ_MAX_VELCURVE_POINTS];
    float velcurve_gain[SFZ_MAX_VELCURVE_POINTS];
    // hds_drum_bus=<1..DRUM_BUS_COUNT>: HDS-specific, non-standard opcode (like direction=reverse
    // above) -- lets a drum-kit SFZ declare each key's own Drum Bus routing directly in the file,
    // instead of needing a separate live OSC message per zone after loading. See SampleSlot.
    // drum_bus's own comment for what this actually does at render time.
    bool has_drum_bus;             int drum_bus;
} SfzRegion;

#define SFZ_MAX_REGIONS_PER_FILE 65536
// File-scope static scratch buffer (used by hds_load_sfz_for_osc(), further down, as the
// out_regions argument to sfz_parse_file()) rather than a stack array or per-call malloc -- reused
// across calls, not reentrant/thread-safe, which is correct because every caller (the CLI
// --sfz-vcoN flags at startup, and /hds/zone/load_sfz's handler) runs on the same single thread
// that already owns g_sample_slot_state/g_pending_load_state (main()'s own OSC-poll/audio
// thread) -- same "audio-thread-only, no cross-thread sync needed" posture those already have.

// Reused output buffer for sfz_parse_file() (too large for the stack; only the OSC/control thread parses).
extern SfzRegion g_sfz_parse_scratch[SFZ_MAX_REGIONS_PER_FILE];

// Parses `path` (following #include/#define) into out_regions; returns the region count (0 on failure).
// out_default_path receives the file's <control> default_path, resolved, for relative sample= values.
int sfz_parse_file(const char *path, SfzRegion *out_regions, int max_regions, char *out_default_path, size_t out_default_path_len);
// Note name ("c#4", "60") -> MIDI note number, or -1 if unparseable.
int sfz_note_to_number(const char *value);
// "frame" offset string -> fraction of total_frames (false if invalid).
bool sfz_frame_to_frac(const char *frame_str, unsigned int total_frames, float *out_frac);

#endif // HDS_SFZ_PARSER_H
