/* json.c — recursive-descent JSON parser. See json.h. */

#include "json.h"

#include <stdlib.h>   /* malloc, calloc, realloc, free, strtod */
#include <string.h>   /* strcmp, strncmp */

/* ------------------------------------------------------------------ errors */
static const char *g_err;
static int         g_err_line;

typedef struct {
    const char *p;
    const char *start;
} Parser;

static void set_err(Parser *ps, const char *msg) {
    const char *c;
    int line;
    if (g_err) return;                     /* first error wins */
    line = 1;
    for (c = ps->start; c < ps->p; c++) {
        if (*c == '\n') line++;
    }
    g_err = msg;
    g_err_line = line;
}

const char *json_last_error(void)      { return g_err ? g_err : "no error"; }
int         json_last_error_line(void) { return g_err_line; }

/* ------------------------------------------------------------- small helpers */
static void skip_ws(Parser *ps) {
    while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r') ps->p++;
}

static JsonValue *value_new(JsonType t) {
    JsonValue *v = (JsonValue *)calloc(1, sizeof(JsonValue));   /* zero -> empty union */
    if (v) v->type = t;
    return v;
}

static int hex_digit(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Encode a BMP code point as UTF-8 (1-3 bytes). Surrogate halves are emitted
   as-is — glTF strings don't use them, and this keeps it simple. */
static char *utf8_encode(unsigned int cp, char *w) {
    if (cp < 0x80) {
        *w++ = (char)cp;
    } else if (cp < 0x800) {
        *w++ = (char)(0xC0 | (cp >> 6));
        *w++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *w++ = (char)(0xE0 | (cp >> 12));
        *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *w++ = (char)(0x80 | (cp & 0x3F));
    }
    return w;
}

/* Parse a "..." string into a fresh, decoded, NUL-terminated buffer (cursor at
   the opening quote). Used for both string values and object keys. */
static char *parse_raw_string(Parser *ps) {
    const char *s, *q;
    char  *out, *w;
    if (*ps->p != '"') { set_err(ps, "expected a string"); return NULL; }
    ps->p++;                                /* consume opening quote */
    s = ps->p;

    q = s;                                  /* find the closing quote, skipping escapes */
    while (*q != '\0' && *q != '"') {
        if (*q == '\\' && q[1] != '\0') q += 2;
        else q++;
    }
    if (*q != '"') { set_err(ps, "unterminated string"); return NULL; }

    out = (char *)malloc((size_t)(q - s) + 1);   /* decoded length <= raw span */
    if (!out) { set_err(ps, "out of memory"); return NULL; }
    w = out;
    while (ps->p < q) {
        if (*ps->p != '\\') { *w++ = *ps->p++; continue; }
        ps->p++;                            /* consume backslash */
        switch (*ps->p) {
            case '"':  *w++ = '"';  ps->p++; break;
            case '\\': *w++ = '\\'; ps->p++; break;
            case '/':  *w++ = '/';  ps->p++; break;
            case 'b':  *w++ = '\b'; ps->p++; break;
            case 'f':  *w++ = '\f'; ps->p++; break;
            case 'n':  *w++ = '\n'; ps->p++; break;
            case 'r':  *w++ = '\r'; ps->p++; break;
            case 't':  *w++ = '\t'; ps->p++; break;
            case 'u': {
                unsigned int cp = 0;
                int i;
                ps->p++;                    /* consume 'u' */
                for (i = 0; i < 4; i++) {
                    int h = hex_digit((unsigned char)*ps->p);
                    if (h < 0) { free(out); set_err(ps, "bad \\u escape"); return NULL; }
                    cp = cp * 16u + (unsigned int)h;
                    ps->p++;
                }
                w = utf8_encode(cp, w);
                break;
            }
            default: free(out); set_err(ps, "bad escape"); return NULL;
        }
    }
    *w = '\0';
    ps->p = q + 1;                          /* consume closing quote */
    return out;
}

/* --------------------------------------------------------------- the parser */
static JsonValue *parse_value(Parser *ps);

static JsonValue *parse_number(Parser *ps) {
    char      *end;
    double     d;
    JsonValue *v;
    d = strtod(ps->p, &end);                /* accepts JSON's sign/fraction/exponent */
    if (end == ps->p) { set_err(ps, "invalid number"); return NULL; }
    ps->p = end;
    v = value_new(JSON_NUMBER);
    if (!v) { set_err(ps, "out of memory"); return NULL; }
    v->u.number = d;
    return v;
}

static JsonValue *parse_literal(Parser *ps) {
    JsonValue *v;
    if (strncmp(ps->p, "true", 4) == 0) {
        ps->p += 4; v = value_new(JSON_BOOL);
        if (v) v->u.boolean = SOL_TRUE; else set_err(ps, "out of memory");
        return v;
    }
    if (strncmp(ps->p, "false", 5) == 0) {
        ps->p += 5; v = value_new(JSON_BOOL);
        if (v) v->u.boolean = SOL_FALSE; else set_err(ps, "out of memory");
        return v;
    }
    if (strncmp(ps->p, "null", 4) == 0) {
        ps->p += 4; v = value_new(JSON_NULL);
        if (!v) set_err(ps, "out of memory");
        return v;
    }
    set_err(ps, "invalid JSON value");
    return NULL;
}

static JsonValue *parse_array(Parser *ps) {
    JsonValue *v = value_new(JSON_ARRAY);
    sol_u32    cap = 0;
    if (!v) { set_err(ps, "out of memory"); return NULL; }
    ps->p++;                                /* consume '[' */
    skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return v; }
    for (;;) {
        JsonValue *item = parse_value(ps);
        if (!item) { json_free(v); return NULL; }
        if (v->u.array.count == cap) {
            JsonValue **grown;
            cap = cap ? cap * 2 : 4;
            grown = (JsonValue **)realloc(v->u.array.items, (size_t)cap * sizeof(JsonValue *));
            if (!grown) { json_free(item); json_free(v); set_err(ps, "out of memory"); return NULL; }
            v->u.array.items = grown;
        }
        v->u.array.items[v->u.array.count++] = item;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == ']') { ps->p++; return v; }
        json_free(v); set_err(ps, "expected ',' or ']' in array"); return NULL;
    }
}

