/* json.h — a small recursive-descent JSON parser producing a typed DOM. Built
   for the glTF (.glb) loader (item 6), but general. Strict C89, no deps. Sibling
   in spirit to stml.c. */
#ifndef JSON_H
#define JSON_H

#include "sol_base.h"

typedef enum {
    JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT
} JsonType;

typedef struct JsonValue  JsonValue;
typedef struct { char *key; JsonValue *value; } JsonMember;

struct JsonValue {
    JsonType type;
    union {
        sol_bool boolean;                                     /* JSON_BOOL   */
        double   number;                                      /* JSON_NUMBER (all numbers are doubles) */
        char    *string;                                      /* JSON_STRING */
        struct { JsonValue **items;   sol_u32 count; } array;   /* JSON_ARRAY  */
        struct { JsonMember *members; sol_u32 count; } object;  /* JSON_OBJECT */
    } u;
};

JsonValue *json_parse(const char *src);     /* NULL on error (see json_last_error) */
void       json_free(JsonValue *v);

/* navigation (NULL/default on type mismatch or absence) */
JsonValue  *json_member(const JsonValue *obj, const char *key);
JsonValue  *json_index (const JsonValue *arr, sol_u32 i);
sol_u32     json_count (const JsonValue *v);                 /* array/object length, else 0 */
const char *json_string(const JsonValue *v);
double      json_number(const JsonValue *v, double dflt);
sol_bool    json_bool  (const JsonValue *v, sol_bool dflt);

const char *json_last_error(void);
int         json_last_error_line(void);     /* 1-based */

#endif /* JSON_H */
