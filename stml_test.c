/* stml_test.c — standalone exercise for the STML parser. Parses a scene-shaped
   sample, prints the tree, then checks the error path. Built by `build.sh
   stmltest` with ASan/UBSan; must be sanitizer-clean (frees every tree). */

#include "stml.h"

#include <stdio.h>
#include <string.h>

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

    /* raw line capture (<tag! (>): rest of the line VERBATIM — leading and
       trailing spaces survive, '<' and '&' are plain bytes, no dedent. */
    {
        static const char *raw_line =
            "<note! (>  a < b && c >= d  \n"
            "<after />\n";
        StmlNode *r = stml_parse(raw_line);
        const char *want = "  a < b && c >= d  ";
        if (!r) {
            printf("FAIL: raw line parse error at line %d: %s\n",
                   stml_last_error_line(), stml_last_error());
            return 1;
        }
        if (r->child_count != 2 ||
            !r->children[0]->raw ||
            !r->children[0]->text ||
            strcmp(r->children[0]->text, want) != 0) {
            printf("FAIL: raw line capture not verbatim: got [%s]\n",
                   r->children[0]->text ? r->children[0]->text : "(null)");
            stml_free(r);
            return 1;
        }
        printf("raw line capture verbatim (spaces, <, &): ok\n");
        stml_free(r);
    }

    /* raw block (<tag!>...</tag>): byte-for-byte between '>' and "</",
       including newlines and indentation — no trim, no dedent. */
    {
        static const char *raw_block =
            "<code!>if (x < 10) {\n"
            "    y++;\n"
            "}</code>\n";
        StmlNode *r = stml_parse(raw_block);
        const char *want = "if (x < 10) {\n    y++;\n}";
        if (!r || r->child_count != 1 || !r->children[0]->text ||
            strcmp(r->children[0]->text, want) != 0) {
            printf("FAIL: raw block not verbatim: got [%s]\n",
                   (r && r->child_count) ? r->children[0]->text : "(parse error)");
            if (r) stml_free(r);
            return 1;
        }
        printf("raw block verbatim (newlines, indent, <): ok\n");
        stml_free(r);
    }

    /* raw block close is still validated: a wrong name is an error, and an
       unclosed raw element is an error (EOF before "</"). */
    bad = stml_parse("<code!>x</wrong>");
    if (bad) { printf("FAIL: raw block accepted a mismatched close\n"); stml_free(bad); return 1; }
    bad = stml_parse("<code!>never closed");
    if (bad) { printf("FAIL: unclosed raw block accepted\n"); stml_free(bad); return 1; }
    printf("raw block close validation + EOF: ok\n");

    /* attribute escapes: \\ \" \' decode (always on, either quote style);
       a quote of the OTHER kind is a plain byte, no escape needed. */
    {
        static const char *esc =
            "<a one=\"say \\\"hi\\\"\" two='it\"s' three=\"back\\\\slash\" />";
        StmlNode *r = stml_parse(esc);
        StmlNode *a = r ? stml_child(r, "a") : NULL;
        if (!a ||
            strcmp(stml_attr(a, "one"),   "say \"hi\"")  != 0 ||
            strcmp(stml_attr(a, "two"),   "it\"s")       != 0 ||
            strcmp(stml_attr(a, "three"), "back\\slash") != 0) {
            printf("FAIL: attribute escape decode\n");
            if (r) stml_free(r);
            return 1;
        }
        printf("attribute escapes (\\\\ \\\" and quote-selection): ok\n");
        stml_free(r);
    }

    /* unknown escapes are rejected — strictness over silent corruption */
    bad = stml_parse("<a v=\"oops\\x\" />");
    if (bad) { printf("FAIL: unknown escape accepted\n"); stml_free(bad); return 1; }
    printf("unknown escape rejected: line %d: %s\n",
           stml_last_error_line(), stml_last_error());

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
