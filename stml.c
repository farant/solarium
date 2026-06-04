/* stml.c — recursive-descent parser for the STML data profile. See stml.h. */

#include "stml.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ errors */
/* First error wins; line is counted from the buffer start on demand. Single
   global state is fine: the parser is synchronous and single-threaded. */
static const char *g_err;
static int         g_err_line;

typedef struct {
    const char *p;       /* cursor */
    const char *start;   /* buffer origin, for line counting */
} Parser;

static void set_err(Parser *ps, const char *msg) {
    const char *c;
    int line;
    if (g_err) return;                 /* keep the earliest error */
    line = 1;
    for (c = ps->start; c < ps->p; c++) {
        if (*c == '\n') line++;
    }
    g_err = msg;
    g_err_line = line;
}

const char *stml_last_error(void)      { return g_err ? g_err : "no error"; }
int         stml_last_error_line(void) { return g_err_line; }

/* ------------------------------------------------------------ small helpers */
static int is_ws(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_name_char(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == ':' || c == '.';
}

static char *dup_range(const char *a, const char *b) {   /* copies [a,b) */
    size_t n = (size_t)(b - a);
    char  *s = (char *)malloc(n + 1);
    if (s) { memcpy(s, a, n); s[n] = '\0'; }
    return s;
}

static char *dup_cstr(const char *s) {
    return dup_range(s, s + strlen(s));
}

/* Smart-whitespace per the profile: trim surrounding blank space and strip the
   common leading indentation shared by all non-blank lines. For the scene's
   single-line capture values this just trims; it also handles multiline text
   for the future content model. Returns a fresh string. */
static char *dedent_range(const char *a, const char *b) {
    const char *p;
    size_t      min_indent;
    int         have_min;
    char       *out, *w;

    while (b > a && is_ws((unsigned char)b[-1])) b--;     /* trim trailing */

    for (;;) {                                            /* drop leading blank lines */
        const char *nl = a;
        while (nl < b && *nl != '\n' && is_ws((unsigned char)*nl)) nl++;
        if (nl < b && *nl == '\n') { a = nl + 1; continue; }
        break;
    }
    if (a >= b) return dup_cstr("");

    have_min = 0;
    min_indent = 0;
    p = a;
    while (p < b) {                                       /* find the common indent */
        const char *ls = p;
        size_t indent = 0;
        while (ls < b && (*ls == ' ' || *ls == '\t')) { ls++; indent++; }
        if (ls < b && *ls != '\n') {                      /* a non-blank line */
            if (!have_min || indent < min_indent) { min_indent = indent; have_min = 1; }
        }
        while (p < b && *p != '\n') p++;
        if (p < b) p++;
    }
    if (!have_min) min_indent = 0;

    out = (char *)malloc((size_t)(b - a) + 1);            /* never grows; safe size */
    if (!out) return NULL;
    w = out;
    p = a;
    while (p < b) {                                       /* emit, indentation stripped */
        size_t skip = min_indent;
        while (p < b && skip > 0 && (*p == ' ' || *p == '\t')) { p++; skip--; }
        while (p < b && *p != '\n') *w++ = *p++;
        if (p < b) { *w++ = '\n'; p++; }
    }
    *w = '\0';
    return out;
}

/* --------------------------------------------------------------- node model */
static StmlNode *node_new(void) {
    return (StmlNode *)calloc(1, sizeof(StmlNode));       /* zero = empty everything */
}

static int node_add_child(StmlNode *parent, StmlNode *child) {
    if (parent->child_count == parent->child_cap) {
        sol_u32    cap   = parent->child_cap ? parent->child_cap * 2 : 4;
        StmlNode **grown = (StmlNode **)realloc(parent->children,
                                                (size_t)cap * sizeof(StmlNode *));
        if (!grown) return 0;
        parent->children  = grown;
        parent->child_cap = cap;
    }
    parent->children[parent->child_count++] = child;
    return 1;
}

static int node_add_attr(StmlNode *n, char *name, char *value) {
    if (n->attr_count == n->attr_cap) {
        sol_u32   cap   = n->attr_cap ? n->attr_cap * 2 : 4;
        StmlAttr *grown = (StmlAttr *)realloc(n->attrs, (size_t)cap * sizeof(StmlAttr));
        if (!grown) return 0;
        n->attrs    = grown;
        n->attr_cap = cap;
    }
    n->attrs[n->attr_count].name  = name;
    n->attrs[n->attr_count].value = value;
    n->attr_count++;
    return 1;
}

void stml_free(StmlNode *node) {
    sol_u32 i;
    if (!node) return;
    for (i = 0; i < node->child_count; i++) stml_free(node->children[i]);
    free(node->children);
    for (i = 0; i < node->attr_count; i++) { free(node->attrs[i].name); free(node->attrs[i].value); }
    free(node->attrs);
    free(node->tag);
    free(node->text);
    free(node);
}

/* --------------------------------------------------------------- the parser */
static StmlNode *parse_element(Parser *ps);
static int       parse_content(Parser *ps, StmlNode *parent);

static void skip_ws(Parser *ps) {
    while (is_ws((unsigned char)*ps->p)) ps->p++;
}

static void skip_ws_and_comments(Parser *ps) {
    for (;;) {
        while (is_ws((unsigned char)*ps->p)) ps->p++;
        if (ps->p[0] == '<' && ps->p[1] == '!' && ps->p[2] == '-' && ps->p[3] == '-') {
            ps->p += 4;
            while (*ps->p != '\0' &&
                   !(ps->p[0] == '-' && ps->p[1] == '-' && ps->p[2] == '>')) ps->p++;
            if (*ps->p != '\0') ps->p += 3;     /* consume "-->" */
            continue;
        }
        break;
    }
}

static int parse_attr(Parser *ps, StmlNode *n) {
    const char *name_start, *name_end;
    char       *name, *value;

    name_start = ps->p;
    while (is_name_char((unsigned char)*ps->p)) ps->p++;
    name_end = ps->p;
    if (name_end == name_start) { set_err(ps, "expected attribute name"); return 0; }
    name = dup_range(name_start, name_end);
    if (!name) { set_err(ps, "out of memory"); return 0; }

    if (*ps->p == '=') {
        const char *val_start, *val_end;
        char        q;
        ps->p++;
        q = *ps->p;
        if (q != '"' && q != '\'') { free(name); set_err(ps, "attribute value must be quoted"); return 0; }
        ps->p++;
        val_start = ps->p;
        while (*ps->p != '\0' && *ps->p != q) ps->p++;
        if (*ps->p != q) { free(name); set_err(ps, "unterminated attribute value"); return 0; }
        val_end = ps->p;
        ps->p++;                                /* consume closing quote */
        value = dup_range(val_start, val_end);
    } else {
        value = dup_cstr("true");               /* boolean attribute */
    }
    if (!value) { free(name); set_err(ps, "out of memory"); return 0; }

    if (!node_add_attr(n, name, value)) {
        free(name); free(value); set_err(ps, "out of memory"); return 0;
    }
    return 1;
}

/* Capture: the element's text is everything up to the next tag, dedented; the
   element self-closes (we leave the cursor ON the next '<'). */
static int parse_capture(Parser *ps, StmlNode *n) {
    const char *t_start = ps->p;
    while (*ps->p != '\0' && *ps->p != '<') ps->p++;
    n->text = dedent_range(t_start, ps->p);
    if (!n->text) { set_err(ps, "out of memory"); return 0; }
    return 1;
}

/* Consume a closing tag for `parent` (cursor at "</"). A named close is
   validated against the open element; `</>` auto-closes whatever is open. */
static int parse_close(Parser *ps, StmlNode *parent) {
    const char *name_start, *name_end;
    ps->p += 2;                                 /* consume "</" */
    skip_ws(ps);
    name_start = ps->p;
    while (is_name_char((unsigned char)*ps->p)) ps->p++;
    name_end = ps->p;
    skip_ws(ps);
    if (*ps->p != '>') { set_err(ps, "malformed closing tag"); return 0; }
    ps->p++;                                    /* consume '>' */

    if (parent->tag == NULL) { set_err(ps, "stray closing tag at top level"); return 0; }
    if (name_end > name_start) {                /* named: must match the open tag */
        size_t len = (size_t)(name_end - name_start);
        if (strlen(parent->tag) != len || strncmp(parent->tag, name_start, len) != 0) {
            set_err(ps, "closing tag does not match open element");
            return 0;
        }
    }
    return 1;
}

/* Parse a run of children into `parent`, stopping at EOF (top level) or the
   closing tag that matches `parent` (which it consumes). */
static int parse_content(Parser *ps, StmlNode *parent) {
    for (;;) {
        StmlNode *child;
        skip_ws_and_comments(ps);
        if (*ps->p == '\0') {
            if (parent->tag != NULL) { set_err(ps, "unexpected end of input: unclosed element"); return 0; }
            return 1;                           /* clean EOF at the document root */
        }
        if (ps->p[0] == '<' && ps->p[1] == '/') return parse_close(ps, parent);
        if (*ps->p != '<') { set_err(ps, "unexpected text outside an element"); return 0; }

        child = parse_element(ps);
        if (!child) return 0;
        if (!node_add_child(parent, child)) { stml_free(child); set_err(ps, "out of memory"); return 0; }
    }
}

/* Parse one element (cursor at '<', not a comment, not a close). */
static StmlNode *parse_element(Parser *ps) {
    StmlNode   *n;
    const char *name_start, *name_end;

    n = node_new();
    if (!n) { set_err(ps, "out of memory"); return NULL; }

    ps->p++;                                    /* consume '<' */
    name_start = ps->p;
    while (is_name_char((unsigned char)*ps->p)) ps->p++;
    name_end = ps->p;
    if (name_end == name_start) { set_err(ps, "empty tag name"); stml_free(n); return NULL; }
    if (*ps->p == '!') { n->raw = SOL_TRUE; ps->p++; }   /* raw marker follows the name */
    n->tag = dup_range(name_start, name_end);
    if (!n->tag) { set_err(ps, "out of memory"); stml_free(n); return NULL; }

    for (;;) {                                  /* attributes */
        skip_ws(ps);
        if (*ps->p == '>' || *ps->p == '/' || *ps->p == '(' || *ps->p == '\0') break;
        if (!parse_attr(ps, n)) { stml_free(n); return NULL; }
    }

    if (*ps->p == '/') {                         /* self-closing leaf */
        ps->p++;
        if (*ps->p != '>') { set_err(ps, "expected '>' after '/'"); stml_free(n); return NULL; }
        ps->p++;
        return n;
    }
    if (*ps->p == '(') {                          /* forward text capture */
        ps->p++;
        if (*ps->p != '>') { set_err(ps, "expected '>' after '(' capture"); stml_free(n); return NULL; }
        ps->p++;
        if (!parse_capture(ps, n)) { stml_free(n); return NULL; }
        return n;
    }
    if (*ps->p == '>') {                          /* open: parse children */
        ps->p++;
        if (!parse_content(ps, n)) { stml_free(n); return NULL; }
        return n;
    }
    set_err(ps, "unterminated tag");
    stml_free(n);
    return NULL;
}

StmlNode *stml_parse(const char *src) {
    Parser    ps;
    StmlNode *root;

    g_err = NULL;
    g_err_line = 0;
    ps.p     = src;
    ps.start = src;

    root = node_new();
    if (!root) { set_err(&ps, "out of memory"); return NULL; }
    if (!parse_content(&ps, root)) { stml_free(root); return NULL; }
    return root;
}

/* --------------------------------------------------------------- accessors */
const char *stml_attr(const StmlNode *node, const char *name) {
    sol_u32 i;
    for (i = 0; i < node->attr_count; i++) {
        if (strcmp(node->attrs[i].name, name) == 0) return node->attrs[i].value;
    }
    return NULL;
}

StmlNode *stml_child(const StmlNode *node, const char *tag) {
    sol_u32 i;
    for (i = 0; i < node->child_count; i++) {
        if (node->children[i]->tag && strcmp(node->children[i]->tag, tag) == 0) {
            return node->children[i];
        }
    }
    return NULL;
}
