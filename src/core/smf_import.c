// smf_import.c -- Standard MIDI File (.mid) import into the Sequencer's MIDI lanes and tempo map, plus its two OSC handlers.
// Moved verbatim out of backend.c.
#include "core/backend.h"

// --- Standard MIDI File (.mid) import: a REAL SMF Type 0/1 reader (e.g. for a file exported from
// a DAW like Reaper) -- distinct from hds_song_save_midi/load_midi (moved to hds_standalone.c,
// standalone-only), which is this engine's OWN small internal round-trip format, not a real SMF
// parser. Populates Sequencer
// lanes from each note-carrying track, and always reports the file's own text markers (0xFF 0x06)
// back to the caller for the Sequencer tab's Markers list ("jump to marker") regardless of
// whether the caller also asked to import the file's tempo map.
//
// Ticks are shared across every track in one SMF file (each track advances the same underlying
// clock via its own independent delta-times), and Set Tempo (0xFF 0x51)/Marker (0xFF 0x06)
// meta-events conventionally live on a Type 1 file's own Track 0 -- but this scans EVERY track
// for them rather than assuming that, so a Type 0 file (tempo, markers, and notes all
// interleaved on one single track) is handled by the exact same code path with no special case.
#define HDS_SMF_MARKER_MAX 64
#define HDS_SMF_MARKER_LABEL_LEN 64
#define HDS_SMF_TEMPO_EVENT_MAX 256
// Generous ceiling on a plausible plain MIDI file -- rejects anything wildly oversized outright
// rather than fread()ing an arbitrarily large file into memory (a real SMF export, even a long,
// busy multi-track song, is almost always well under 1MB).
#define HDS_SMF_FILE_SIZE_MAX (16 * 1024 * 1024)

typedef struct { long tick; uint32_t usec_per_qn; } HdsSmfTempoEvent;
typedef struct { long tick; char label[HDS_SMF_MARKER_LABEL_LEN]; } HdsSmfMarkerEvent;
typedef struct { long tick; unsigned char type; unsigned char note; unsigned char velocity; } HdsSmfNoteEvent; // type: 0 = note_on, 1 = note_off -- pre-conversion twin of SeqMidiEvent, tick instead of time_sec

typedef struct {
    bool ok;
    char error[128]; // empty on success
    int lanes_used;
    bool tempo_applied;
    int marker_count;
    float marker_time[HDS_SMF_MARKER_MAX];
    char marker_label[HDS_SMF_MARKER_MAX][HDS_SMF_MARKER_LABEL_LEN];
} HdsSmfImportResult;

static bool hds_smf_read_u16be(const uint8_t *buf, size_t len, size_t *pos, uint16_t *out) {
    if (*pos + 2 > len) return false;
    *out = (uint16_t)(((uint16_t)buf[*pos] << 8) | buf[*pos + 1]);
    *pos += 2;
    return true;
}
static bool hds_smf_read_u32be(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out) {
    if (*pos + 4 > len) return false;
    *out = ((uint32_t)buf[*pos] << 24) | ((uint32_t)buf[*pos + 1] << 16) | ((uint32_t)buf[*pos + 2] << 8) | (uint32_t)buf[*pos + 3];
    *pos += 4;
    return true;
}
// A Variable-Length Quantity: 7 payload bits per byte, MSB set on every byte but the last. Used
// for both delta-times and meta-event lengths. Real SMF values never need more than 4 continuation
// bytes (28 bits) -- more than that is corrupt input, not a value this format can express.
static bool hds_smf_read_vlq(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= len) return false;
        uint8_t b = buf[*pos]; (*pos)++;
        value = (value << 7) | (uint32_t)(b & 0x7F);
        if (!(b & 0x80)) { *out = value; return true; }
    }
    return false;
}

