// json.c -- Minimal JSON reader (hds_json_parse/hds_json_object_get/hds_json_free), used for .hdsprog Program files.
// Moved verbatim out of backend.c.
#include "../backend.h"

// --- Minimal JSON reader, for --program (hds_standalone.c's own hds_load_program_file()) AND

void hds_json_free(HdsJsonValue *v) {
    if (!v) return;
    for (HdsJsonMember *m = v->first_member; m; ) {
        HdsJsonMember *next = m->next;
        free(m->key);
        hds_json_free(m->value);
        free(m);
        m = next;
    }
    free(v->string);
    free(v);
}

static void hds_json_skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}

// Parses one JSON string literal (the opening quote must already be consumed by the caller) into a
// freshly malloc'd, NUL-terminated C string, decoding the standard backslash escapes (\uXXXX only
// as far as a plain UTF-8 passthrough of the code point below U+0800 -- every address/value this
// file actually reads is plain ASCII, e.g. a file path or an OSC address, so this stays deliberately
// simple rather than a full UTF-16-surrogate-pair-aware decoder). Returns NULL on a malformed/
// unterminated string.
static char *hds_json_parse_string_body(const char **p) {
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    while (**p != '"') {
        if (**p == '\0') { free(out); return NULL; }
        char c = **p;
        if (c == '\\') {
            (*p)++;
            switch (**p) {
                case '"': c = '"'; break;
                case '\\': c = '\\'; break;
                case '/': c = '/'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case 'n': c = '\n'; break;
                case 'r': c = '\r'; break;
                case 't': c = '\t'; break;
                case 'u': {
                    if (!isxdigit((unsigned char)(*p)[1]) || !isxdigit((unsigned char)(*p)[2]) ||
                        !isxdigit((unsigned char)(*p)[3]) || !isxdigit((unsigned char)(*p)[4])) { free(out); return NULL; }
                    char hex[5] = { (*p)[1], (*p)[2], (*p)[3], (*p)[4], '\0' };
                    long code = strtol(hex, NULL, 16);
                    *p += 4;
                    c = (code >= 0x20 && code < 0x7f) ? (char)code : '?'; // ASCII passthrough, see comment above
                    break;
                }
                default: free(out); return NULL;
            }
        }
        if (len + 1 >= cap) { cap *= 2; char *grown = realloc(out, cap); if (!grown) { free(out); return NULL; } out = grown; }
        out[len++] = c;
        (*p)++;
    }
    (*p)++; // closing quote
    out[len] = '\0';
    return out;
}

static HdsJsonValue *hds_json_parse_value(const char **p);

static HdsJsonValue *hds_json_new_value(HdsJsonType type) {
    HdsJsonValue *v = calloc(1, sizeof(HdsJsonValue));
    if (v) v->type = type;
    return v;
}

static HdsJsonValue *hds_json_parse_object(const char **p) {
    HdsJsonValue *obj = hds_json_new_value(HDS_JSON_OBJECT);
    if (!obj) return NULL;
    (*p)++; // '{'
    hds_json_skip_ws(p);
    if (**p == '}') { (*p)++; return obj; }
    HdsJsonMember **next_slot = &obj->first_member;
    for (;;) {
        hds_json_skip_ws(p);
        if (**p != '"') { hds_json_free(obj); return NULL; }
        (*p)++;
        char *key = hds_json_parse_string_body(p);
        if (!key) { hds_json_free(obj); return NULL; }
        hds_json_skip_ws(p);
        if (**p != ':') { free(key); hds_json_free(obj); return NULL; }
        (*p)++;
        hds_json_skip_ws(p);
        HdsJsonValue *val = hds_json_parse_value(p);
        if (!val) { free(key); hds_json_free(obj); return NULL; }
        HdsJsonMember *member = calloc(1, sizeof(HdsJsonMember));
        if (!member) { free(key); hds_json_free(val); hds_json_free(obj); return NULL; }
        member->key = key;
        member->value = val;
        *next_slot = member;
        next_slot = &member->next;
        hds_json_skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == '}') { (*p)++; break; }
        hds_json_free(obj);
        return NULL;
    }
    return obj;
}

