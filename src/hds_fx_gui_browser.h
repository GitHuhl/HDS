// hds_fx_gui_browser.h -- the in-house "pick a file/folder" browser's data model (HdsFxBrowser),
// shared by hds_fx_gui_draw.c (which draws it and handles clicks, with no OS calls) and
// hds_fx_gui_browser.c (the one place that lists directories). Compiled as its own object and
// linked into hds.clap, hds_fx.clap and synth_engine.
#ifndef HDS_FX_GUI_BROWSER_H
#define HDS_FX_GUI_BROWSER_H

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "hds_config.h" // SAMPLE_PATH_LEN

// A plain bounded strcpy -- used in place of snprintf(dst, n, "%s", src) wherever src's own
// declared buffer is bigger than dst's (e.g. a joined "dir/name" that could in principle exceed
// SAMPLE_PATH_LEN for a sufficiently deep/long real path): correctly truncating and NUL-terminating
// either way, just without tripping GCC's -Wformat-truncation, which can't see that this is an
// intentional, already-safe cap rather than an overlooked bug.
static inline void hds_fx_safe_copy(char *dst, size_t dst_size, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

// In-house file browser for FX_TYPE_NAM's Model/IR pills -- a real directory listing drawn with
// this file's own Cairo primitives, not a subprocess (zenity/kdialog) and not a GTK/Qt widget. This
// replaced an earlier revision that shelled out to zenity/kdialog: that approach depended on
// whichever dialog tool (if any) happened to be installed and reachable from inside the HOST's own
// process, and real DAW testing found it silently unresponsive on more than one real system (a
// tool that's on PATH but can't actually show a picker there for reasons this plugin has no way to
// diagnose remotely -- sandboxing, a missing display/DBus portal, etc.) even after hardening the
// failure-recovery heuristics twice. An in-house browser has no external dependency to fail: it's
// exactly the same opendir()/readdir() any other program on the SAME machine already needs to list
// a directory, drawn with the exact same Cairo calls every other pixel in this GUI already uses --
// works identically on every Linux desktop regardless of what's installed, and (unlike zenity/
// kdialog, which don't exist on Windows/macOS at all) is the genuinely portable path to this file's
// own already-stated future-Windows-port goal: a win32 platform shim would only need its own
// FindFirstFile/FindNextFile-based refresh function, this entire struct and every render/hit-test
// function around it stay untouched.
//
// This file (the portable draw file) owns the STATE and all rendering/hit-testing; it never touches
// the filesystem directly (see this file's own header comment on staying free of any OS call, the
// same discipline that already kept Xlib/Win32 types out). hds_fx_gui_x11.c's own
// hds_fx_gui_browser_refresh() is the one place that actually calls opendir()/readdir()/stat() --
// triggered whenever needs_refresh is set here (opening the browser, navigating into a directory,
// pressing Up, or committing a typed path) and polled once per timer tick, same "never block the
// UI thread on a filesystem call from inside a click handler" discipline the old subprocess
// approach already established (a network filesystem's own realistic latency is exactly the kind
// of stall a single click shouldn't have to wait through synchronously).
#define HDS_FX_BROWSER_MAX_ENTRIES 256
#define HDS_FX_BROWSER_NAME_LEN 256

typedef struct {
    bool open;
    // Generic: which extension to filter/match (e.g. ".nam", ".wav", ".sfz", ".scl") and the
    // caller-owned destination buffer the picked path gets written into on commit -- set at open
    // time (hds_fx_gui_browser_open()), so this ONE modal instance can serve every "pick an
    // existing file" need in the whole GUI (NAM Model/IR, a Play-tab oscillator's own .sfz, the
    // Global sidebar's own .scl, ...) rather than each caller needing its own copy of this widget.
    // `dest` must outlive this browser's own open state -- true for every real caller, since it's
    // always a field on some persistent struct (FXSlotParams, HdsClapPlugin, ...), never a stack
    // buffer.
    char ext[8];
    char title[64]; // header text, e.g. "Select NAM Model (*.nam)" -- set at open time too
    char *dest;
    size_t dest_len;
    // Folder-picking mode (hds_fx_gui_browser_open_folder()): lists only subdirectories (no
    // extension filtering at all) and adds a "Choose This Folder" button that commits the CURRENTLY
    // listed directory (b->dir itself) rather than any one entry -- the Recording tab's own output
    // folder is the one thing in this GUI that needs an existing DIRECTORY picked, not a file.
    bool folder_mode;

    // dir is the authoritative current directory while NOT editing the path bar; WHILE editing
    // (path_bar_editing) it doubles as the live typed buffer instead (same "one buffer serves both
    // display and edit" the old edit_buffer used) -- last_valid_dir is what Escape reverts to.
    char dir[SAMPLE_PATH_LEN];
    char last_valid_dir[SAMPLE_PATH_LEN];
    bool path_bar_editing;
    // Set by Enter on the path bar: the platform shim tries the typed text as an exact FILE path
    // first (stat()+S_ISREG -- committing it directly, same as clicking a file entry), then falls
    // back to trying it as a directory (same as clicking a directory entry) if that fails.
    bool path_bar_commit_attempt;
    bool needs_refresh; // platform shim sees this, re-lists `dir`, fills entries below, clears it

    char entry_name[HDS_FX_BROWSER_MAX_ENTRIES][HDS_FX_BROWSER_NAME_LEN];
    bool entry_is_dir[HDS_FX_BROWSER_MAX_ENTRIES];
    int entry_count;
    int scroll; // index of the first visible entry row
} HdsFxBrowser;

// Re-lists b->dir into b->entry_* (filtered by b->ext, or directories only in folder mode), and
// resolves a pending path-bar commit. Called from the platform shim's timer tick when
// b->needs_refresh or b->path_bar_commit_attempt is set; never from a click handler.
void hds_fx_gui_browser_refresh(HdsFxBrowser *b);

#endif // HDS_FX_GUI_BROWSER_H