// Parses one MTrk chunk's raw event bytes (delta-time + event, repeated until the chunk ends).
// Returns false only for genuine STRUCTURAL corruption (a truncated VLQ, or an event that runs
// off the end of the chunk) -- "no partial application" all the way up to hds_song_import_smf's
// own caller, same posture as hds_song_load_midi (standalone-only, in hds_standalone.c) and
// hds_tuning_load_scl (this file, further up -- shared by both hosts). A recognized-but-full
// capacity (more tempo/marker/note events than this
// engine reserves room for) is NOT such a failure -- it's silently capped, with a diagnostic,
// exactly like every other fixed-capacity import here (see hds_tuning_load_scl's HDS_TUNING_MAX_DEGREES, or
// seq_record_midi_event's own SEQ_MIDI_EVENT_CAPACITY cap). note_cap == 0 means "don't collect
// notes for this track at all" (this file already has SEQ_LANE_COUNT note tracks) -- silent,
// since the caller already reports that once per file, not once per dropped note.
static bool hds_smf_parse_track(const uint8_t *data, size_t len,
                                 HdsSmfTempoEvent *tempo_events, int *tempo_count, int tempo_cap,
                                 HdsSmfMarkerEvent *markers, int *marker_count, int marker_cap,
                                 HdsSmfNoteEvent *notes, int *note_count, int note_cap) {
    size_t pos = 0;
    long abs_tick = 0;
    uint8_t running_status = 0;
    while (pos < len) {
        uint32_t delta;
        if (!hds_smf_read_vlq(data, len, &pos, &delta)) return false;
        abs_tick += (long)delta;
        if (pos >= len) return false; // a delta-time with no event following it -- truncated
        uint8_t status_byte = data[pos];
        if (status_byte == 0xFF) {
            pos++;
            if (pos >= len) return false;
            uint8_t meta_type = data[pos]; pos++;
            uint32_t meta_len;
            if (!hds_smf_read_vlq(data, len, &pos, &meta_len)) return false;
            if (meta_len > len - pos) return false;
            if (meta_type == 0x51) { // Set Tempo: always exactly 3 bytes, microseconds per quarter note, big-endian
                if (meta_len != 3) return false;
                uint32_t usec = ((uint32_t)data[pos] << 16) | ((uint32_t)data[pos + 1] << 8) | (uint32_t)data[pos + 2];
                if (*tempo_count < tempo_cap) { tempo_events[*tempo_count] = (HdsSmfTempoEvent){ abs_tick, usec }; (*tempo_count)++; }
                else fprintf(stderr, "[smf] more than %d tempo changes in this file -- dropping the rest\n", tempo_cap);
            } else if (meta_type == 0x06) { // Marker: an ASCII label, exactly meta_len bytes, not NUL-terminated in the file
                if (*marker_count < marker_cap) {
                    size_t copy_len = meta_len < (uint32_t)(HDS_SMF_MARKER_LABEL_LEN - 1) ? meta_len : (uint32_t)(HDS_SMF_MARKER_LABEL_LEN - 1);
                    HdsSmfMarkerEvent *me = &markers[*marker_count];
                    memcpy(me->label, &data[pos], copy_len);
                    me->label[copy_len] = '\0';
                    me->tick = abs_tick;
                    (*marker_count)++;
                } else fprintf(stderr, "[smf] more than %d markers in this file -- dropping the rest\n", marker_cap);
            }
            // Every other meta type (Track Name, End of Track, Time/Key Signature, Sequencer-
            // Specific, ...) is simply skipped by its own length prefix -- a real SMF reader must
            // tolerate any meta type it doesn't specifically use, not just the two above.
            pos += meta_len;
        } else if (status_byte == 0xF0 || status_byte == 0xF7) { // Sysex -- skip by its own length prefix
            pos++;
            uint32_t sysex_len;
            if (!hds_smf_read_vlq(data, len, &pos, &sysex_len)) return false;
            if (sysex_len > len - pos) return false;
            pos += sysex_len;
        } else {
            // A channel voice/mode message, possibly via running status: a status byte < 0x80 is
            // NOT a fresh status -- it's the first DATA byte of an event reusing whichever real
            // status byte came before it (a standard MIDI file-size optimization for runs of the
            // same message type, e.g. a whole chord's worth of Note On events).
            uint8_t status;
            if (status_byte & 0x80) { status = status_byte; running_status = status_byte; pos++; }
            else { status = running_status; if (status == 0) return false; } // no running status yet to fall back on -- corrupt
            uint8_t hi = status & 0xF0;
            int data_bytes = (hi == 0xC0 || hi == 0xD0) ? 1 : 2; // Program Change/Channel Pressure: 1 data byte; everything else here: 2
            if ((size_t)data_bytes > len - pos) return false;
            uint8_t d1 = data[pos];
            uint8_t d2 = (data_bytes == 2) ? data[pos + 1] : 0;
            pos += (size_t)data_bytes;
            if (hi == 0x80 || hi == 0x90) { // Note Off / Note On
                bool is_on = (hi == 0x90 && d2 > 0); // velocity-0 Note On counts as Note Off, matching this engine's own live MIDI decode convention
                if (*note_count < note_cap) { notes[*note_count] = (HdsSmfNoteEvent){ abs_tick, is_on ? (unsigned char)0 : (unsigned char)1, d1, d2 }; (*note_count)++; }
                else if (note_cap > 0) fprintf(stderr, "[smf] more than %d note events on one track -- dropping the rest\n", note_cap);
            }
            // Every other channel message (CC, Program Change, Pitch Bend, Aftertouch) is simply
            // ignored for import purposes -- matches seq_record_midi_event's own scope (this
            // engine's recorded Sequencer lanes only ever hold note-on/off events).
        }
    }
    return true;
}

