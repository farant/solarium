/* fuzzy.h — case-insensitive subsequence fuzzy matcher for the command palette.
   Pure: depends on the C library only, no engine state. Greedy leftmost match
   (good enough for short command names). See the palette design spec. */
#ifndef SOL_FUZZY_H
#define SOL_FUZZY_H

#include "sol_base.h"

/* Returns SOL_TRUE if every character of `query` appears in `cand` in order
   (case-insensitive). An empty query matches anything with score 0.
   On a match, *out_score (if non-NULL) gets a relevance score (higher = better),
   and out_pos[] (if non-NULL, capacity max_pos) gets the byte index in `cand` of
   each matched query character, in order. NULL query or cand => no match. */
sol_bool fuzzy_match(const char *query, const char *cand,
                     int *out_score, int *out_pos, int max_pos);

#endif /* SOL_FUZZY_H */
