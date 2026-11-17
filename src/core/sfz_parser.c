// sfz_parser.c -- Native SFZ file parser: tokenizer, #define/#include handling, opcode -> SfzRegion. The loader that turns regions into Sample Bank zones (hds_load_sfz_for_osc) stays in backend.c.
// Moved verbatim out of backend.c.
#include "core/sfz_parser.h"
#include "core/hds_compat.h" // realpath()/NAME_MAX on Windows

// ============================================================================
// Native SFZ file parser -- lets the backend load a multisample instrument directly from a local
// .sfz file, without any external controller ever having been connected (see the CLI --sfz-vcoN
// flags and the /hds/zone/load_sfz OSC method, both further down, which are the two things that
// actually call sfz_parse_file()/hds_load_sfz_for_osc()). #define/$variable macros are NOT
// supported: a region using one is skipped (its sample= fails to resolve), never crashed on or
// silently mis-mapped.
// ============================================================================

SfzRegion g_sfz_parse_scratch[SFZ_MAX_REGIONS_PER_FILE];

// Bare MIDI int, or a note name like "C4"/"F#3"/"Ab-1" (C4=60, 's' accepted as an alternate sharp
// spelling). Returns -1 on unparseable/out-of-range input.
int sfz_note_to_number(const char *value) {
    if (!value || !*value) return -1;
    // Bare integer (allowing a leading '-' for the theoretical negative case, though notes are
    // always 0-127 in practice).
    char *endptr = NULL;
    long as_int = strtol(value, &endptr, 10);
    if (endptr && *endptr == '\0' && endptr != value) {
        if (as_int < 0 || as_int > 127) return -1;
        return (int)as_int;
    }
    // Note name: letter (a-g/A-G), optional accidental (#, s, S, b, B), signed octave integer.
    const char *p = value;
    if (!isalpha((unsigned char)*p)) return -1;
    char letter = (char)tolower((unsigned char)*p); p++;
    static const int PITCH_CLASS[7] = {9, 11, 0, 2, 4, 5, 7}; // a,b,c,d,e,f,g
    if (letter < 'a' || letter > 'g') return -1;
    int pitch_class = PITCH_CLASS[letter - 'a'];
    if (*p == '#' || *p == 's' || *p == 'S') { pitch_class += 1; p++; }
    else if (*p == 'b' || *p == 'B') { pitch_class -= 1; p++; }
    if (!*p) return -1;
    endptr = NULL;
    long octave = strtol(p, &endptr, 10);
    if (!endptr || *endptr != '\0' || endptr == p) return -1;
    long note = (octave + 1) * 12 + pitch_class;
    if (note < 0 || note > 127) return -1;
    return (int)note;
}

// Mirrors _sfz_frame_to_frac(): an absolute frame-count string + total_frames -> a 0-1 fraction.
// Returns false (out_frac left untouched) if frame_str isn't a valid number.
bool sfz_frame_to_frac(const char *frame_str, unsigned int total_frames, float *out_frac) {
    if (!frame_str || !*frame_str || total_frames <= 1) return false;
    char *endptr = NULL;
    double frame = strtod(frame_str, &endptr);
    if (!endptr || endptr == frame_str) return false;
    double frac = frame / (double)(total_frames - 1);
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    *out_frac = (float)frac;
    return true;
}

// Mirrors _resolve_sfz_sample_path(): backslashes normalized to '/' first (Windows-authored
// libraries), then tried in order: (1) as-is if absolute and it exists; (2) joined with
// default_path (the <control> header's own default_path=, itself already resolved against
// sfz_dir by the caller); (3) joined with sfz_dir; (4) a case-insensitive filename match via
// opendir()/readdir() within whichever of (2)/(3) resolved to a real, existing directory.
// Bounded "dir/name" join with an explicit precision on each "%s", for the same
// -Wformat-truncation reason sfz_set_text_field exists -- dir can come from a PATH_MAX-sized
// directory buffer several frames up (sfz_parse_file_recursive's sfz_dir), but the destination is
// the SAMPLE_PATH_LEN(256) bound every actual sample path in this file already lives within; a
// join that would overflow simply truncates, and the resulting fopen() attempt then just fails
// like any other unresolvable candidate path (skip, don't crash).
static void sfz_join_path(char *dst, size_t dst_len, const char *dir, const char *name) {
    int half = (int)(dst_len / 2) - 1;
    if (half < 1) half = 1;
    snprintf(dst, dst_len, "%.*s/%.*s", half, dir, half, name);
}

