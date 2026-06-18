/* fuzzy.c — case-insensitive subsequence fuzzy matcher for the command palette.
   Pure: depends on the C library only. Greedy leftmost match; scoring rewards
   start-of-string and word-boundary hits and contiguous runs, penalises gaps. */
#include "fuzzy.h"

#include <stddef.h>
#include <ctype.h>

#define FUZZY_BONUS_FIRST     12   /* match at the very first char of cand */
#define FUZZY_BONUS_BOUNDARY   8   /* match right after a separator (word start) */
#define FUZZY_BONUS_CONTIG     5   /* match directly follows the previous match */
#define FUZZY_PENALTY_GAP      1   /* per skipped char between matches */

static int fuzzy_is_sep(int c) {
    return c == ' ' || c == '-' || c == '_' || c == '/' || c == '.';
}

static int fuzzy_lower(int c) {
    return tolower((unsigned char)c);
}

sol_bool fuzzy_match(const char *query, const char *cand,
                     int *out_score, int *out_pos, int max_pos) {
    int qi, ci, score, npos, prev;

    if (query == NULL || cand == NULL) {
        if (out_score) *out_score = 0;
        return SOL_FALSE;
    }

    qi = 0; ci = 0; score = 0; npos = 0; prev = -1;

    while (query[qi] != '\0') {
        while (cand[ci] != '\0' && fuzzy_lower(cand[ci]) != fuzzy_lower(query[qi]))
            ci++;
        if (cand[ci] == '\0') {                 /* ran out: not a subsequence */
            if (out_score) *out_score = 0;
            return SOL_FALSE;
        }
        if (ci == 0)
            score += FUZZY_BONUS_FIRST;
        else if (fuzzy_is_sep((unsigned char)cand[ci - 1]))
            score += FUZZY_BONUS_BOUNDARY;
        if (prev >= 0) {
            if (ci == prev + 1) score += FUZZY_BONUS_CONTIG;
            else                score -= FUZZY_PENALTY_GAP * (ci - prev - 1);
        }
        if (out_pos != NULL && npos < max_pos)
            out_pos[npos] = ci;
        npos++;
        prev = ci;
        ci++;
        qi++;
    }

    if (out_score != NULL) *out_score = score;
    return SOL_TRUE;
}
