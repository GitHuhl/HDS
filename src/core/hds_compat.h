// hds_compat.h -- small POSIX gaps on Windows (mingw-w64), shared by backend.c and the SFZ parser.
#ifndef HDS_COMPAT_H
#define HDS_COMPAT_H

#include <limits.h>

#ifdef _WIN32
// Two small POSIX-portability gaps, both only reachable through the native SFZ parser (Windows CLAP
// builds -- see the Makefile's own clap-windows/hds-fx-windows section): mingw-w64's own limits.h
// only defines NAME_MAX under an #ifdef _POSIX_ this codebase never sets (PATH_MAX is defined
// unconditionally there already, so only NAME_MAX needs a fallback -- verified directly against a
// real mingw-w64 cross-compile, not assumed), and mingw-w64 ships no realpath() at all (Windows has
// no direct equivalent). GetFullPathNameA() gives the same "one canonical absolute path string"
// property sfz_parse_file_recursive()'s own #include-cycle guard actually needs (mixed '/'/'\\'
// separators and '.'/'..' segments collapse to the same string either way), just without
// realpath()'s own symlink resolution -- a real, accepted Windows-only gap (a cycle through two
// DIFFERENT symlinks to the same file won't be caught there), not worth a bigger fix for what is
// otherwise a cheap infinite-recursion guard.
#include <windows.h>
#ifndef NAME_MAX
#define NAME_MAX 255
#endif
static inline char *hds_realpath_compat(const char *path, char *resolved) {
    DWORD n = GetFullPathNameA(path, PATH_MAX, resolved, NULL);
    return (n > 0 && n < PATH_MAX) ? resolved : NULL;
}
#define realpath(path, resolved) hds_realpath_compat(path, resolved)
#endif

#endif // HDS_COMPAT_H