static int hds_smf_tempo_event_cmp(const void *a, const void *b) {
    long ta = ((const HdsSmfTempoEvent *)a)->tick, tb = ((const HdsSmfTempoEvent *)b)->tick;
    return (ta > tb) - (ta < tb);
}
// Used only by hds_song_import_smf_track() below, which -- unlike hds_song_import_smf() -- can
// merge more than one track's own note events into a single lane, so their combined order needs
// an explicit sort (each track's own slice is already tick-ordered on its own, but concatenating
// several tracks' slices back-to-back in file order is not the same as concatenating them in TIME
// order). qsort's lack of a stability guarantee is harmless here: two notes landing on the exact
// same tick from different tracks fire simultaneously either way.
static int hds_smf_note_event_cmp(const void *a, const void *b) {
    long ta = ((const HdsSmfNoteEvent *)a)->tick, tb = ((const HdsSmfNoteEvent *)b)->tick;
    return (ta > tb) - (ta < tb);
}

// Converts an absolute tick position into real seconds by walking the (already tick-sorted)
// tempo map and accumulating elapsed time segment by segment. Before the first explicit Set Tempo
// event, the MIDI spec's own default applies: 500000 usec/quarter note = 120 BPM.
static double hds_smf_ticks_to_seconds(long target_tick, const HdsSmfTempoEvent *tempo_events, int tempo_count, int ticks_per_quarter) {
    double seconds = 0.0;
    long prev_tick = 0;
    uint32_t usec_per_qn = 500000;
    for (int i = 0; i < tempo_count && tempo_events[i].tick <= target_tick; i++) {
        seconds += (double)(tempo_events[i].tick - prev_tick) * (double)usec_per_qn / 1e6 / (double)ticks_per_quarter;
        usec_per_qn = tempo_events[i].usec_per_qn;
        prev_tick = tempo_events[i].tick;
    }
    seconds += (double)(target_tick - prev_tick) * (double)usec_per_qn / 1e6 / (double)ticks_per_quarter;
    return seconds;
}