static bool sfz_resolve_sample_path(const char *sample_value, const char *sfz_dir, const char *default_path, char *out_resolved, size_t out_len) {
    char normalized[SAMPLE_PATH_LEN];
    snprintf(normalized, sizeof(normalized), "%s", sample_value);
    for (char *c = normalized; *c; c++) if (*c == '\\') *c = '/';

    if (normalized[0] == '/') {
        FILE *f = fopen(normalized, "rb");
        if (f) { fclose(f); snprintf(out_resolved, out_len, "%s", normalized); return true; }
    }

    const char *candidate_dirs[2]; int candidate_dir_count = 0;
    if (default_path && default_path[0]) candidate_dirs[candidate_dir_count++] = default_path;
    candidate_dirs[candidate_dir_count++] = sfz_dir;

    for (int d = 0; d < candidate_dir_count; d++) {
        char joined[SAMPLE_PATH_LEN];
        sfz_join_path(joined, sizeof(joined), candidate_dirs[d], normalized);
        FILE *f = fopen(joined, "rb");
        if (f) { fclose(f); snprintf(out_resolved, out_len, "%s", joined); return true; }
    }

    // Case-insensitive filename fallback: only the basename needs matching (a sample= value with
    // its own subdirectory prefix that merely differs in case is rare enough not to be worth
    // walking a full recursive search for).
    const char *base = strrchr(normalized, '/');
    base = base ? base + 1 : normalized;
    for (int d = 0; d < candidate_dir_count; d++) {
        DIR *dir = opendir(candidate_dirs[d]);
        if (!dir) continue;
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcasecmp(entry->d_name, base) == 0) {
                char joined[SAMPLE_PATH_LEN];
                sfz_join_path(joined, sizeof(joined), candidate_dirs[d], entry->d_name);
                FILE *f = fopen(joined, "rb");
                if (f) { fclose(f); closedir(dir); snprintf(out_resolved, out_len, "%s", joined); return true; }
            }
        }
        closedir(dir);
    }
    return false;
}

// Finds the next SFZ token (a "<name>" header or a "name=" opcode key) at or after `pos`, mirroring
// _SFZ_TOKEN_RE's own "<(\w+)>|([a-zA-Z_][a-zA-Z0-9_]*)=" alternation -- a header always starts
// with a literal '<', an opcode key always starts with a letter/underscore, so there's no ambiguity
// in which pattern to try at a given scan position. Returns the token's own start position (its
// end position is written to *out_token_end), or SFZ_TOKEN_NOT_FOUND if nothing matched before len.
#define SFZ_TOKEN_NOT_FOUND (-1)
static long sfz_find_next_token(const char *text, size_t pos, size_t len, size_t *out_token_end, bool *out_is_header, char *out_name, size_t out_name_cap) {
    for (size_t i = pos; i < len; i++) {
        if (text[i] == '<') {
            size_t j = i + 1, n = 0;
            while (j < len && (isalnum((unsigned char)text[j]) || text[j] == '_')) { if (n + 1 < out_name_cap) out_name[n++] = text[j]; j++; }
            if (n > 0 && j < len && text[j] == '>') {
                out_name[n] = '\0';
                *out_is_header = true;
                *out_token_end = j + 1;
                return (long)i;
            }
            continue;
        }
        if (isalpha((unsigned char)text[i]) || text[i] == '_') {
            size_t j = i, n = 0;
            while (j < len && (isalnum((unsigned char)text[j]) || text[j] == '_')) { if (n + 1 < out_name_cap) out_name[n++] = text[j]; j++; }
            if (n > 0 && j < len && text[j] == '=') {
                out_name[n] = '\0';
                *out_is_header = false;
                *out_token_end = j + 1;
                return (long)i;
            }
            // Not immediately followed by '=' -- not an opcode token; resume scanning right after
            // this identifier run (not i+1) to avoid re-testing every character inside it.
            i = j - 1;
            continue;
        }
    }
    return SFZ_TOKEN_NOT_FOUND;
}

// Bounded copy into a fixed-size SfzRegion text field, with an explicit precision on the "%s" so
// -Wformat-truncation can prove the write can never overflow regardless of value's actual length
// -- value always comes from a much larger scratch buffer (see sfz_tokenize_and_apply's own
// value[SAMPLE_PATH_LEN]), which is what triggers the warning on a plain "%s" here. Truncation
// itself is harmless for every field this is used on (note names/loop-mode keywords/group ids are
// always short in real SFZ files).
static void sfz_set_text_field(char *dst, size_t dst_len, const char *value) {
    snprintf(dst, dst_len, "%.*s", (int)(dst_len - 1), value);
}