static JsonValue *parse_object(Parser *ps) {
    JsonValue *v = value_new(JSON_OBJECT);
    sol_u32    cap = 0;
    if (!v) { set_err(ps, "out of memory"); return NULL; }
    ps->p++;                                /* consume '{' */
    skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return v; }
    for (;;) {
        char      *key;
        JsonValue *val;
        skip_ws(ps);
        key = parse_raw_string(ps);
        if (!key) { json_free(v); return NULL; }
        skip_ws(ps);
        if (*ps->p != ':') { free(key); json_free(v); set_err(ps, "expected ':' after key"); return NULL; }
        ps->p++;
        val = parse_value(ps);
        if (!val) { free(key); json_free(v); return NULL; }
        if (v->u.object.count == cap) {
            JsonMember *grown;
            cap = cap ? cap * 2 : 4;
            grown = (JsonMember *)realloc(v->u.object.members, (size_t)cap * sizeof(JsonMember));
            if (!grown) { free(key); json_free(val); json_free(v); set_err(ps, "out of memory"); return NULL; }
            v->u.object.members = grown;
        }
        v->u.object.members[v->u.object.count].key   = key;
        v->u.object.members[v->u.object.count].value = val;
        v->u.object.count++;
        skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; return v; }
        json_free(v); set_err(ps, "expected ',' or '}' in object"); return NULL;
    }
}

static JsonValue *parse_value(Parser *ps) {
    skip_ws(ps);
    switch (*ps->p) {
        case '{': return parse_object(ps);
        case '[': return parse_array(ps);
        case '"': {
            JsonValue *v;
            char *s = parse_raw_string(ps);
            if (!s) return NULL;
            v = value_new(JSON_STRING);
            if (!v) { free(s); set_err(ps, "out of memory"); return NULL; }
            v->u.string = s;
            return v;
        }
        case 't': case 'f': case 'n': return parse_literal(ps);
        case '-': case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            return parse_number(ps);
        case '\0': set_err(ps, "unexpected end of input"); return NULL;
        default:   set_err(ps, "unexpected character"); return NULL;
    }
}

JsonValue *json_parse(const char *src) {
    Parser     ps;
    JsonValue *v;
    g_err = NULL;
    g_err_line = 0;
    ps.p     = src;
    ps.start = src;
    v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (*ps.p != '\0') {                     /* one value, then EOF */
        set_err(&ps, "trailing characters after the JSON value");
        json_free(v);
        return NULL;
    }
    return v;
}

void json_free(JsonValue *v) {
    sol_u32 i;
    if (!v) return;
    switch (v->type) {
        case JSON_STRING:
            free(v->u.string);
            break;
        case JSON_ARRAY:
            for (i = 0; i < v->u.array.count; i++) json_free(v->u.array.items[i]);
            free(v->u.array.items);
            break;
        case JSON_OBJECT:
            for (i = 0; i < v->u.object.count; i++) {
                free(v->u.object.members[i].key);
                json_free(v->u.object.members[i].value);
            }
            free(v->u.object.members);
            break;
        default:
            break;
    }
    free(v);
}

/* --------------------------------------------------------------- navigation */
JsonValue *json_member(const JsonValue *obj, const char *key) {
    sol_u32 i;
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.members[i].key, key) == 0) return obj->u.object.members[i].value;
    }
    return NULL;
}

JsonValue *json_index(const JsonValue *arr, sol_u32 i) {
    if (!arr || arr->type != JSON_ARRAY || i >= arr->u.array.count) return NULL;
    return arr->u.array.items[i];
}

sol_u32 json_count(const JsonValue *v) {
    if (!v) return 0;
    if (v->type == JSON_ARRAY)  return v->u.array.count;
    if (v->type == JSON_OBJECT) return v->u.object.count;
    return 0;
}

const char *json_string(const JsonValue *v) {
    if (!v || v->type != JSON_STRING) return NULL;
    return v->u.string;
}

double json_number(const JsonValue *v, double dflt) {
    if (!v || v->type != JSON_NUMBER) return dflt;
    return v->u.number;
}

sol_bool json_bool(const JsonValue *v, sol_bool dflt) {
    if (!v || v->type != JSON_BOOL) return dflt;
    return v->u.boolean;
}