// Opens, reads, and validates file_path's own MThd header -- shared by both the whole-song
// importer (hds_song_import_smf) and the single-lane/single-track importer
// (hds_song_import_smf_track) below, which differ only in what they do with each track's own
// tempo/marker/note events, not in how the file itself is opened or its header validated. On
// success returns the malloc'd raw file buffer positioned to read from byte 14 onward (the
// header is always exactly 14 bytes: "MThd" + u32 length(6) + u16 format/ntrks/division) --
// caller owns it and must free() it. On failure returns NULL with *error already filled in.
static uint8_t *hds_smf_read_and_validate_header(const char *file_path, long *out_fsize, uint16_t *out_ntrks, int *out_ticks_per_quarter, char *error, size_t error_len) {
    FILE *f = fopen(file_path, "rb");
    if (!f) { snprintf(error, error_len, "could not open file"); return NULL; }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 14 || fsize > HDS_SMF_FILE_SIZE_MAX) {
        fclose(f);
        snprintf(error, error_len, "not a plausible Standard MIDI File (size)");
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)fsize);
    if (!buf) { fclose(f); snprintf(error, error_len, "out of memory"); return NULL; }
    size_t read_bytes = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (read_bytes != (size_t)fsize) { free(buf); snprintf(error, error_len, "short read"); return NULL; }

    if (memcmp(buf, "MThd", 4) != 0) { free(buf); snprintf(error, error_len, "missing MThd header"); return NULL; }
    size_t pos = 4;
    uint32_t header_len; hds_smf_read_u32be(buf, (size_t)fsize, &pos, &header_len);
    if (header_len != 6) { free(buf); snprintf(error, error_len, "unexpected MThd length"); return NULL; }
    uint16_t format, ntrks, division;
    hds_smf_read_u16be(buf, (size_t)fsize, &pos, &format);
    hds_smf_read_u16be(buf, (size_t)fsize, &pos, &ntrks);
    hds_smf_read_u16be(buf, (size_t)fsize, &pos, &division);
    if (format == 2) { free(buf); snprintf(error, error_len, "Type 2 (independent unsynced tracks) has no single timeline -- not supported"); return NULL; }
    if (division & 0x8000) { free(buf); snprintf(error, error_len, "SMPTE timecode division is not supported (only ticks-per-quarter-note files)"); return NULL; }
    int ticks_per_quarter = (int)division;
    if (ticks_per_quarter <= 0) { free(buf); snprintf(error, error_len, "invalid ticks-per-quarter-note division"); return NULL; }

    *out_fsize = fsize;
    *out_ntrks = ntrks;
    *out_ticks_per_quarter = ticks_per_quarter;
    return buf;
}