// Applies one opcode (already-trimmed key/value strings) onto a region-shaped accumulator --
// shared by every header level (<global>/<group>/<region>/<control>/anything-else-thrown-away).
static void sfz_apply_opcode(SfzRegion *r, const char *key, const char *value) {
    if (strcasecmp(key, "sample") == 0) { r->has_sample = true; sfz_set_text_field(r->sample, sizeof(r->sample), value); }
    else if (strcasecmp(key, "key") == 0) { r->has_key = true; sfz_set_text_field(r->key, sizeof(r->key), value); }
    else if (strcasecmp(key, "lokey") == 0) { r->has_lokey = true; sfz_set_text_field(r->lokey, sizeof(r->lokey), value); }
    else if (strcasecmp(key, "hikey") == 0) { r->has_hikey = true; sfz_set_text_field(r->hikey, sizeof(r->hikey), value); }
    else if (strcasecmp(key, "pitch_keycenter") == 0) { r->has_pitch_keycenter = true; sfz_set_text_field(r->pitch_keycenter, sizeof(r->pitch_keycenter), value); }
    else if (strcasecmp(key, "pitch_keytrack") == 0) { r->has_pitch_keytrack = true; r->pitch_keytrack = (float)atof(value); }
    else if (strcasecmp(key, "lovel") == 0) { r->has_lovel = true; r->lovel = atoi(value); }
    else if (strcasecmp(key, "hivel") == 0) { r->has_hivel = true; r->hivel = atoi(value); }
    else if (strcasecmp(key, "loop_mode") == 0) { r->has_loop_mode = true; sfz_set_text_field(r->loop_mode, sizeof(r->loop_mode), value); }
    else if (strcasecmp(key, "loop_start") == 0) { r->has_loop_start = true; sfz_set_text_field(r->loop_start, sizeof(r->loop_start), value); }
    else if (strcasecmp(key, "loop_end") == 0) { r->has_loop_end = true; sfz_set_text_field(r->loop_end, sizeof(r->loop_end), value); }
    else if (strcasecmp(key, "loop_crossfade") == 0) { r->has_loop_crossfade = true; r->loop_crossfade = (float)atof(value); }
    else if (strcasecmp(key, "offset") == 0) { r->has_offset = true; sfz_set_text_field(r->offset, sizeof(r->offset), value); }
    else if (strcasecmp(key, "end") == 0) { r->has_end = true; sfz_set_text_field(r->end, sizeof(r->end), value); }
    else if (strcasecmp(key, "tune") == 0 || strcasecmp(key, "pitch") == 0) {
        // "pitch=" is the fallback spelling for "tune=" -- both write the same field (matches
        // _build_zones_from_sfz's own "tune= (fallback pitch=)" note).
        r->has_tune = true; r->tune = (float)atof(value);
    }
    else if (strcasecmp(key, "volume") == 0) { r->has_volume = true; r->volume = (float)atof(value); }
    else if (strcasecmp(key, "pan") == 0) { r->has_pan = true; r->pan = (float)atof(value); }
    else if (strcasecmp(key, "direction") == 0) { r->has_direction = true; sfz_set_text_field(r->direction, sizeof(r->direction), value); }
    else if (strcasecmp(key, "hds_drum_bus") == 0) { r->has_drum_bus = true; r->drum_bus = atoi(value); }
    else if (strcasecmp(key, "group") == 0) { r->has_group = true; r->group = atoi(value); }
    else if (strcasecmp(key, "off_by") == 0 || strcasecmp(key, "offby") == 0) { r->has_off_by = true; r->off_by = atoi(value); }
    else if (strcasecmp(key, "seq_length") == 0) { r->has_seq_length = true; r->seq_length = atoi(value); }
    else if (strcasecmp(key, "seq_position") == 0) { r->has_seq_position = true; r->seq_position = atoi(value); }
    else if (strcasecmp(key, "lorand") == 0) { r->has_lorand = true; r->lorand = (float)atof(value); }
    else if (strcasecmp(key, "hirand") == 0) { r->has_hirand = true; r->hirand = (float)atof(value); }
    else if (strcasecmp(key, "trigger") == 0) { r->has_trigger = true; sfz_set_text_field(r->trigger, sizeof(r->trigger), value); }
    else if (strcasecmp(key, "sw_lokey") == 0) { r->has_sw_lokey = true; sfz_set_text_field(r->sw_lokey, sizeof(r->sw_lokey), value); }
    else if (strcasecmp(key, "sw_hikey") == 0) { r->has_sw_hikey = true; sfz_set_text_field(r->sw_hikey, sizeof(r->sw_hikey), value); }
    else if (strcasecmp(key, "sw_last") == 0) { r->has_sw_last = true; sfz_set_text_field(r->sw_last, sizeof(r->sw_last), value); }
    else if (strcasecmp(key, "sw_default") == 0) { r->has_sw_default = true; sfz_set_text_field(r->sw_default, sizeof(r->sw_default), value); }
    else if (strcasecmp(key, "xfin_lovel") == 0) { r->has_xfin_lovel = true; r->xfin_lovel = atoi(value); }
    else if (strcasecmp(key, "xfin_hivel") == 0) { r->has_xfin_hivel = true; r->xfin_hivel = atoi(value); }
    else if (strcasecmp(key, "xfout_lovel") == 0) { r->has_xfout_lovel = true; r->xfout_lovel = atoi(value); }
    else if (strcasecmp(key, "xfout_hivel") == 0) { r->has_xfout_hivel = true; r->xfout_hivel = atoi(value); }
    else if (strcasecmp(key, "xfin_lokey") == 0) { r->has_xfin_lokey = true; sfz_set_text_field(r->xfin_lokey, sizeof(r->xfin_lokey), value); }
    else if (strcasecmp(key, "xfin_hikey") == 0) { r->has_xfin_hikey = true; sfz_set_text_field(r->xfin_hikey, sizeof(r->xfin_hikey), value); }
    else if (strcasecmp(key, "xfout_lokey") == 0) { r->has_xfout_lokey = true; sfz_set_text_field(r->xfout_lokey, sizeof(r->xfout_lokey), value); }
    else if (strcasecmp(key, "xfout_hikey") == 0) { r->has_xfout_hikey = true; sfz_set_text_field(r->xfout_hikey, sizeof(r->xfout_hikey), value); }
    else if (strcasecmp(key, "rt_decay") == 0) { r->has_rt_decay = true; r->rt_decay = (float)atof(value); }
    else if (strcasecmp(key, "delay") == 0) { r->has_delay = true; r->delay = (float)atof(value); }
    else if (strcasecmp(key, "ampeg_delay") == 0) { r->has_ampeg_delay = true; r->ampeg_delay = (float)atof(value); }
    else if (strcasecmp(key, "ampeg_attack") == 0) { r->has_ampeg_attack = true; r->ampeg_attack = (float)atof(value); }
    else if (strcasecmp(key, "ampeg_hold") == 0) { r->has_ampeg_hold = true; r->ampeg_hold = (float)atof(value); }
    else if (strcasecmp(key, "ampeg_decay") == 0) { r->has_ampeg_decay = true; r->ampeg_decay = (float)atof(value); }
    else if (strcasecmp(key, "ampeg_sustain") == 0) { r->has_ampeg_sustain = true; r->ampeg_sustain = (float)atof(value); }
    else if (strcasecmp(key, "ampeg_release") == 0) { r->has_ampeg_release = true; r->ampeg_release = (float)atof(value); }
    else if (strcasecmp(key, "ampeg_start") == 0) { r->has_ampeg_start = true; r->ampeg_start = (float)atof(value); }
    else if (strcasecmp(key, "note_polyphony") == 0) { r->has_note_polyphony = true; r->note_polyphony = atoi(value); }
    else if (strncasecmp(key, "amp_velcurve_", 13) == 0) {
        // amp_velcurve_<N>=<value> -- a dynamic opcode NAME, not a fixed one: the velocity point
        // is embedded in the key itself. Kept sorted by ascending point as each one is applied (a
        // plain insertion, never more than SFZ_MAX_VELCURVE_POINTS long) so sfz_velcurve_gain()
        // can binary/linear-scan it directly at note-on without needing its own sort pass.
        int point = atoi(key + 13);
        if (point < 0) point = 0; else if (point > 127) point = 127;
        float gain = (float)atof(value);
        int insert_at = r->velcurve_count;
        for (int i = 0; i < r->velcurve_count; i++) {
            if (r->velcurve_point[i] == point) { r->velcurve_gain[i] = gain; return; } // re-set of the same point -- replace in place, don't grow
            if (r->velcurve_point[i] > point) { insert_at = i; break; }
        }
        if (r->velcurve_count >= SFZ_MAX_VELCURVE_POINTS) return; // table full -- see SFZ_MAX_VELCURVE_POINTS's own comment
        for (int i = r->velcurve_count; i > insert_at; i--) { r->velcurve_point[i] = r->velcurve_point[i - 1]; r->velcurve_gain[i] = r->velcurve_gain[i - 1]; }
        r->velcurve_point[insert_at] = point;
        r->velcurve_gain[insert_at] = gain;
        r->velcurve_count++;
    }
    // Any other opcode (filter, curve, MIDI CC modulation, etc.) is silently ignored -- out of
    // scope for HDS's own zone model, same posture _build_zones_from_sfz's own opcode table
    // already has.
}

