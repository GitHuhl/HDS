// test_backend.c -- unit tests for backend.o's plain-function subsystems:
//   - the JSON reader (hds_json_parse/hds_json_object_get/hds_json_free), used for .hdsprog files
//   - the Scala (.scl) microtuning loader (hds_tuning_load_scl -> g_tuning_table)
//   - the OSC address table (build_osc_dispatch_table/osc_dispatch_lookup): every concrete
//     control address, fingerprinted, so any addition/removal/rename in the protocol shows up
// Usage: test_backend <scratch dir>
#include "test_common.h"
#include "backend.h"

#include <stdlib.h>

// ---------------------------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------------------------
static int json_array_len(const HdsJsonValue *v) {
    int n = 0;
    for (const HdsJsonMember *m = v ? v->first_member : NULL; m; m = m->next) n++;
    return n;
}
static const HdsJsonValue *json_array_at(const HdsJsonValue *v, int i) {
    for (const HdsJsonMember *m = v ? v->first_member : NULL; m; m = m->next) if (i-- == 0) return m->value;
    return NULL;
}

static void test_json(void) {
    const char *doc =
        "{ \"name\": \"Lead \\\"A\\\"\\n\", \"volume\": 0.8, \"big\": -2.5e3, \"zero\": 0,\n"
        "  \"flags\": [true, false, null], \"empty_arr\": [], \"empty_obj\": {},\n"
        "  \"nested\": { \"osc\": [ { \"wave\": 3 }, { \"wave\": 1 } ] } }";
    HdsJsonValue *v = hds_json_parse(doc);
    test_check(v && v->type == HDS_JSON_OBJECT, "backend/json/parse_object", "document did not parse as an object");
    if (!v) return;
    const HdsJsonValue *name = hds_json_object_get(v, "name");
    test_check(name && name->type == HDS_JSON_STRING && strcmp(name->string, "Lead \"A\"\n") == 0,
               "backend/json/string_escapes", "got '%s'", name && name->string ? name->string : "(null)");
    const HdsJsonValue *vol = hds_json_object_get(v, "volume"), *big = hds_json_object_get(v, "big"), *zero = hds_json_object_get(v, "zero");
    test_check(vol && vol->type == HDS_JSON_NUMBER && vol->number == 0.8 && big && big->number == -2500.0 && zero && zero->number == 0.0,
               "backend/json/numbers", "volume/big/zero parsed wrong");
    const HdsJsonValue *flags = hds_json_object_get(v, "flags");
    test_check(flags && flags->type == HDS_JSON_ARRAY && json_array_len(flags) == 3 &&
               json_array_at(flags, 0)->type == HDS_JSON_BOOL && json_array_at(flags, 0)->boolean &&
               json_array_at(flags, 1)->type == HDS_JSON_BOOL && !json_array_at(flags, 1)->boolean &&
               json_array_at(flags, 2)->type == HDS_JSON_NULL, "backend/json/array_bool_null", "flags array parsed wrong");
    const HdsJsonValue *ea = hds_json_object_get(v, "empty_arr"), *eo = hds_json_object_get(v, "empty_obj");
    test_check(ea && ea->type == HDS_JSON_ARRAY && json_array_len(ea) == 0 && eo && eo->type == HDS_JSON_OBJECT && !eo->first_member,
               "backend/json/empty_containers", "empty array/object parsed wrong");
    const HdsJsonValue *osc = hds_json_object_get(hds_json_object_get(v, "nested"), "osc");
    const HdsJsonValue *w1 = json_array_at(osc, 1) ? hds_json_object_get(json_array_at(osc, 1), "wave") : NULL;
    test_check(json_array_len(osc) == 2 && w1 && w1->number == 1.0, "backend/json/nested", "nested object/array lookup failed");
    test_check(hds_json_object_get(v, "missing") == NULL, "backend/json/missing_key", "lookup of a missing key returned a value");
    hds_json_free(v);

    const char *bad[] = { "", "{", "{\"a\" 1}", "{\"a\": }", "[1, 2", "\"unterminated", "nul", "{\"a\": tru}" };
    int accepted = 0;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        HdsJsonValue *b = hds_json_parse(bad[i]);
        if (b) { accepted++; printf("     (malformed input accepted: '%s')\n", bad[i]); hds_json_free(b); }
    }
    test_check(accepted == 0, "backend/json/rejects_malformed", "%d malformed documents parsed", accepted);
}

// ---------------------------------------------------------------------------------------------
// Scala tuning files
// ---------------------------------------------------------------------------------------------
static const char *write_file(const char *dir, const char *name, const char *text) {
    static char path[4][512]; static int k = 0;
    char *p = path[k++ & 3];
    snprintf(p, 512, "%s/%s", dir, name);
    FILE *f = fopen(p, "wb"); fputs(text, f); fclose(f);
    return p;
}