static HdsJsonValue *hds_json_parse_array(const char **p) {
    HdsJsonValue *arr = hds_json_new_value(HDS_JSON_ARRAY);
    if (!arr) return NULL;
    (*p)++; // '['
    hds_json_skip_ws(p);
    if (**p == ']') { (*p)++; return arr; }
    HdsJsonMember **next_slot = &arr->first_member;
    for (;;) {
        hds_json_skip_ws(p);
        HdsJsonValue *val = hds_json_parse_value(p);
        if (!val) { hds_json_free(arr); return NULL; }
        HdsJsonMember *member = calloc(1, sizeof(HdsJsonMember));
        if (!member) { hds_json_free(val); hds_json_free(arr); return NULL; }
        member->value = val;
        *next_slot = member;
        next_slot = &member->next;
        hds_json_skip_ws(p);
        if (**p == ',') { (*p)++; continue; }
        if (**p == ']') { (*p)++; break; }
        hds_json_free(arr);
        return NULL;
    }
    return arr;
}

static HdsJsonValue *hds_json_parse_value(const char **p) {
    hds_json_skip_ws(p);
    switch (**p) {
        case '{': return hds_json_parse_object(p);
        case '[': return hds_json_parse_array(p);
        case '"': {
            (*p)++;
            char *s = hds_json_parse_string_body(p);
            if (!s) return NULL;
            HdsJsonValue *v = hds_json_new_value(HDS_JSON_STRING);
            if (!v) { free(s); return NULL; }
            v->string = s;
            return v;
        }
        case 't':
            if (strncmp(*p, "true", 4) != 0) return NULL;
            *p += 4;
            { HdsJsonValue *v = hds_json_new_value(HDS_JSON_BOOL); if (v) v->boolean = true; return v; }
        case 'f':
            if (strncmp(*p, "false", 5) != 0) return NULL;
            *p += 5;
            return hds_json_new_value(HDS_JSON_BOOL);
        case 'n':
            if (strncmp(*p, "null", 4) != 0) return NULL;
            *p += 4;
            return hds_json_new_value(HDS_JSON_NULL);
        default: {
            if (**p != '-' && !isdigit((unsigned char)**p)) return NULL;
            char *end = NULL;
            double num = strtod(*p, &end);
            if (end == *p) return NULL;
            *p = end;
            HdsJsonValue *v = hds_json_new_value(HDS_JSON_NUMBER);
            if (v) v->number = num;
            return v;
        }
    }
}

// Top-level entry point: parses a whole JSON document, requiring the entire (whitespace-trimmed)
// input to be consumed -- trailing garbage after a syntactically valid value is still a malformed
// document, not a value to silently truncate to. Returns NULL on any parse failure.
HdsJsonValue *hds_json_parse(const char *text) {
    const char *p = text;
    HdsJsonValue *root = hds_json_parse_value(&p);
    if (!root) return NULL;
    hds_json_skip_ws(&p);
    if (*p != '\0') { hds_json_free(root); return NULL; }
    return root;
}

// Linear scan -- these are small, hand-edited-file-sized objects (a Program's own top-level keys,
// or its "fields"/"sfz_file" sub-objects), never a hot path, so a hash lookup would be pure
// overkill (unlike osc_dispatch_table, which really does need O(1) for every incoming OSC message
// during real-time playback).
HdsJsonValue *hds_json_object_get(const HdsJsonValue *obj, const char *key) {
    if (!obj || obj->type != HDS_JSON_OBJECT) return NULL;
    for (HdsJsonMember *m = obj->first_member; m; m = m->next) {
        if (m->key && strcmp(m->key, key) == 0) return m->value;
    }
    return NULL;
}