// Imports file_path into engine->seq_midi[]/knobs, per this section's own header comment.
// import_tempo controls ONLY whether the file's Set Tempo events also get applied to Single
// Program mode's own tempo-map knobs (programs[SINGLE_PROGRAM_SLOT].tempo_marker_*) -- note
// timing itself always uses the file's own tempo map to convert ticks to seconds regardless of
// this flag, since that's simply what correct playback of the file requires.
static bool hds_song_import_smf(HdsEngineState *engine, SynthSharedControls *knobs, const char *file_path, bool import_tempo, HdsSmfImportResult *result) {
    *result = (HdsSmfImportResult){0};
    long fsize; uint16_t ntrks; int ticks_per_quarter;
    uint8_t *buf = hds_smf_read_and_validate_header(file_path, &fsize, &ntrks, &ticks_per_quarter, result->error, sizeof(result->error));
    if (!buf) return false;
    size_t pos = 14;

    static HdsSmfTempoEvent tempo_events[HDS_SMF_TEMPO_EVENT_MAX];
    int tempo_count = 0;
    static HdsSmfMarkerEvent marker_events[HDS_SMF_MARKER_MAX];
    int marker_count = 0;
    // Per-track note staging -- static, far too large for the stack (mirrors hds_song_load_midi's
    // own identical reasoning for its "loaded[]" staging array).
    static HdsSmfNoteEvent note_stage[SEQ_LANE_COUNT][SEQ_MIDI_EVENT_CAPACITY];
    static int note_stage_count[SEQ_LANE_COUNT];
    int lanes_used = 0;
    memset(note_stage_count, 0, sizeof(note_stage_count));

    for (int t = 0; t < (int)ntrks; t++) {
        if (pos + 8 > (size_t)fsize || memcmp(&buf[pos], "MTrk", 4) != 0) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "missing MTrk header for track %d", t);
            return false;
        }
        pos += 4;
        uint32_t track_len; hds_smf_read_u32be(buf, (size_t)fsize, &pos, &track_len);
        if (track_len > (size_t)fsize - pos) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "track %d runs past end of file", t);
            return false;
        }
        int this_track_note_count = 0;
        HdsSmfNoteEvent *dest_notes = (lanes_used < SEQ_LANE_COUNT) ? note_stage[lanes_used] : NULL;
        int dest_cap = (dest_notes != NULL) ? SEQ_MIDI_EVENT_CAPACITY : 0;
        bool track_ok = hds_smf_parse_track(&buf[pos], track_len,
                                             tempo_events, &tempo_count, HDS_SMF_TEMPO_EVENT_MAX,
                                             marker_events, &marker_count, HDS_SMF_MARKER_MAX,
                                             dest_notes, &this_track_note_count, dest_cap);
        if (!track_ok) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "corrupt event data in track %d", t);
            return false;
        }
        if (this_track_note_count > 0) {
            if (lanes_used < SEQ_LANE_COUNT) {
                note_stage_count[lanes_used] = this_track_note_count;
                lanes_used++;
            } else {
                fprintf(stderr, "[smf] more than %d note-carrying tracks in this file -- dropping track %d entirely\n", SEQ_LANE_COUNT, t);
            }
        }
        pos += track_len;
    }
    free(buf);

    qsort(tempo_events, (size_t)tempo_count, sizeof(HdsSmfTempoEvent), hds_smf_tempo_event_cmp);

    // Everything parsed cleanly -- commit. Clears every lane first (a real Load, not a merge --
    // same "no stale leftovers" posture as hds_song_load_midi), then refills however many the
    // file actually used.
    for (int l = 0; l < SEQ_LANE_COUNT; l++) { engine->seq_midi[l].event_count = 0; engine->seq_midi[l].playback_cursor = 0; }
    for (int l = 0; l < lanes_used; l++) {
        int n = note_stage_count[l];
        engine->seq_midi[l].event_count = n;
        for (int e = 0; e < n; e++) {
            engine->seq_midi[l].events[e] = (SeqMidiEvent){
                (float)hds_smf_ticks_to_seconds(note_stage[l][e].tick, tempo_events, tempo_count, ticks_per_quarter),
                note_stage[l][e].type, note_stage[l][e].note, note_stage[l][e].velocity
            };
        }
    }

    if (import_tempo) {
        ProgramState *prog = &knobs->programs[SINGLE_PROGRAM_SLOT];
        // MIDI Set Tempo events are instantaneous: the tempo holds until the next one. The tempo
        // map interpolates linearly between markers, so each change is written as a STEP -- a
        // marker holding the old tempo at the change time, then one with the new tempo at the
        // same time (compute_current_tempo_bpm resolves same-time markers to the last one).
        // Repeated identical tempos are skipped, and of several events at the same tick the last
        // one wins, so only real changes use map slots.
        float step_time[TEMPO_MARKER_MAX], step_bpm[TEMPO_MARKER_MAX];
        int steps = 0, dropped = 0;
        for (int i = 0; i < tempo_count; i++) {
            float t = (float)hds_smf_ticks_to_seconds(tempo_events[i].tick, tempo_events, tempo_count, ticks_per_quarter);
            float bpm = 60000000.0f / (float)tempo_events[i].usec_per_qn;
            if (steps > 0 && step_time[steps - 1] == t) { step_bpm[steps - 1] = bpm; continue; }   // same tick: last wins
            if (steps > 0 && step_bpm[steps - 1] == bpm) continue;                                  // no actual change
            // Each change after the first needs 2 map slots (hold + new tempo).
            if (steps >= TEMPO_MARKER_MAX || (steps > 0 && 2 * steps + 1 > TEMPO_MARKER_MAX)) { dropped++; continue; }
            step_time[steps] = t; step_bpm[steps] = bpm; steps++;
        }
        if (steps > 1 && step_bpm[steps - 1] == step_bpm[steps - 2]) steps--; // a same-tick override can undo a change
        if (dropped > 0) {
            fprintf(stderr, "[smf] file has more tempo changes than this engine's %d-slot tempo map holds as steps "
                            "(%d per change) -- keeping the first %d, dropping %d\n", TEMPO_MARKER_MAX, 2, steps, dropped);
        }
        if (steps <= 1) {
            // One flat, unchanging tempo (the common case): no map needed at all -- matches
            // compute_current_tempo_bpm()'s own "no markers" fast path.
            prog->tempo_bpm = (steps == 1) ? step_bpm[0] : 120.0f;
            prog->tempo_marker_count = 0.0f;
        } else {
            int n = 0;
            prog->tempo_marker_time[n] = step_time[0]; prog->tempo_marker_bpm[n] = step_bpm[0]; n++;
            for (int i = 1; i < steps; i++) {
                prog->tempo_marker_time[n] = step_time[i]; prog->tempo_marker_bpm[n] = step_bpm[i - 1]; n++; // hold the old tempo up to the change
                prog->tempo_marker_time[n] = step_time[i]; prog->tempo_marker_bpm[n] = step_bpm[i];     n++; // then switch
            }
            prog->tempo_marker_count = (float)n;
            // Keep the flat tempo consistent with the map: the song's starting tempo. Playback
            // ignores it while markers exist (compute_current_tempo_bpm holds the first marker's
            // bpm before it anyway), but the import reply, the state dump and any controller that
            // caches tempo_bpm would otherwise see whatever stale value was there before.
            prog->tempo_bpm = prog->tempo_marker_bpm[0];
        }
    }

    int n_markers = marker_count < HDS_SMF_MARKER_MAX ? marker_count : HDS_SMF_MARKER_MAX;
    for (int i = 0; i < n_markers; i++) {
        result->marker_time[i] = (float)hds_smf_ticks_to_seconds(marker_events[i].tick, tempo_events, tempo_count, ticks_per_quarter);
        snprintf(result->marker_label[i], HDS_SMF_MARKER_LABEL_LEN, "%s", marker_events[i].label);
    }
    // Sort markers by time -- almost always already in order (collected in increasing-tick order
    // per track, and markers conventionally live on just one track), but a stable guarantee costs
    // nothing here (marker_count is capped small) and the Sequencer tab's Markers list shouldn't
    // have to re-sort what should already read as a timeline. Plain insertion sort (small N).
    for (int i = 1; i < n_markers; i++) {
        float t = result->marker_time[i];
        char lbl[HDS_SMF_MARKER_LABEL_LEN]; snprintf(lbl, sizeof(lbl), "%s", result->marker_label[i]);
        int j = i - 1;
        while (j >= 0 && result->marker_time[j] > t) {
            result->marker_time[j + 1] = result->marker_time[j];
            memcpy(result->marker_label[j + 1], result->marker_label[j], HDS_SMF_MARKER_LABEL_LEN);
            j--;
        }
        result->marker_time[j + 1] = t;
        snprintf(result->marker_label[j + 1], HDS_SMF_MARKER_LABEL_LEN, "%s", lbl);
    }

    result->ok = true;
    result->lanes_used = lanes_used;
    result->tempo_applied = import_tempo;
    result->marker_count = n_markers;
    return true;
}