// Tokenizes one already-comment-stripped, #include-blanked text buffer into <global>/<group>/
// <master>/<region>/<control> opcode sets, applying inheritance exactly like _parse_sfz's own
// current_dict swap: <global> resets the global accumulator; <group>/<master> both copy from the
// current global accumulator into a fresh group accumulator; <region> copies from the current
// group accumulator into a fresh region, appended to out_regions once the NEXT header (or EOF) is
// reached; <control> opcodes go into a separate accumulator (only default_path= is used, resolved
// against base_dir); any other header (<curve>, <effect>, ...) tokenizes into a throwaway
// accumulator and is otherwise ignored.
static void sfz_tokenize_and_apply(const char *text, const char *base_dir, SfzRegion *out_regions, int max_regions, int *io_region_count, char *out_default_path, size_t out_default_path_len) {
    size_t len = strlen(text);
    SfzRegion global_opcodes; memset(&global_opcodes, 0, sizeof(global_opcodes));
    SfzRegion group_opcodes; memset(&group_opcodes, 0, sizeof(group_opcodes));
    SfzRegion control_opcodes; memset(&control_opcodes, 0, sizeof(control_opcodes));
    SfzRegion throwaway; memset(&throwaway, 0, sizeof(throwaway));
    SfzRegion current_region; memset(&current_region, 0, sizeof(current_region));
    bool region_open = false;
    SfzRegion *active = &throwaway; // where opcodes seen before ANY header go -- discarded, matches SFZ having no meaning for stray top-level opcodes

    size_t pos = 0;
    for (;;) {
        size_t token_end; bool is_header; char name[64];
        long token_start = sfz_find_next_token(text, pos, len, &token_end, &is_header, name, sizeof(name));

        if (is_header || token_start == SFZ_TOKEN_NOT_FOUND) {
            // Close out whatever region was open before switching context (a new header, or EOF).
            if (region_open && *io_region_count < max_regions) {
                out_regions[(*io_region_count)++] = current_region;
                region_open = false;
            }
        }
        if (token_start == SFZ_TOKEN_NOT_FOUND) break;

        if (is_header) {
            if (strcasecmp(name, "global") == 0) {
                memset(&global_opcodes, 0, sizeof(global_opcodes));
                active = &global_opcodes;
            } else if (strcasecmp(name, "group") == 0 || strcasecmp(name, "master") == 0) {
                group_opcodes = global_opcodes;
                active = &group_opcodes;
            } else if (strcasecmp(name, "region") == 0) {
                current_region = group_opcodes;
                region_open = true;
                active = &current_region;
            } else if (strcasecmp(name, "control") == 0) {
                memset(&control_opcodes, 0, sizeof(control_opcodes));
                active = &control_opcodes;
            } else {
                memset(&throwaway, 0, sizeof(throwaway));
                active = &throwaway;
            }
            pos = token_end;
            continue;
        }

        // Opcode token: name[0..) is the key, token_end is right after '='. The value runs until
        // the NEXT token's own start (or EOF), trimmed of surrounding whitespace.
        size_t value_start = token_end;
        size_t next_end; bool next_is_header; char next_name[64];
        long next_start = sfz_find_next_token(text, token_end, len, &next_end, &next_is_header, next_name, sizeof(next_name));
        size_t value_end = (next_start == SFZ_TOKEN_NOT_FOUND) ? len : (size_t)next_start;
        while (value_start < value_end && isspace((unsigned char)text[value_start])) value_start++;
        size_t trimmed_end = value_end;
        while (trimmed_end > value_start && isspace((unsigned char)text[trimmed_end - 1])) trimmed_end--;
        char value[SAMPLE_PATH_LEN];
        size_t vlen = trimmed_end - value_start; if (vlen >= sizeof(value)) vlen = sizeof(value) - 1;
        memcpy(value, text + value_start, vlen); value[vlen] = '\0';

        if (active == &control_opcodes) {
            if (strcasecmp(name, "default_path") == 0) {
                char normalized[SAMPLE_PATH_LEN]; snprintf(normalized, sizeof(normalized), "%s", value);
                for (char *c = normalized; *c; c++) if (*c == '\\') *c = '/';
                if (normalized[0] == '/') snprintf(out_default_path, out_default_path_len, "%s", normalized);
                else sfz_join_path(out_default_path, out_default_path_len, base_dir, normalized);
            }
        } else {
            sfz_apply_opcode(active, name, value);
            if (strcasecmp(name, "sample") == 0) {
                // Resolved EAGERLY, right here, using THIS file's own base_dir/default_path --
                // not deferred to the caller -- because a region's sample= is usually set at
                // <group>/<global> level and inherited by many regions via the struct-copy
                // above; resolving once here (and inheriting the already-resolved absolute path)
                // is both correct for multi-file libraries (each #include'd file's own regions
                // resolve against ITS OWN directory, not the top-level file's) and cheaper than
                // re-resolving per region. Left as the raw, unresolved value if resolution fails
                // -- that zone's own load simply fails later (logged, slot deactivated) rather than
                // crashing on an unresolved path.
                char resolved[SAMPLE_PATH_LEN];
                if (sfz_resolve_sample_path(value, base_dir, out_default_path[0] ? out_default_path : NULL, resolved, sizeof(resolved))) {
                    snprintf(active->sample, sizeof(active->sample), "%s", resolved);
                }
            }
        }
        pos = token_end; // resume scanning right after '=' -- the value text itself is skipped
                          // over naturally since the next token search always starts past it
    }
}

