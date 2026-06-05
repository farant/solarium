/* json_test.c — headless checks for the JSON parser: glTF-flavored navigation,
   numbers, escapes incl. \u, and an error case. Built by `build.sh jsontest`. */

#include "json.h"

#include <stdio.h>
#include <string.h>

static int g_fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", (msg)); g_fails++; } } while (0)

static int streq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

int main(void) {
    static const char *src =
        "{\n"
        "  \"asset\": {\"version\": \"2.0\"},\n"
        "  \"meshes\": [ {\"primitives\": [ {\"attributes\": {\"POSITION\": 2}, \"indices\": 3} ]} ],\n"
        "  \"scale\": -1.5e3,\n"
        "  \"flag\": true,\n"
        "  \"empty\": [],\n"
        "  \"name\": \"a\\nb\\u0041\"\n"          /* newline, then 'A' from \\u0041 */
        "}";
    JsonValue *root, *meshes, *prim, *attrs;

    root = json_parse(src);
    if (!root) {
        printf("FAIL parse: line %d: %s\n", json_last_error_line(), json_last_error());
        return 1;
    }

    CHECK(streq(json_string(json_member(json_member(root, "asset"), "version")), "2.0"),
          "asset.version == \"2.0\"");

    meshes = json_member(root, "meshes");
    CHECK(json_count(meshes) == 1, "meshes has 1 entry");

    prim  = json_index(json_member(json_index(meshes, 0), "primitives"), 0);
    attrs = json_member(prim, "attributes");
    CHECK(json_number(json_member(attrs, "POSITION"), -1.0) == 2.0, "attributes.POSITION == 2");
    CHECK(json_number(json_member(prim, "indices"), -1.0) == 3.0, "indices == 3");

    CHECK(json_number(json_member(root, "scale"), 0.0) == -1500.0, "scale == -1.5e3");
    CHECK(json_bool(json_member(root, "flag"), SOL_FALSE) == SOL_TRUE, "flag == true");
    CHECK(json_count(json_member(root, "empty")) == 0, "empty array length 0");
    CHECK(streq(json_string(json_member(root, "name")), "a\nbA"), "escapes + \\u0041 -> \"a\\nbA\"");

    /* absent keys / type mismatches return NULL/default, not crash */
    CHECK(json_member(root, "nope") == NULL, "absent key -> NULL");
    CHECK(json_number(json_member(root, "name"), 7.0) == 7.0, "string-as-number -> default");

    json_free(root);

    {   /* malformed input must be rejected */
        JsonValue *bad = json_parse("{\"a\": }");
        CHECK(bad == NULL, "malformed object rejected");
        if (bad) json_free(bad);
        printf("error path: line %d: %s\n", json_last_error_line(), json_last_error());
    }

    if (g_fails) { printf("json_test: %d FAILURE(S)\n", g_fails); return 1; }
    printf("json_test: OK\n");
    return 0;
}