typedef struct {
    bool ok;
    char error[128];
    int lane;
    int note_count;
    int tracks_with_notes; // > 1 means more than one track in the file had notes -- they were all merged into this one lane, not silently dropped
} HdsSmfTrackImportResult;

// Per-lane "Load MIDI..." (Sequencer tab, each Program/MIDI lane's own strip): imports a single
// .mid/.midi file's note events straight into ONE lane, replacing whatever it currently holds --
// distinct from hds_song_import_smf's whole-song import just above, which repopulates EVERY lane
// and can also apply the file's tempo map. This never touches tempo_marker_*/the Markers list at
// all -- those are exclusively a whole-song concept; a single dropped-in track has no "song
// position" of its own to speak of. Every note-carrying track in the file is merged together,
// sorted by time, into the target lane -- most single-track exports only ever have one such
// track anyway, but a stray extra one (e.g. a leftover tempo-only conductor track alongside the
// real notes) merges in rather than getting silently dropped.
static bool hds_song_import_smf_track(HdsEngineState *engine, int lane_index, const char *file_path, HdsSmfTrackImportResult *result) {
    *result = (HdsSmfTrackImportResult){0};
    result->lane = lane_index;
    if (lane_index < 0 || lane_index >= SEQ_LANE_COUNT) { snprintf(result->error, sizeof(result->error), "invalid lane index"); return false; }

    long fsize; uint16_t ntrks; int ticks_per_quarter;
    uint8_t *buf = hds_smf_read_and_validate_header(file_path, &fsize, &ntrks, &ticks_per_quarter, result->error, sizeof(result->error));
    if (!buf) return false;
    size_t pos = 14;

    static HdsSmfTempoEvent tempo_events[HDS_SMF_TEMPO_EVENT_MAX];
    int tempo_count = 0;
    // Collected (Set Tempo meta-events are still needed to convert this file's OWN ticks to
    // correct real seconds) but never applied to any knob -- see this function's own comment.
    static HdsSmfMarkerEvent marker_events[HDS_SMF_MARKER_MAX]; // collected, then simply discarded -- markers are a whole-song concept only
    int marker_count = 0;
    static HdsSmfNoteEvent note_stage[SEQ_MIDI_EVENT_CAPACITY]; // every note-carrying track's events, concatenated (sorted by time below)
    int note_count = 0;
    int tracks_with_notes = 0;

    for (int t = 0; t < (int)ntrks; t++) {
        if (pos + 8 > (size_t)fsize || memcmp(&buf[pos], "MTrk", 4) != 0) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "missing MTrk header for track %d", t);
            return false;
        }
        pos += 4;
        uint32_t track_len; hds_smf_read_u32be(buf, (size_t)fsize, &pos, &track_len);
        if (track_len > (size_t)fsize - pos) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "track %d runs past end of file", t);
            return false;
        }
        int this_track_note_count = 0;
        int remaining_cap = SEQ_MIDI_EVENT_CAPACITY - note_count;
        bool track_ok = hds_smf_parse_track(&buf[pos], track_len,
                                             tempo_events, &tempo_count, HDS_SMF_TEMPO_EVENT_MAX,
                                             marker_events, &marker_count, HDS_SMF_MARKER_MAX,
                                             &note_stage[note_count], &this_track_note_count, remaining_cap);
        if (!track_ok) {
            free(buf);
            snprintf(result->error, sizeof(result->error), "corrupt event data in track %d", t);
            return false;
        }
        if (this_track_note_count > 0) { note_count += this_track_note_count; tracks_with_notes++; }
        pos += track_len;
    }
    free(buf);

    qsort(tempo_events, (size_t)tempo_count, sizeof(HdsSmfTempoEvent), hds_smf_tempo_event_cmp);
    qsort(note_stage, (size_t)note_count, sizeof(HdsSmfNoteEvent), hds_smf_note_event_cmp);

    engine->seq_midi[lane_index].event_count = note_count;
    engine->seq_midi[lane_index].playback_cursor = 0;
    for (int e = 0; e < note_count; e++) {
        engine->seq_midi[lane_index].events[e] = (SeqMidiEvent){
            (float)hds_smf_ticks_to_seconds(note_stage[e].tick, tempo_events, tempo_count, ticks_per_quarter),
            note_stage[e].type, note_stage[e].note, note_stage[e].velocity
        };
    }

    result->ok = true;
    result->note_count = note_count;
    result->tracks_with_notes = tracks_with_notes;
    return true;
}

