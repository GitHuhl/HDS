// Dead-on-purpose stand-in for liblo, for the Android build ONLY.
//
// backend.c #includes <lo/lo.h> directly for its OSC control protocol (34 handler functions, ~70
// call sites -- see this project's own README "Control protocol: OSC" section). None of that is
// reachable from hds_android.c: nothing there ever calls lo_server_new()/lo_server_thread_new() or
// registers a single one of those handlers with a real server, so every lo_* symbol backend.c
// references only needs to TYPE-CHECK and LINK here, never actually DO anything. This header gives
// it exactly that: real liblo's type/function shapes (so backend.c compiles completely unmodified,
// byte-identical to the Linux/Windows builds), bodies that are all inert no-ops.
//
// This is the one variable pulled out of the Android build for now, on purpose (cross-compiling
// the real liblo for android-arm64, wiring it into CMakeLists.txt, and actually starting an OSC
// server from hds_android.c is real but SEPARATE follow-up work, once basic audio+MIDI is verified
// on a real phone) -- not a permanent design decision. To turn real OSC on later: cross-build
// liblo for android-arm64, point CMakeLists.txt's include/link at that instead of this directory,
// and nothing in backend.c or hds_android.c needs to change at all -- the real lo_send()/
// lo_message_new() etc. just start working instead of no-opping.
#ifndef HDS_ANDROID_LO_STUB_H
#define HDS_ANDROID_LO_STUB_H

#include <stdint.h>
#include <stddef.h>

#define LO_TT_IMMEDIATE ((uint64_t)0)

typedef void *lo_address;
typedef void *lo_message;
typedef void *lo_bundle;

// Field names/types match real liblo's lo_arg union closely enough for backend.c's handler bodies
// to compile (argv[n]->f, ->i, ->s, ->blob.size/.data -- see this project's own grep of every field
// access) -- exact bit layout is irrelevant since nothing here is ever actually populated from a
// real OSC packet; see this file's own header comment for why.
typedef union {
    int32_t i;
    int32_t i32;
    float f;
    double d;
    unsigned char c;
    char s[4];
    struct { int32_t size; unsigned char data[4]; } blob;
} lo_arg;

static inline lo_message lo_message_new(void) { return NULL; }
static inline void lo_message_add_float(lo_message m, float f) { (void)m; (void)f; }
static inline void lo_message_add_string(lo_message m, const char *s) { (void)m; (void)s; }
static inline lo_address lo_message_get_source(lo_message m) { (void)m; return NULL; }

static inline lo_bundle lo_bundle_new(uint64_t tt) { (void)tt; return NULL; }
static inline void lo_bundle_add_message(lo_bundle b, const char *path, lo_message m) { (void)b; (void)path; (void)m; }
static inline void lo_bundle_free_recursive(lo_bundle b) { (void)b; }
static inline int lo_send_bundle(lo_address targ, lo_bundle b) { (void)targ; (void)b; return 0; }

static inline int lo_send(lo_address targ, const char *path, const char *types, ...) {
    (void)targ; (void)path; (void)types;
    return 0;
}

static inline lo_address lo_address_new(const char *host, const char *port) { (void)host; (void)port; return NULL; }
static inline void lo_address_free(lo_address a) { (void)a; }
static inline const char *lo_address_get_hostname(lo_address a) { (void)a; return ""; }
static inline const char *lo_address_get_port(lo_address a) { (void)a; return ""; }

#endif
