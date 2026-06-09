/* stml.h — a tiny STML parser producing a generic DOM tree. Scene-agnostic:
   this module knows nothing about nid/pos/object — the scene loader is just its
   first client. Supports the "STML data profile" (see SCENE_FORMAT.md):
   elements, self-closing, `</>` auto-close, quoted + boolean attributes
   (backslash escapes \\ \" \' always decoded inside quotes; `&` is NEVER
   special — reserved for the future &name; entity reference), `<tag (>`
   forward text capture (trimmed/dedented), raw mode `!` (`<tag! (>` = rest of
   line verbatim; `<tag!>...</tag>` = verbatim block, terminator "</"),
   comments. Above every seam: depends on the C library only, never GL. */
#ifndef STML_H
#define STML_H

#include "sol_base.h"

typedef struct {
    char *name;
    char *value;   /* boolean attribute -> "true"; never NULL for a present attr */
} StmlAttr;

/* One node in the DOM. Children are an array of POINTERS, not inline structs:
   growing the array must not move the nodes (we hold pointers to them mid-parse,
   and references resolve against them later). */
typedef struct StmlNode {
    char             *tag;          /* element name; NULL only for the document root */
    StmlAttr         *attrs;
    sol_u32           attr_count;
    sol_u32           attr_cap;
    struct StmlNode **children;
    sol_u32           child_count;
    sol_u32           child_cap;
    char             *text;         /* text content (dedented; VERBATIM if raw); NULL if none */
    sol_bool          raw;          /* tag carried the `!` raw marker */
} StmlNode;

/* Parse NUL-terminated STML source into a document tree. Returns a synthetic
   root node (tag == NULL) whose children are the top-level elements, or NULL on
   a structural error (inspect stml_last_error / stml_last_error_line). The
   caller owns the tree and must release it with stml_free. */
StmlNode *stml_parse(const char *src);

/* Recursively free a tree from stml_parse. NULL-safe. */
void stml_free(StmlNode *root);

/* Convenience lookups (linear scans; the DOM is small). */
const char *stml_attr(const StmlNode *node, const char *name);  /* NULL if absent */
StmlNode   *stml_child(const StmlNode *node, const char *tag);   /* first child so tagged */

/* Diagnostics for the most recent stml_parse that returned NULL. */
const char *stml_last_error(void);
int         stml_last_error_line(void);   /* 1-based */

#endif /* STML_H */