// "#define $NAME value" -- SFZ's own macro-substitution mechanism, letting a file parameterize
// repeated values (velocity splits, tuning offsets, ...) instead of typing them out per region.
// Scoped to the ONE file that declares it (not threaded into #include'd children, unlike
// out_default_path being explicitly NOT threaded either -- see sfz_parse_file_recursive's own
// comment on that) -- the same "cover the common, single-file case; a cross-file macro is rare
// enough not to be worth the extra plumbing" posture. Without this at all, a file using #define
// would have every opcode referencing "$NAME" fail its own numeric parse (atoi/atof silently
// read 0 from a string starting with '$'), corrupting that opcode instead of just not honoring
// the shorthand.
#define SFZ_MAX_DEFINES 64
typedef struct { char name[SFZ_TEXT_FIELD_LEN]; char value[SFZ_TEXT_FIELD_LEN]; } SfzDefine;

// Substitutes every "$NAME" occurrence in `src` with its #define'd value, returning a freshly
// malloc'd buffer (caller frees) sized to fit exactly -- computed via one length-counting pass
// before the real copy pass, rather than an oversized guess, since macro values can be either
// longer or shorter than their own name. Returns NULL (meaning "nothing to substitute, keep using
// the original buffer unchanged") when define_count is 0, so the overwhelmingly common
// no-#define-at-all file pays zero allocation cost for this feature. Matches longest name first
// (sorted once, up front) so a shorter macro that happens to prefix a longer one (e.g. $VEL
// against $VELOCITY) never wins by accident, and only ever matches a WHOLE "$NAME" token --
// requires the character immediately after the name to not itself continue an identifier.
static char *sfz_substitute_defines(const char *src, size_t src_len, const SfzDefine *defines, int define_count, size_t *out_len) {
    if (define_count <= 0) return NULL;
    SfzDefine sorted[SFZ_MAX_DEFINES];
    memcpy(sorted, defines, sizeof(SfzDefine) * (size_t)define_count);
    for (int i = 1; i < define_count; i++) {
        SfzDefine key = sorted[i]; int j = i - 1;
        while (j >= 0 && strlen(sorted[j].name) < strlen(key.name)) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = key;
    }

    size_t out_size = 0;
    for (size_t i = 0; i < src_len;) {
        int matched = -1;
        if (src[i] == '$') {
            for (int d = 0; d < define_count; d++) {
                size_t nlen = strlen(sorted[d].name);
                if (nlen > 0 && i + nlen <= src_len && strncmp(src + i, sorted[d].name, nlen) == 0) {
                    char next = (i + nlen < src_len) ? src[i + nlen] : '\0';
                    if (!isalnum((unsigned char)next) && next != '_') { matched = d; break; }
                }
            }
        }
        if (matched >= 0) { out_size += strlen(sorted[matched].value); i += strlen(sorted[matched].name); }
        else { out_size++; i++; }
    }

    char *dst = (char *)malloc(out_size + 1);
    if (!dst) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < src_len;) {
        int matched = -1;
        if (src[i] == '$') {
            for (int d = 0; d < define_count; d++) {
                size_t nlen = strlen(sorted[d].name);
                if (nlen > 0 && i + nlen <= src_len && strncmp(src + i, sorted[d].name, nlen) == 0) {
                    char next = (i + nlen < src_len) ? src[i + nlen] : '\0';
                    if (!isalnum((unsigned char)next) && next != '_') { matched = d; break; }
                }
            }
        }
        if (matched >= 0) {
            size_t vlen = strlen(sorted[matched].value);
            memcpy(dst + w, sorted[matched].value, vlen); w += vlen;
            i += strlen(sorted[matched].name);
        } else { dst[w++] = src[i]; i++; }
    }
    dst[w] = '\0';
    *out_len = out_size;
    return dst;
}

