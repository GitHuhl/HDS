// tuning.c -- Scala (.scl) microtuning import into g_tuning_table; hds_note_to_hz() in backend.c reads it.
// Moved verbatim out of backend.c.
#include "../backend.h"

// --- Microtuning (Scala .scl import -- see SynthSharedControls.tuning_enable's own comment) ---
HdsTuningTable g_tuning_table;

// Parses one .scl pitch line into a ratio. Returns false (leaving *out_ratio untouched) if the
// line has no parseable number at all -- a blank/malformed line the caller should skip rather
// than insert a garbage degree.
static bool hds_parse_scl_pitch_line(const char *line, float *out_ratio) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '!') return false; // blank or comment
    if (strchr(line, '.')) {
        *out_ratio = powf(2.0f, (float)(atof(line) / 1200.0)); // cents
        return true;
    }
    long numerator = 0, denominator = 1; // denominator's own default covers a bare integer line ("2" means 2/1)
    if (sscanf(line, "%ld / %ld", &numerator, &denominator) >= 1) {
        if (denominator == 0) return false;
        *out_ratio = (float)numerator / (float)denominator;
        return true;
    }
    return false;
}

// Reads a Scala .scl file directly (backend-native, matching --sfz-vcoN/zone_load_sfz's own "OSC
// carries just a path, the backend does the file I/O" shape) -- replaces the currently-loaded
// scale wholesale on success; a malformed/truncated file is rejected with NOTHING changed (same
// "no partial application" posture as hds_song_load_midi). Lives here (not in hds_standalone.c,
// where it originated) rather than in hds_standalone.c specifically so hds_clap_global_draw.c's own
// "Load Scale (.scl)..." button can call it too -- it has no standalone-specific dependency at all,
// just plain FILE I/O and a write into g_tuning_table just above, which both hosts already share.
bool hds_tuning_load_scl(const char *file_path) {
    FILE *f = fopen(file_path, "r");
    if (!f) return false;
    char line[256];
    char description[256] = {0}; // matches line's own size (only the first 127 chars actually survive into g_tuning_table.description's own smaller buffer at the end)
    int degree_count = -1; // -1 = "count line not seen yet"
    float ratios[HDS_TUNING_MAX_DEGREES];
    int parsed = 0;
    bool have_description = false;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0'; // tolerate CRLF files
        char *trimmed = line; while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        if (*trimmed == '!' || *trimmed == '\0') continue; // comment or blank -- never counts toward description/count/pitch lines
        if (!have_description) {
            snprintf(description, sizeof(description), "%s", trimmed);
            have_description = true;
        } else if (degree_count < 0) {
            degree_count = atoi(trimmed);
            if (degree_count <= 0 || degree_count > HDS_TUNING_MAX_DEGREES) { fclose(f); return false; }
        } else if (parsed < degree_count) {
            float ratio;
            if (!hds_parse_scl_pitch_line(trimmed, &ratio) || ratio <= 0.0f) { fclose(f); return false; }
            ratios[parsed++] = ratio;
        }
    }
    fclose(f);
    if (degree_count < 0 || parsed != degree_count) return false; // truncated -- fewer pitch lines than the count line promised
    g_tuning_table.degree_count = degree_count;
    memcpy(g_tuning_table.degree_ratio, ratios, sizeof(float) * (size_t)degree_count);
    // %.127s (not a bare %s): description's own local buffer is 256 bytes (matching line[]) while
    // g_tuning_table.description is a smaller, deliberately compact 128 -- an explicit precision
    // matching that destination size lets the compiler verify no truncation risk regardless of
    // optimization level, rather than relying on it proving that some other way.
    snprintf(g_tuning_table.description, sizeof(g_tuning_table.description), "%.127s", description);
    g_tuning_table.loaded = true;
    return true;
}