// Fired once, synchronously -- same posture as song_import_smf_handler() just below, which this
// mirrors closely (a per-lane import instead of a whole-song one, no tempo/knobs argument since
// this never touches any).
int song_import_smf_track_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types;
    HdsSmfTrackImportContext *ctx = (HdsSmfTrackImportContext *)user_data;
    if (argc != 2) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    int lane_index = (int)argv[1]->f;
    HdsSmfTrackImportResult result;
    if (!hds_song_import_smf_track(ctx->engine, lane_index, file_path, &result)) {
        fprintf(stderr, "[smf] track import of '%s' into lane %d failed: %s\n", file_path, lane_index, result.error);
    }
    // Reply to the actual sender (see request_state_handler's own comment on why) rather than
    // ctx->reply_addr's fixed startup target -- whichever frontend just asked for this import is
    // the one that should hear back, not necessarily whoever --osc-reply-host/--osc-reply-port
    // originally pointed at.
    lo_address reply_addr = lo_message_get_source(msg);
    if (!reply_addr) return 0;
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    lo_message m;
    m = lo_message_new(); lo_message_add_float(m, result.ok ? 1.0f : 0.0f); lo_bundle_add_message(b, "/hds/status/smf_import_track/ok", m);
    m = lo_message_new(); lo_message_add_string(m, result.error); lo_bundle_add_message(b, "/hds/status/smf_import_track/error", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.lane); lo_bundle_add_message(b, "/hds/status/smf_import_track/lane", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.note_count); lo_bundle_add_message(b, "/hds/status/smf_import_track/note_count", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.tracks_with_notes); lo_bundle_add_message(b, "/hds/status/smf_import_track/tracks_with_notes", m);
    lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b);
    return 0;
}

