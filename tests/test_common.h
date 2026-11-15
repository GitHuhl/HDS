// test_common.h -- tiny helpers shared by the HDS test programs (tests/*.c).
//
// Every test program prints one line per check:
//   "ok   <name>"                  -- a pass/fail check that passed
//   "FAIL <name>: <reason>"        -- a pass/fail check that failed (the program exits nonzero)
//   "hash <name> <16 hex digits>"  -- a regression fingerprint, compared against tests/golden.txt
//                                    by tests/run_tests.sh
#ifndef HDS_TEST_COMMON_H
#define HDS_TEST_COMMON_H

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_test_failures = 0;

static inline void test_check(int cond, const char *name, const char *fmt, ...) {
    if (cond) { printf("ok   %s\n", name); return; }
    g_test_failures++;
    printf("FAIL %s: ", name);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
}

// FNV-1a, 64-bit -- order-sensitive, fast, no dependencies. Hashes raw bytes, so float output is
// fingerprinted bit-exactly.
typedef struct { uint64_t h; } TestHash;
static inline void test_hash_init(TestHash *t) { t->h = 1469598103934665603ULL; }
static inline void test_hash_bytes(TestHash *t, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; i++) { t->h ^= p[i]; t->h *= 1099511628211ULL; }
}
static inline void test_hash_print(const TestHash *t, const char *name) {
    printf("hash %s %016llx\n", name, (unsigned long long)t->h);
}

// Deterministic noise source (LCG), independent of libc rand().
static uint32_t test_rng_state = 12345u;
static inline void test_rng_seed(uint32_t s) { test_rng_state = s; }
static inline float test_frand(void) { test_rng_state = test_rng_state * 1664525u + 1013904223u; return (float)(test_rng_state >> 8) / 16777216.0f; }

static inline int test_exit_code(void) { return g_test_failures ? 1 : 0; }

#endif // HDS_TEST_COMMON_H
