#ifndef BROWSER_H
#define BROWSER_H
/* browser.h — pure Miller-columns navigation state for the entity browser HUD.
   No GL, no scene, no engine deps (only fuzzy.h, also pure). The shell feeds
   per-column row counts; this owns focus, per-column selection, and per-column
   filter strings, plus a fuzzy rank helper. Two modes: NAV (hjkl/arrows move,
   '/' enters filter) and FILTER (typing edits the focused column's filter). */
#include "sol_base.h"

#define BROWSER_COLS       3      /* 0 = Types, 1 = Entities, 2 = Commands */
#define BROWSER_FILTER_CAP 64
#define BROWSER_MAX_ITEMS  2048   /* max entities a column enumerates/ranks (e.g. a big image library) */

typedef enum {
    BROWSER_KEY_NONE = 0,
    BROWSER_KEY_LEFT, BROWSER_KEY_RIGHT,   /* change focused column */
    BROWSER_KEY_UP,   BROWSER_KEY_DOWN,    /* move selection in focused column */
    BROWSER_KEY_ENTER,                     /* nav: descend/activate; filter: commit + leave */
    BROWSER_KEY_BACKSPACE,                 /* filter: trim */
    BROWSER_KEY_FILTER,                    /* nav: '/' -> enter filter mode */
    BROWSER_KEY_CANCEL                     /* nav: close; filter: cancel filter + leave */
} BrowserKey;

typedef enum { BROWSER_NONE = 0, BROWSER_ACTIVATE, BROWSER_CLOSE } BrowserAction;

typedef struct {
    int      focus;                                   /* 0..BROWSER_COLS-1 */
    int      sel[BROWSER_COLS];                        /* selected row per column */
    char     filter[BROWSER_COLS][BROWSER_FILTER_CAP];
    int      flen[BROWSER_COLS];
    sol_bool filtering;                               /* true = typing edits the focused filter */
} Browser;

void          browser_reset(Browser *b);
/* clamp every selection into [0, counts[col]-1] (0 when empty). */
void          browser_clamp(Browser *b, const int counts[BROWSER_COLS]);
/* one nav/edit key; counts = current visible row counts per column. */
BrowserAction browser_key(Browser *b, BrowserKey k, const int counts[BROWSER_COLS]);
/* append a printable char to the focused filter (only while filtering). */
void          browser_char(Browser *b, char c);
/* fuzzy-rank `names` (n) against `filter`: write winning indices into out[<=cap],
   return match count. Mirrors palette_rank (stable, score-desc). Cap 256 cands. */
int           browser_rank(const char *filter, const char *const *names, int n,
                           int *out, int cap);
#endif /* BROWSER_H */
