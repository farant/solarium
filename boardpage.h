#ifndef SOL_BOARDPAGE_H
#define SOL_BOARDPAGE_H

/* A page slug is a flat, board-scoped key: a leading '/' + lowercase
   [a-z0-9-]. PAGE_SLUG_CAP bounds its bytes incl. the '/' and NUL. */
#define PAGE_SLUG_CAP   96
/* Max distinct pages a single board's arrow-cycle enumerates. */
#define BOARD_PAGE_MAX  64

/* Slugify a user-typed page name into out[cap] (NUL-terminated):
   lowercase; every maximal run of non-[a-z0-9] characters (spaces AND
   punctuation, including any typed '/') collapses to a single '-';
   trim leading/trailing '-'; prepend exactly one leading '/'.
   Returns the length written (excl. NUL). A name with no alphanumerics
   yields just "/" (length 1) -- the caller reads that as "cancel". */
int boardpage_slugify(const char *in, char *out, int cap);

/* Build a board's navigable page list from its raw page tags:
   dedupe the n input slugs, always include "/" and `active`, and sort
   "/" first then ascending strcmp, into out[cap][PAGE_SLUG_CAP].
   NULL/empty input slugs are skipped. `active` NULL/"" is treated as "/".
   Returns the row count (<= cap). */
int boardpage_collect(const char *const *pages, int n, const char *active,
                      char out[][PAGE_SLUG_CAP], int cap);

/* Build a board's ordered page list from its stored space-delimited slug list
   (creation order) + the active page: "/" first, then each token in order,
   then `active` if not already present. Dedupes; does NOT sort. Returns count. */
int  boardpage_list(const char *stored, const char *active,
                    char out[][PAGE_SLUG_CAP], int cap);

/* 1 if `slug` is a whole space-delimited token of `list`, else 0 (so "/page-1"
   is NOT contained in "/page-10"). */
int  boardpage_contains(const char *list, const char *slug);

/* Space-join the non-"/" entries of list[0..n) into out[cap] (NUL-terminated,
   truncate-safe). "/" is implicit and skipped. */
void boardpage_serialize(const char list[][PAGE_SLUG_CAP], int n,
                         char *out, int cap);

#endif
