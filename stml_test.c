/* stml_test.c — standalone exercise for the STML parser. Parses a scene-shaped
   sample, prints the tree, then checks the error path. Built by `build.sh
   stmltest` with ASan/UBSan; must be sanitizer-clean (frees every tree). */

#include "stml.h"

#include <stdio.h>

static void print_indent(int depth) {
    int i;
    for (i = 0; i < depth; i++) fputs("  ", stdout);
}

static void print_node(const StmlNode *n, int depth) {
    sol_u32 i;

    if (n->tag == NULL) {                    /* document root: just its children */
        for (i = 0; i < n->child_count; i++) print_node(n->children[i], depth);
        return;
    }

    print_indent(depth);
    printf("<%s%s", n->tag, n->raw ? "!" : "");
    for (i = 0; i < n->attr_count; i++) {
        printf(" %s=\"%s\"", n->attrs[i].name, n->attrs[i].value);
    }
    if (n->text != NULL) {
        printf(" (> %s\n", n->text);          /* capture node */
    } else if (n->child_count == 0) {
        printf(" />\n");                       /* leaf */
    } else {
        printf(">\n");
        for (i = 0; i < n->child_count; i++) print_node(n->children[i], depth + 1);
        print_indent(depth);
        printf("</%s>\n", n->tag);
    }
}

int main(void) {
    static const char *sample =
        "<scene version=\"1\">\n"
        "  <object nid=\"01HZ3\" parent=\"\" labels=\"furniture::shelf\">\n"
        "    <pos x=\"0\" y=\"0\" z=\"-2\" />\n"
        "    <rot x=\"0\" y=\"0\" z=\"0\" w=\"1\" />\n"
        "    <scale x=\"1\" y=\"1\" z=\"1\" />\n"
        "    <mesh ref=\"box\" />\n"
        "    <meta key=\"title\" (>Plato's Republic\n"
        "    <!-- comments are skipped -->\n"
        "    <flag pinned hidden />\n"          /* two boolean attributes */
        "    <rel type=\"shelved_on\" target=\"01HZ3\" />\n"
        "  </object>\n"
        "</scene>\n";

    StmlNode *root, *bad;

    root = stml_parse(sample);
    if (!root) {
        printf("parse failed at line %d: %s\n", stml_last_error_line(), stml_last_error());
        return 1;
    }
    printf("--- parsed tree ---\n");
    print_node(root, 0);

    /* spot-check the lookup accessors against the parsed tree */
    {
        StmlNode *scene  = root->children[0];
        StmlNode *object = stml_child(scene, "object");
        printf("--- accessors ---\n");
        printf("scene version = %s\n", stml_attr(scene, "version"));
        printf("object nid    = %s\n", stml_attr(object, "nid"));
        printf("object labels = %s\n", stml_attr(object, "labels"));
    }
    stml_free(root);

    /* anonymous close: `</>` closes the nearest open element, so it can stand
       in for </object> and </scene>. Parse the same shape with `</>` and check
       the nesting came out identical. */
    {
        static const char *anon =
            "<scene>\n"
            "  <object nid=\"K1\">\n"
            "    <pos x=\"1\" y=\"2\" z=\"3\" />\n"
            "  </>\n"                              /* closes <object> */
            "</>\n";                               /* closes <scene>  */
        StmlNode *aroot = stml_parse(anon);
        if (!aroot) {
            printf("FAIL: `</>` parse error at line %d: %s\n",
                   stml_last_error_line(), stml_last_error());
            return 1;
        }
        printf("--- anonymous </> close ---\n");
        {
            StmlNode *scene  = aroot->children[0];
            StmlNode *object = stml_child(scene, "object");
            StmlNode *pos    = object ? stml_child(object, "pos") : NULL;
            printf("scene tag    = %s\n", scene->tag);
            printf("object nid   = %s\n", object ? stml_attr(object, "nid") : "(missing)");
            printf("pos under object: x=%s y=%s z=%s\n",
                   pos ? stml_attr(pos, "x") : "?",
                   pos ? stml_attr(pos, "y") : "?",
                   pos ? stml_attr(pos, "z") : "?");
            if (!object || !pos) { printf("FAIL: `</>` did not nest correctly\n"); stml_free(aroot); return 1; }
        }
        stml_free(aroot);
    }

    /* error path: a mismatched closing tag must be rejected, not parsed */
    bad = stml_parse("<a><b></a>");
    if (bad) {
        printf("FAIL: expected an error for <a><b></a>\n");
        stml_free(bad);
        return 1;
    }
    printf("--- expected error ---\n");
    printf("line %d: %s\n", stml_last_error_line(), stml_last_error());

    printf("stml_test: OK\n");
    return 0;
}
