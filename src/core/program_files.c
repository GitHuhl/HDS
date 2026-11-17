// program_files.c -- the ~/HDS_Programs/ folder scanner (hds_scan_programs_folder) for saved .hdsprog Programs.
// Split verbatim out of backend.c.
#include "core/backend.h"
#include "core/sample_engine.h"
#include "core/engine_internal.h"
#include "core/hds_compat.h" // NAME_MAX on Windows

static int hds_qsort_strcmp(const void *a, const void *b) {
    return strcmp(*(const char * const *)a, *(const char * const *)b);
}

// Sorted list of saved Program names -- one per ~/HDS_Programs/<name>/<name>.hdsprog subfolder, in
// alphabetical order, so hds_standalone.c's own arrow-key Program cycling and hds.clap's own native
// Combi-lane Program picker (hds_clap_combi_draw.c) both see the identical list. Caller owns the
// returned array (and each of its strings) via free(); returns NULL (out_count 0) if
// ~/HDS_Programs doesn't exist or has no saved Programs.
char **hds_scan_programs_folder(int *out_count) {
    *out_count = 0;
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char root[SAMPLE_PATH_LEN];
    snprintf(root, sizeof(root), "%s/HDS_Programs", home);
    DIR *d = opendir(root);
    if (!d) return NULL;
    char **names = NULL;
    int count = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; // skip "."/".."/hidden
        char check_path[SAMPLE_PATH_LEN + 2 * (NAME_MAX + 1) + 16]; // root + "/" + d_name + "/" + d_name + ".hdsprog"
        snprintf(check_path, sizeof(check_path), "%s/%s/%s.hdsprog", root, ent->d_name, ent->d_name);
        FILE *f = fopen(check_path, "r");
        if (!f) continue; // not a valid Program subfolder -- a matching .hdsprog is the whole test
        fclose(f);
        if (count >= cap) {
            cap = cap ? cap * 2 : 8;
            char **grown = realloc(names, sizeof(char *) * (size_t)cap);
            if (!grown) break; // allocation failure -- keep whatever's already collected
            names = grown;
        }
        names[count++] = strdup(ent->d_name);
    }
    closedir(d);
    if (count == 0) { free(names); return NULL; }
    qsort(names, (size_t)count, sizeof(char *), hds_qsort_strcmp);
    *out_count = count;
    return names;
}
