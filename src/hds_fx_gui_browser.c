// hds_fx_gui_browser.c: the actual filesystem-touching half of the in-house, generic "pick an
// existing file with one extension" file browser (opendir/readdir/stat) -- see HdsFxBrowser's own
// comment in hds_fx_gui_draw.c for why an in-house browser replaced an earlier zenity/kdialog
// subprocess approach, and for why it's generic (ext/dest set per-open, not fixed to NAM's own
// Model/IR pair) rather than a whole separate copy of this widget per caller. hds_fx_gui_draw.c only
// ever reads the plain-data results this fills in, keeping it free of any OS call; this is the ONE
// place that makes real filesystem calls. Pulled into its own file (rather than living inside
// hds_fx_gui_x11.c, where it originated) specifically so hds_clap_gui_x11.c can reuse it verbatim
// too, without either platform shim's own window/event-pump code needing to be shared -- see each
// platform file's own header comment on why those stay separate, small copies.
//
// #include'd right after hds_fx_gui_draw.c (needs HdsFxBrowser/HDS_FX_BROWSER_*/hds_fx_safe_copy,
// all defined there).
#include <dirent.h>   // opendir/readdir/closedir
#include <sys/stat.h> // stat/S_ISDIR/S_ISREG
#include <strings.h>  // strcasecmp -- case-insensitive *.nam/*.wav filtering and sorting
#include <stdlib.h>   // getenv/qsort

typedef struct { char name[HDS_FX_BROWSER_NAME_LEN]; bool is_dir; } HdsFxBrowserSortEntry;

static int hds_fx_browser_sort_cmp(const void *a, const void *b) {
    const HdsFxBrowserSortEntry *ea = (const HdsFxBrowserSortEntry *)a;
    const HdsFxBrowserSortEntry *eb = (const HdsFxBrowserSortEntry *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1; // directories before files
    return strcasecmp(ea->name, eb->name);
}

// Case-insensitive "does this filename end in this extension" check -- the ONE place this
// decision is made, shared by the directory listing below AND the path bar's own commit-attempt
// (see hds_fx_gui_browser_refresh()'s own comment on path_bar_commit_attempt): without this, the
// listing was already filtered to only ever show a *.nam/*.wav file, but typing or pasting an
// exact path into the path bar and pressing Enter bypassed that filter entirely -- any existing
// regular file, any extension (or none), got accepted as if it had been a real pick from the
// list. Requiring the same extension match there too means the path bar can never be used to load
// something the browser itself would never have shown in the first place.
static bool hds_fx_path_has_ext(const char *name, const char *ext) {
    size_t nlen = strlen(name), elen = strlen(ext);
    return nlen >= elen && strcasecmp(name + nlen - elen, ext) == 0;
}

// Called every timer tick (a no-op unless needs_refresh is set -- opening the browser, navigating
// into a directory or Up, or committing a typed path all set it, see hds_fx_gui_draw.c's own
// browser functions). Never blocks the UI thread on anything slow (a real directory read is fast
// on local disk, but this same non-blocking-poll shape is what would let a future version move the
// actual I/O to a worker thread for a slow/network path without touching any caller).
// Takes just the browser (fully self-contained since HdsFxBrowser generalized to ext/dest -- see
// its own comment; no host lookup needed anymore) so it's reusable by any platform shim with an
// HdsFxBrowser lying around, regardless of how deep in its own view-state nesting that browser
// lives (hds_fx.clap's own window keeps it at the top level; hds.clap's own nests it one level
// down, inside its own FX-tab sub-state, yet reuses this SAME instance from its Play/Global tabs
// too -- see hds_clap_gui_draw.c's own header comment).
static void hds_fx_gui_browser_refresh(HdsFxBrowser *b) {
    if (!b->open || !b->needs_refresh) return;
    b->needs_refresh = false;

    // Enter was pressed on the path bar: try the typed text as an exact FILE path first (same
    // effect as clicking a file entry), before ever trying it as a directory to list. Requires the
    // same extension match the listing itself already enforces (hds_fx_path_has_ext()'s own
    // comment) -- an existing regular file with the WRONG extension (or none) is deliberately left
    // to fall through to the directory-listing attempt below, which will then simply fail to
    // opendir() it and fall back to home/"/", same as any other bad typed path.
    if (b->path_bar_commit_attempt) {
        b->path_bar_commit_attempt = false;
        struct stat st;
        if (hds_fx_path_has_ext(b->dir, b->ext) && stat(b->dir, &st) == 0 && S_ISREG(st.st_mode)) {
            hds_fx_safe_copy(b->dest, b->dest_len, b->dir);
            b->open = false;
            return;
        }
    }

    DIR *d = b->dir[0] ? opendir(b->dir) : NULL;
    if (!d) {
        // The requested directory doesn't exist/isn't readable (or was never set at all, e.g. the
        // very first open with no existing path to derive one from) -- fall back to a sane
        // default rather than showing a dead end with no way to navigate anywhere.
        const char *home = getenv("HOME");
        if (home && (d = opendir(home))) snprintf(b->dir, sizeof(b->dir), "%s", home);
        else if ((d = opendir("/"))) snprintf(b->dir, sizeof(b->dir), "/");
    }
    b->entry_count = 0;
    b->scroll = 0;
    if (!d) return; // nothing listable at all -- the path bar and Close are still available

    HdsFxBrowserSortEntry sort_buf[HDS_FX_BROWSER_MAX_ENTRIES];
    int n = 0;
    struct dirent *de;
    while (n < HDS_FX_BROWSER_MAX_ENTRIES && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (de->d_name[0] == '.') continue; // skip hidden entries
        char full[SAMPLE_PATH_LEN + HDS_FX_BROWSER_NAME_LEN + 2];
        if (strcmp(b->dir, "/") == 0) snprintf(full, sizeof(full), "/%s", de->d_name);
        else snprintf(full, sizeof(full), "%s/%s", b->dir, de->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue; // e.g. a broken symlink -- just skip it
        bool is_dir = S_ISDIR(st.st_mode);
        if (!is_dir && !hds_fx_path_has_ext(de->d_name, b->ext)) continue; // filtered out
        snprintf(sort_buf[n].name, sizeof(sort_buf[n].name), "%s", de->d_name);
        sort_buf[n].is_dir = is_dir;
        n++;
    }
    closedir(d);
    qsort(sort_buf, (size_t)n, sizeof(sort_buf[0]), hds_fx_browser_sort_cmp);
    for (int i = 0; i < n; i++) {
        hds_fx_safe_copy(b->entry_name[i], sizeof(b->entry_name[i]), sort_buf[i].name);
        b->entry_is_dir[i] = sort_buf[i].is_dir;
    }
    b->entry_count = n;
    snprintf(b->last_valid_dir, sizeof(b->last_valid_dir), "%s", b->dir);
}