static void test_scl(const char *dir) {
    // 12-TET in cents, with comments, a CRLF line ending and indented pitch lines
    char tet[2048] = "! 12tet.scl\r\n!\n12-tone equal temperament\n 12\n!\n";
    for (int i = 1; i <= 12; i++) { char l[32]; snprintf(l, sizeof l, " %d.0\n", i * 100); strcat(tet, l); }
    bool ok = hds_tuning_load_scl(write_file(dir, "12tet.scl", tet));
    float worst = 0;
    for (int i = 0; ok && i < 12; i++) { float e = fabsf(g_tuning_table.degree_ratio[i] - powf(2.0f, (i + 1) / 12.0f)); if (e > worst) worst = e; }
    test_check(ok && g_tuning_table.loaded && g_tuning_table.degree_count == 12 && worst < 1e-5f &&
               strcmp(g_tuning_table.description, "12-tone equal temperament") == 0,
               "backend/scl/cents_12tet", "load=%d count=%d worst ratio error %g desc='%s'", ok, g_tuning_table.degree_count, worst, g_tuning_table.description);

    // Just intonation pentatonic in ratios, including a bare integer period
    ok = hds_tuning_load_scl(write_file(dir, "ji5.scl", "JI pentatonic\n5\n9/8\n5/4\n3/2\n5/3\n2\n"));
    const float want[5] = { 9.0f / 8, 5.0f / 4, 3.0f / 2, 5.0f / 3, 2.0f };
    int match = ok && g_tuning_table.degree_count == 5;
    for (int i = 0; match && i < 5; i++) match = fabsf(g_tuning_table.degree_ratio[i] - want[i]) < 1e-6f;
    test_check(match, "backend/scl/ratios", "ratio-format scale loaded wrong (load=%d count=%d)", ok, g_tuning_table.degree_count);

    // Malformed files must be rejected and leave the loaded table untouched
    HdsTuningTable before = g_tuning_table;
    const char *bad[][2] = {
        { "zero.scl", "zero degrees\n0\n" },
        { "truncated.scl", "promises three\n3\n100.0\n200.0\n" },
        { "garbage.scl", "bad pitch\n2\nabc\n2/1\n" },
        { "negative.scl", "negative ratio\n1\n-3/2\n" },
        { "nocount.scl", "only a description\n" },
    };
    int accepted = 0;
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++)
        if (hds_tuning_load_scl(write_file(dir, bad[i][0], bad[i][1]))) { accepted++; printf("     (malformed .scl accepted: %s)\n", bad[i][0]); }
    test_check(accepted == 0, "backend/scl/rejects_malformed", "%d malformed files loaded", accepted);
    test_check(memcmp(&before, &g_tuning_table, sizeof before) == 0, "backend/scl/reject_keeps_table", "a rejected file changed the loaded table");
    test_check(!hds_tuning_load_scl("/nonexistent/dir/x.scl"), "backend/scl/missing_file", "a missing file loaded");
}

// ---------------------------------------------------------------------------------------------
// OSC address table
// ---------------------------------------------------------------------------------------------
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void test_osc_table(void) {
    assert_fm_algorithms_acyclic();
    build_osc_dispatch_table();
    static const char *addrs[OSC_DISPATCH_TABLE_SIZE];
    int n = 0, strings = 0;
    for (int i = 0; i < OSC_DISPATCH_TABLE_SIZE; i++) {
        if (!osc_dispatch_table[i].occupied) continue;
        addrs[n++] = osc_dispatch_table[i].address;
        if (osc_dispatch_table[i].kind == OSC_KIND_STRING) strings++;
    }
    qsort(addrs, (size_t)n, sizeof addrs[0], cmp_str);
    int dups = 0, bad_lookup = 0;
    for (int i = 0; i < n; i++) {
        if (i && strcmp(addrs[i], addrs[i - 1]) == 0) dups++;
        const OscDispatchSlot *s = osc_dispatch_lookup(addrs[i]);
        if (!s || strcmp(s->address, addrs[i]) != 0) bad_lookup++;
    }
    printf("     (%d OSC control addresses, %d of them strings)\n", n, strings);
    test_check(n > 1000, "backend/osc/table_size", "only %d addresses", n);
    test_check(dups == 0, "backend/osc/unique", "%d duplicate addresses", dups);
    test_check(bad_lookup == 0, "backend/osc/lookup_all", "%d addresses don't look up to themselves", bad_lookup);
    test_check(osc_dispatch_lookup("/hds/tuning/enable") != NULL && osc_dispatch_lookup("/hds/no/such/address") == NULL &&
               osc_dispatch_lookup("/hds/tuning/enabl") == NULL, "backend/osc/lookup_known_unknown", "known/unknown address lookup wrong");
    TestHash h; test_hash_init(&h);
    for (int i = 0; i < n; i++) {
        const OscDispatchSlot *s = osc_dispatch_lookup(addrs[i]);
        test_hash_bytes(&h, addrs[i], strlen(addrs[i]) + 1);
        test_hash_bytes(&h, &s->kind, sizeof s->kind);
    }
    test_hash_print(&h, "backend/osc/address_space");
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    test_json();
    test_scl(dir);
    test_osc_table();
    return test_exit_code();
}