// Reads `path`, strips "//" comments and blanks "#include \"...\"" lines (mirrors _parse_sfz's
// own two-pass shape: strip/tokenize first, separately re-scan the raw text for #include lines
// and recurse into each). Cycle guard: `visited_realpaths` is a caller-owned array of
// realpath()-resolved strings; this file is skipped (no new regions) if already present, mirroring
// _parse_sfz's own `_visited` set exactly. `io_region_count` is threaded through every recursive
// call (not a global) so regions from every #include'd file land in the SAME out_regions array,
// continuing to append rather than each recursion starting its own separate count.
static void sfz_parse_file_recursive(const char *path, SfzRegion *out_regions, int max_regions, int *io_region_count, char *out_default_path, size_t out_default_path_len, char (*visited_realpaths)[PATH_MAX], int *visited_count, int max_visited, SfzDefine *defines, int *define_count) {
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) { fprintf(stderr, "[sfz] failed to resolve '%s'\n", path); return; }
    for (int i = 0; i < *visited_count; i++) if (strcmp(visited_realpaths[i], resolved) == 0) return; // cycle
    if (*visited_count >= max_visited) return;
    strcpy(visited_realpaths[*visited_count], resolved);
    (*visited_count)++;

    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[sfz] failed to open '%s': %s\n", path, strerror(errno)); return; }
    fseek(f, 0, SEEK_END); long fsize = ftell(f); fseek(f, 0, SEEK_SET);
    if (fsize < 0) { fclose(f); return; }
    char *buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return; }
    size_t read_n = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    buf[read_n] = '\0';

    char sfz_dir[PATH_MAX];
    snprintf(sfz_dir, sizeof(sfz_dir), "%s", resolved);
    char *last_slash = strrchr(sfz_dir, '/');
    if (last_slash) *last_slash = '\0'; else snprintf(sfz_dir, sizeof(sfz_dir), ".");

    // Strip "//" comments (blank to end of line), collect+blank "#include \"...\"" lines, and
    // collect+blank "#define $NAME value" lines, in one forward pass over the mutable buffer.
    // `defines`/`define_count` are the CALLER's own arrays (see this function's own signature), not
    // fresh per-file locals -- the real SFZ spec treats #include as literal text concatenation (an
    // included file is pasted in at its #include's own position and the whole tree parsed as one
    // document), so a #define appearing before an #include is standard-documented to apply to
    // everything that #include brings in too. A master kit file that #defines each drum's own key
    // macro before #including a generic per-instrument fragment (exactly this kit's own layout)
    // depends on that: this file's #defines are appended onto the SAME growing array the caller
    // already populated, and get passed on unchanged into any #include this file has of its own, so
    // the propagation direction (parent -> child, and via the shared array, sibling -> later
    // sibling) matches a real concatenate-then-parse pass instead of resetting to empty per file.
    #define SFZ_MAX_INCLUDES_PER_FILE 64
    char include_paths[SFZ_MAX_INCLUDES_PER_FILE][SAMPLE_PATH_LEN];
    int include_count = 0;
    for (size_t i = 0; i < read_n; i++) {
        if (buf[i] == '/' && i + 1 < read_n && buf[i + 1] == '/') {
            while (i < read_n && buf[i] != '\n') buf[i++] = ' ';
        } else if (buf[i] == '#' && strncmp(buf + i, "#include", 8) == 0) {
            size_t j = i + 8;
            while (j < read_n && isspace((unsigned char)buf[j])) j++;
            if (j < read_n && buf[j] == '"') {
                size_t start = j + 1, k = start;
                while (k < read_n && buf[k] != '"' && buf[k] != '\n') k++;
                if (k < read_n && buf[k] == '"' && include_count < SFZ_MAX_INCLUDES_PER_FILE) {
                    size_t plen = k - start; if (plen >= SAMPLE_PATH_LEN) plen = SAMPLE_PATH_LEN - 1;
                    memcpy(include_paths[include_count], buf + start, plen);
                    include_paths[include_count][plen] = '\0';
                    include_count++;
                    for (size_t z = i; z <= k && z < read_n; z++) buf[z] = ' '; // blank the whole directive
                    i = k;
                }
            }
        } else if (buf[i] == '#' && strncmp(buf + i, "#define", 7) == 0 && (i + 7 >= read_n || isspace((unsigned char)buf[i + 7]))) {
            size_t line_start = i;
            size_t j = i + 7;
            while (j < read_n && buf[j] != '\n' && isspace((unsigned char)buf[j])) j++;
            if (j < read_n && buf[j] == '$') {
                size_t name_start = j; // kept pointing AT the '$' -- sfz_substitute_defines() matches
                                        // a stored name against "$NAME" text verbatim, so the '$'
                                        // itself must be part of the stored name, not skipped over
                j++; // advance PAST the '$' before scanning identifier characters -- buf[j] here is
                     // '$' itself, which is neither alnum nor '_', so leaving this out made the scan
                     // loop below never advance at all: name_len was always 0 and no #define was
                     // ever actually captured, in any file (this is the real bug behind "why is
                     // every $KEY macro in my SFZ still literally '$KEY' after loading" -- nothing
                     // to do with #include scope, which is a SEPARATE fix elsewhere in this function).
                while (j < read_n && (isalnum((unsigned char)buf[j]) || buf[j] == '_')) j++;
                size_t name_len = j - name_start;
                while (j < read_n && buf[j] != '\n' && isspace((unsigned char)buf[j])) j++;
                size_t value_start = j;
                while (j < read_n && !isspace((unsigned char)buf[j])) j++;
                size_t value_len = j - value_start;
                size_t line_end = j;
                while (line_end < read_n && buf[line_end] != '\n') line_end++;
                if (*define_count < SFZ_MAX_DEFINES && name_len > 0 && name_len < sizeof(defines[0].name) && value_len < sizeof(defines[0].value)) {
                    memcpy(defines[*define_count].name, buf + name_start, name_len); defines[*define_count].name[name_len] = '\0';
                    memcpy(defines[*define_count].value, buf + value_start, value_len); defines[*define_count].value[value_len] = '\0';
                    (*define_count)++;
                }
                for (size_t z = line_start; z < line_end; z++) buf[z] = ' '; // blank the whole directive
                i = line_end;
            }
        }
    }

    size_t expanded_len = 0;
    char *expanded = sfz_substitute_defines(buf, read_n, defines, *define_count, &expanded_len);
    if (expanded) {
        sfz_tokenize_and_apply(expanded, sfz_dir, out_regions, max_regions, io_region_count, out_default_path, out_default_path_len);
        free(expanded);
    } else {
        sfz_tokenize_and_apply(buf, sfz_dir, out_regions, max_regions, io_region_count, out_default_path, out_default_path_len);
    }
    free(buf);

    for (int inc = 0; inc < include_count; inc++) {
        char inc_path[SAMPLE_PATH_LEN];
        if (include_paths[inc][0] == '/') sfz_set_text_field(inc_path, sizeof(inc_path), include_paths[inc]);
        else sfz_join_path(inc_path, sizeof(inc_path), sfz_dir, include_paths[inc]);
        char inc_default_path[SAMPLE_PATH_LEN]; inc_default_path[0] = '\0';
        // Same shared `defines`/`define_count` passed straight through -- see this function's own
        // header comment on why propagating them (rather than each file starting a fresh, empty
        // array) is what real concatenate-then-parse #define/#include scoping requires.
        sfz_parse_file_recursive(inc_path, out_regions, max_regions, io_region_count, inc_default_path, sizeof(inc_default_path), visited_realpaths, visited_count, max_visited, defines, define_count);
        // A sub-file's own default_path= (if any) only applies to ITS OWN regions -- already
        // baked into them via the recursive call above, which passes sfz_dir derived from
        // inc_path's own directory. The outer file's out_default_path is left as whatever the
        // outer file's own <control> block set (or empty), matching real SFZ scoping.
    }
}

// Public entry point: owns/frees the visited-path cycle-guard list itself. Returns the number of
// regions parsed into out_regions (0 on total failure, e.g. the file doesn't open).
int sfz_parse_file(const char *path, SfzRegion *out_regions, int max_regions, char *out_default_path, size_t out_default_path_len) {
    out_default_path[0] = '\0';
    int region_count = 0;
    #define SFZ_MAX_VISITED_FILES 256
    char (*visited)[PATH_MAX] = (char (*)[PATH_MAX])malloc((size_t)SFZ_MAX_VISITED_FILES * PATH_MAX);
    if (!visited) return 0;
    int visited_count = 0;
    // One shared, growing #define table for the WHOLE file tree -- see sfz_parse_file_recursive's
    // own header comment for why this (not a fresh empty array per file) is what the SFZ spec's own
    // #include-as-text-concatenation model requires.
    SfzDefine defines[SFZ_MAX_DEFINES];
    int define_count = 0;
    sfz_parse_file_recursive(path, out_regions, max_regions, &region_count, out_default_path, out_default_path_len, visited, &visited_count, SFZ_MAX_VISITED_FILES, defines, &define_count);
    free(visited);
    return region_count;
}