// Fired once, synchronously, right from this handler (same "rare, discrete, user-initiated
// action" posture as hds_song_save_midi/load_midi -- see that section's own REALTIME NOTE): parses
// the file, applies it, and immediately reports the result back to reply_addr as one small bundle
// -- there's no periodic per-block status poll for this the way send_seq_audio_status has, since
// an import is a one-shot event, not ongoing state.
int song_import_smf_handler(const char *path, const char *types, lo_arg **argv, int argc, lo_message msg, void *user_data) {
    (void)path; (void)types;
    HdsSmfImportContext *ctx = (HdsSmfImportContext *)user_data;
    if (argc != 2) return 0;
    const char *file_path = (const char *)&argv[0]->s;
    bool import_tempo = argv[1]->f != 0.0f;
    HdsSmfImportResult result;
    if (!hds_song_import_smf(ctx->engine, ctx->knobs, file_path, import_tempo, &result)) {
        fprintf(stderr, "[smf] import of '%s' failed: %s\n", file_path, result.error);
    }
    // Reply to the actual sender -- see request_state_handler's own comment.
    lo_address reply_addr = lo_message_get_source(msg);
    if (!reply_addr) return 0;
    lo_bundle b = lo_bundle_new(LO_TT_IMMEDIATE);
    lo_message m;
    m = lo_message_new(); lo_message_add_float(m, result.ok ? 1.0f : 0.0f); lo_bundle_add_message(b, "/hds/status/smf_import/ok", m);
    m = lo_message_new(); lo_message_add_string(m, result.error); lo_bundle_add_message(b, "/hds/status/smf_import/error", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.lanes_used); lo_bundle_add_message(b, "/hds/status/smf_import/lanes_used", m);
    m = lo_message_new(); lo_message_add_float(m, result.tempo_applied ? 1.0f : 0.0f); lo_bundle_add_message(b, "/hds/status/smf_import/tempo_applied", m);
    m = lo_message_new(); lo_message_add_float(m, (float)result.marker_count); lo_bundle_add_message(b, "/hds/status/smf_import/marker_count", m);
    for (int i = 0; i < result.marker_count; i++) {
        char addr[64];
        snprintf(addr, sizeof(addr), "/hds/status/smf_import/marker/%d/time", i);
        m = lo_message_new(); lo_message_add_float(m, result.marker_time[i]); lo_bundle_add_message(b, addr, m);
        snprintf(addr, sizeof(addr), "/hds/status/smf_import/marker/%d/label", i);
        m = lo_message_new(); lo_message_add_string(m, result.marker_label[i]); lo_bundle_add_message(b, addr, m);
    }
    // Tempo map values, if applied -- so a controller that resends every knob's own current value
    // on any change (tempo markers included) can cache these and keep echoing them back, rather
    // than blasting the just-imported tempo map back to flat/zero on the very next unrelated knob
    // tweak for lack of its own cached copy.
    if (result.tempo_applied) {
        const ProgramState *prog = &ctx->knobs->programs[SINGLE_PROGRAM_SLOT];
        int n = (int)prog->tempo_marker_count;
        m = lo_message_new(); lo_message_add_float(m, prog->tempo_bpm); lo_bundle_add_message(b, "/hds/status/smf_import/tempo_bpm", m);
        m = lo_message_new(); lo_message_add_float(m, (float)n); lo_bundle_add_message(b, "/hds/status/smf_import/tempo_marker_count", m);
        for (int i = 0; i < n; i++) {
            char addr[64];
            snprintf(addr, sizeof(addr), "/hds/status/smf_import/tempo_marker/%d/time", i);
            m = lo_message_new(); lo_message_add_float(m, prog->tempo_marker_time[i]); lo_bundle_add_message(b, addr, m);
            snprintf(addr, sizeof(addr), "/hds/status/smf_import/tempo_marker/%d/bpm", i);
            m = lo_message_new(); lo_message_add_float(m, prog->tempo_marker_bpm[i]); lo_bundle_add_message(b, addr, m);
        }
    }
    lo_send_bundle(reply_addr, b);
    lo_bundle_free_recursive(b);
    return 0;
}
